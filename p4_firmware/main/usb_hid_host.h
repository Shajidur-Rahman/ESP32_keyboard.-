/**
 * @file usb_hid_host.h
 * @brief USB HID Host driver wrapper for ESP32-P4.
 *
 * Manages enumeration of USB keyboard and mouse devices via the
 * ESP-IDF usb_host + hid_host class driver.  Parsed reports are
 * posted to a FreeRTOS queue for the SDIO master task to forward.
 */
#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "bridge_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the USB Host HID subsystem.
 *
 * Creates internal tasks and the USB Host library instance.
 * @param report_queue  Queue handle where bridge_pkt_t reports are posted.
 *                      The queue must accept items of sizeof(bridge_pkt_t).
 * @return ESP_OK on success.
 */
esp_err_t usb_hid_host_start(QueueHandle_t report_queue);

/**
 * @brief Stop the USB Host HID subsystem (call on shutdown).
 */
void usb_hid_host_stop(void);

#ifdef __cplusplus
}
#endif
