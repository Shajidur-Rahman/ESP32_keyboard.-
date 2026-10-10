/**
 * @file sdio_slave_task.c
 * @brief SDIO slave receiver for ESP32-C6.
 *
 * Receives bridge_pkt_t packets sent by the ESP32-P4 SDIO master
 * and forwards them to the BLE HID task via a FreeRTOS queue.
 *
 * The ESP32-C6 SDIO slave uses the standard esp-idf sdio_slave driver
 * which exposes Function-1 CMD53 transfers.
 *
 * Receive buffers are pre-allocated and recycled to avoid malloc in ISR.
 *
 * SDIO Slave GPIO (ESP32-C6 fixed pins – not remappable):
 *   CLK  → GPIO19
 *   CMD  → GPIO18
 *   D0   → GPIO20
 *   D1   → GPIO21
 *   D2   → GPIO22
 *   D3   → GPIO23
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/sdio_slave.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"

#include "sdio_slave_task.h"
#include "bridge_protocol.h"

static const char *TAG = "sdio_slave";

/* ─── Buffer pool ──────────────────────────────────────────────────
 * NUM_RX_BUFS receive buffers of SDIO_SLAVE_BUF_SIZE bytes each.
 * Must be DMA-capable and aligned to 4 bytes.                       */
#define NUM_RX_BUFS       8
#define SDIO_SLAVE_BUF_SZ (BRIDGE_PKT_SIZE)  /* exactly one packet */

static uint8_t s_rx_bufs[NUM_RX_BUFS][SDIO_SLAVE_BUF_SZ] __attribute__((aligned(4)));

static QueueHandle_t g_report_queue = NULL;
static TaskHandle_t  g_slave_task   = NULL;

/* ─── Init ─────────────────────────────────────────────────────────── */

static esp_err_t sdio_slave_init(void)
{
    sdio_slave_config_t slave_cfg = {
        .sending_mode       = SDIO_SLAVE_SEND_STREAM,
        .send_queue_size    = 8,
        .recv_buffer_size   = SDIO_SLAVE_BUF_SZ,
        .event_cb           = NULL,
    };
    ESP_RETURN_ON_ERROR(sdio_slave_initialize(&slave_cfg),
                        TAG, "sdio_slave_initialize failed");

    /* Register and load all receive buffers with the driver */
    for (int i = 0; i < NUM_RX_BUFS; i++) {
        sdio_slave_buf_handle_t handle = sdio_slave_recv_register_buf(s_rx_bufs[i]);
        ESP_RETURN_ON_FALSE(handle != NULL, ESP_FAIL, TAG, "sdio_slave_recv_register_buf failed (buf %d)", i);
        ESP_RETURN_ON_ERROR(sdio_slave_recv_load_buf(handle), TAG, "sdio_slave_recv_load_buf failed (buf %d)", i);
    }

    ESP_RETURN_ON_ERROR(sdio_slave_start(), TAG, "sdio_slave_start failed");
    ESP_LOGI(TAG, "SDIO slave started – waiting for master (P4)");
    return ESP_OK;
}

/* ─── Receive task ─────────────────────────────────────────────────── */

static void sdio_slave_receive_task(void *arg)
{
    ESP_ERROR_CHECK(sdio_slave_init());

    uint8_t          *buf       = NULL;
    size_t            recv_len  = 0;
    sdio_slave_buf_handle_t buf_handle = NULL;

    while (1) {
        /* Block until a packet arrives from the P4 master */
        esp_err_t err = sdio_slave_recv(&buf_handle, &buf, &recv_len,
                                        portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "sdio_slave_recv error: 0x%x", err);
            continue;
        }

        if (recv_len >= sizeof(bridge_pkt_t)) {
            const bridge_pkt_t *pkt = (const bridge_pkt_t *)buf;

            if (pkt->magic == BRIDGE_MAGIC) {
                bridge_pkt_t local_pkt;
                memcpy(&local_pkt, pkt, sizeof(bridge_pkt_t));

                if (xQueueSend(g_report_queue, &local_pkt, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "Report queue full – drop");
                }
            } else {
                ESP_LOGW(TAG, "Bad magic 0x%02X – dropping packet", pkt->magic);
            }
        }

        /* Return buffer to driver for reuse */
        sdio_slave_recv_load_buf(buf_handle);
    }
}

/* ─── Public API ─────────────────────────────────────────────────── */

esp_err_t sdio_slave_task_start(QueueHandle_t report_queue)
{
    ESP_RETURN_ON_FALSE(report_queue != NULL, ESP_ERR_INVALID_ARG,
                        TAG, "report_queue is NULL");
    g_report_queue = report_queue;

    BaseType_t ret = xTaskCreate(sdio_slave_receive_task, "sdio_slave",
                                 4096, NULL, 7, &g_slave_task);
    ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_FAIL, TAG,
                        "failed to create sdio_slave_receive_task");

    ESP_LOGI(TAG, "SDIO slave task started");
    return ESP_OK;
}

void sdio_slave_task_stop(void)
{
    if (g_slave_task) {
        vTaskDelete(g_slave_task);
        g_slave_task = NULL;
    }
    sdio_slave_stop();
    sdio_slave_deinit();
}
