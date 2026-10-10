/**
 * @file ble_hid.h
 * @brief BLE HID Device (HOGP) implementation for ESP32-C6 using NimBLE.
 */
#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "bridge_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise NimBLE and start advertising as a BLE HID device.
 *
 * @return ESP_OK on success.
 */
esp_err_t ble_hid_start(void);

/**
 * @brief Send a keyboard HID report over BLE notification.
 *
 * @param report  Pointer to 8-byte keyboard boot report.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not connected.
 */
esp_err_t ble_hid_send_keyboard(const uint8_t *report);

/**
 * @brief Send a mouse HID report over BLE notification.
 *
 * @param report  Pointer to 4-byte mouse boot report.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not connected.
 */
esp_err_t ble_hid_send_mouse(const uint8_t *report);

/**
 * @brief Returns true if a BLE central (PC) is currently connected.
 */
bool ble_hid_is_connected(void);

#ifdef __cplusplus
}
#endif
