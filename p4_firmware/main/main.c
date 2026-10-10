/**
 * @file main.c (ESP32-P4 firmware)
 * @brief Low-latency USB-to-Wireless (BLE + Wi-Fi UDP) Streaming Bridge.
 *
 * Simultaneously streams USB keyboard and mouse reports:
 *   1. Over Wi-Fi 6 (802.11ax) UDP broadcast on port 5000 (<1 ms latency).
 *   2. Over BLE HID (HOGP) if a host PC is paired.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"

#include "usb_hid_host.h"
#include "ble_hid.h"
#include "wifi_stream.h"
#include "bridge_protocol.h"

static const char *TAG = "main_p4";

#define REPORT_QUEUE_DEPTH 32

static void forward_task(void *arg)
{
    QueueHandle_t queue = (QueueHandle_t)arg;
    bridge_pkt_t  pkt;

    ESP_LOGI(TAG, "Wireless forward task started");

    while (1) {
        if (xQueueReceive(queue, &pkt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* 1. Fast Wi-Fi UDP stream (<1 ms delivery) */
        wifi_stream_send_pkt(&pkt);

        /* 2. BLE HID notification if Bluetooth host connected */
        if (ble_hid_is_connected()) {
            switch (pkt.type) {
            case BRIDGE_PKT_KEYBOARD:
                if (pkt.len >= KBD_REPORT_LEN) {
                    ble_hid_send_keyboard(pkt.data);
                }
                break;
            case BRIDGE_PKT_MOUSE:
                if (pkt.len >= MOUSE_REPORT_LEN) {
                    ble_hid_send_mouse(pkt.data);
                }
                break;
            default:
                break;
            }
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Low-Latency USB-to-Wireless Bridge (ESP32-P4 + C6) ===");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    QueueHandle_t report_queue = xQueueCreate(REPORT_QUEUE_DEPTH, sizeof(bridge_pkt_t));
    if (report_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create report queue");
        return;
    }

    /* Start Wi-Fi 6 streaming via ESP32-C6 Hosted Co-processor */
    ESP_ERROR_CHECK(wifi_stream_init());

    /* Start BLE HID advertising so Redmi Pad SE can pair directly */
    esp_err_t ble_err = ble_hid_start();
    if (ble_err == ESP_OK) {
        ESP_LOGI(TAG, "BLE HID Keyboard active! Pairing name: 'ESP-HID-Bridge'");
    } else {
        ESP_LOGW(TAG, "BLE HID start returned: %d", ble_err);
    }

    /* Start forwarder task */
    xTaskCreatePinnedToCore(forward_task, "fwd_task", 4096, report_queue, 6, NULL, 1);

    /* Start USB Host HID stack */
    ESP_ERROR_CHECK(usb_hid_host_start(report_queue));

    ESP_LOGI(TAG, "Ready! Plug in USB keyboard/mouse into ESP32-P4.");
}
