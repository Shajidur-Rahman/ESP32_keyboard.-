/**
 * @file main.c  (ESP32-C6 firmware)
 * @brief Entry point for the BLE HID bridge on ESP32-C6.
 *
 * Pipeline:
 *   SDIO slave ──►[report_queue]──► BLE HID notify task ──► PC/laptop
 */

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_err.h"

#include "sdio_slave_task.h"
#include "ble_hid.h"
#include "bridge_protocol.h"

static const char *TAG = "main_c6";

#define REPORT_QUEUE_DEPTH 32

/**
 * @brief BLE forwarding task – dequeues HID packets and sends BLE notifications.
 *
 * Sits on top of the shared report queue.  Low priority to yield to the
 * NimBLE host task (which handles retries / ACKs) when necessary.
 */
static void ble_forward_task(void *arg)
{
    QueueHandle_t queue = (QueueHandle_t)arg;
    bridge_pkt_t  pkt;

    ESP_LOGI(TAG, "BLE forward task running");

    while (1) {
        if (xQueueReceive(queue, &pkt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!ble_hid_is_connected()) {
            /* Not connected – silently discard */
            continue;
        }

        esp_err_t err;
        switch (pkt.type) {
        case BRIDGE_PKT_KEYBOARD:
            if (pkt.len >= KBD_REPORT_LEN) {
                err = ble_hid_send_keyboard(pkt.data);
                if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                    ESP_LOGW(TAG, "ble_hid_send_keyboard: 0x%x", err);
                }
            }
            break;

        case BRIDGE_PKT_MOUSE:
            if (pkt.len >= MOUSE_REPORT_LEN) {
                err = ble_hid_send_mouse(pkt.data);
                if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                    ESP_LOGW(TAG, "ble_hid_send_mouse: 0x%x", err);
                }
            }
            break;

        default:
            ESP_LOGW(TAG, "Unknown packet type 0x%02X", pkt.type);
            break;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== USB-to-BLE Bridge (ESP32-C6) starting ===");

    /* Shared queue: SDIO slave → BLE forward task */
    QueueHandle_t report_queue = xQueueCreate(REPORT_QUEUE_DEPTH,
                                              sizeof(bridge_pkt_t));
    if (!report_queue) {
        ESP_LOGE(TAG, "Failed to create report queue – halting");
        return;
    }

    /* Start BLE HID stack */
    ESP_ERROR_CHECK(ble_hid_start());

    /* Start SDIO slave receiver */
    ESP_ERROR_CHECK(sdio_slave_task_start(report_queue));

    /* Start BLE forwarding task */
    BaseType_t ret = xTaskCreate(ble_forward_task, "ble_fwd",
                                 4096, report_queue, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ble_forward_task – halting");
        return;
    }

    ESP_LOGI(TAG, "All C6 subsystems started. Waiting for BLE connection...");
}
