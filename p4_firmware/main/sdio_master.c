/**
 * @file sdio_master.c
 * @brief SDIO master driver – ESP32-P4 side.
 *
 * Reads bridge_pkt_t items from the shared report queue and writes them
 * to the ESP32-C6 SDIO slave over the internal 4-bit SDIO bus.
 *
 * Pin mapping (Waveshare ESP32-P4 ↔ ESP32-C6 SDIO):
 * ┌──────────────┬──────┬──────────────────────────┐
 * │ Signal       │ GPIO │ Notes                    │
 * ├──────────────┼──────┼──────────────────────────┤
 * │ CLK          │  18  │ SDIO clock               │
 * │ CMD (DAT3)   │  19  │ SDIO command / data3     │
 * │ DAT0         │  14  │ SDIO data 0              │
 * │ DAT1         │  15  │ SDIO data 1              │
 * │ DAT2         │  16  │ SDIO data 2              │
 * │ DAT3         │  17  │ SDIO data 3              │
 * └──────────────┴──────┴──────────────────────────┘
 *
 * Adjust GPIO numbers in SDIO_CLK_GPIO / DATA0_GPIO etc. below if your
 * board variant differs.
 *
 * Protocol:
 *   - Function 1 is used for data (ESP SDIO slave convention)
 *   - Each BRIDGE_PKT_SIZE-byte packet is written via esp_slave_send_packet()
 *   - The C6 slave buffers arrive in its CMD53 receive FIFO
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_serial_slave_link/essl_sdio.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"

#include "sdio_master.h"
#include "bridge_protocol.h"

static const char *TAG = "sdio_master";

/* ─── GPIO configuration ─────────────────────────────────────────── */
/* Adjust these if your Waveshare board routes SDIO to different pins */
#define SDIO_CLK_GPIO   18
#define SDIO_CMD_GPIO   19
#define SDIO_D0_GPIO    14
#define SDIO_D1_GPIO    15
#define SDIO_D2_GPIO    16
#define SDIO_D3_GPIO    17

/* ─── Internal state ─────────────────────────────────────────────── */
static QueueHandle_t     g_report_queue = NULL;
static TaskHandle_t      g_sdio_task    = NULL;
static essl_handle_t     g_essl         = NULL;
static sdmmc_card_t     *g_card         = NULL;

/* Timeout waiting for C6 slave to be ready (ms) */
#define SLAVE_READY_TIMEOUT_MS 10000

/* ─── SDIO initialisation ────────────────────────────────────────── */

static esp_err_t sdio_master_init(void)
{
    /* Configure SDMMC host for 4-bit SDIO at 20 MHz */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot         = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED; /* 20000 kHz */
    host.flags        = SDMMC_HOST_FLAG_4BIT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk   = SDIO_CLK_GPIO;
    slot.cmd   = SDIO_CMD_GPIO;
    slot.d0    = SDIO_D0_GPIO;
    slot.d1    = SDIO_D1_GPIO;
    slot.d2    = SDIO_D2_GPIO;
    slot.d3    = SDIO_D3_GPIO;
    slot.width = 4;
    /* No CD/WP pins on internal bus */
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_RETURN_ON_ERROR(sdmmc_host_init(), TAG, "sdmmc_host_init failed");
    ESP_RETURN_ON_ERROR(sdmmc_host_init_slot(host.slot, &slot),
                        TAG, "sdmmc_host_init_slot failed");

    /* Probe the slave */
    g_card = (sdmmc_card_t *)heap_caps_malloc(sizeof(sdmmc_card_t),
                                              MALLOC_CAP_DEFAULT);
    ESP_RETURN_ON_FALSE(g_card, ESP_ERR_NO_MEM, TAG, "OOM for sdmmc_card_t");

    esp_err_t err = sdmmc_card_init(&host, g_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_card_init failed (0x%x) – is C6 running?", err);
        heap_caps_free(g_card);
        g_card = NULL;
        return err;
    }

    /* Initialise the ESSL (ESP Serial Slave Link) SDIO layer */
    essl_sdio_config_t essl_cfg = {
        .card    = g_card,
        .recv_buffer_size = BRIDGE_PKT_SIZE * 16,  /* C6 slave buffer size */
    };
    ESP_RETURN_ON_ERROR(essl_sdio_init_dev(&g_essl, &essl_cfg),
                        TAG, "essl_sdio_init_dev failed");
    ESP_RETURN_ON_ERROR(essl_init(g_essl, portMAX_DELAY),
                        TAG, "essl_init failed");

    ESP_LOGI(TAG, "SDIO master initialised – slave ready");
    return ESP_OK;
}

/* ─── Forwarding task ────────────────────────────────────────────── */

static void sdio_master_task(void *arg)
{
    /* Retry SDIO init until C6 is ready (C6 may boot slower) */
    esp_err_t err;
    uint32_t  retry = 0;
    do {
        err = sdio_master_init();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SDIO init attempt %lu failed – retry in 500ms", ++retry);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    } while (err != ESP_OK &&
             (retry * 500) < SLAVE_READY_TIMEOUT_MS);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SDIO slave not found after %lu ms – task exiting",
                 (unsigned long)SLAVE_READY_TIMEOUT_MS);
        vTaskDelete(NULL);
        return;
    }

    bridge_pkt_t pkt;
    while (1) {
        /* Block until a report arrives (up to 100 ms, then send keepalive) */
        if (xQueueReceive(g_report_queue, &pkt, pdMS_TO_TICKS(100)) == pdTRUE) {
            /* Transmit over SDIO */
            err = essl_send_packet(g_essl, &pkt, sizeof(pkt), pdMS_TO_TICKS(10));
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "essl_send_packet failed: 0x%x", err);
            }
        }

        /* Poll slave TX credit registers to maintain link health */
        uint32_t tx_buf_num = 0;
        essl_get_tx_buffer_num(g_essl, &tx_buf_num, 0);
    }
}

/* ─── Public API ──────────────────────────────────────────────────── */

esp_err_t sdio_master_start(QueueHandle_t report_queue)
{
    ESP_RETURN_ON_FALSE(report_queue != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "report_queue is NULL");
    g_report_queue = report_queue;

    BaseType_t ret = xTaskCreatePinnedToCore(sdio_master_task, "sdio_master",
                                             6144, NULL, 6,
                                             &g_sdio_task, 1); /* Core 1 */
    ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_FAIL, TAG,
                        "failed to create sdio_master_task");

    ESP_LOGI(TAG, "SDIO master task started on Core 1");
    return ESP_OK;
}

void sdio_master_stop(void)
{
    if (g_sdio_task) {
        vTaskDelete(g_sdio_task);
        g_sdio_task = NULL;
    }
    if (g_essl) {
        essl_sdio_deinit_dev(g_essl);
        g_essl = NULL;
    }
    if (g_card) {
        heap_caps_free(g_card);
        g_card = NULL;
    }
}
