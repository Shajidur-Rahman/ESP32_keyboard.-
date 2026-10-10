/**
 * @file sdio_master.h
 * @brief SDIO master driver for ESP32-P4 → ESP32-C6 forwarding.
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
 * @brief Initialise the SDIO master and start the forwarding task.
 *
 * @param report_queue  Queue to read bridge_pkt_t items from (produced by
 *                      the USB HID host task).
 * @return ESP_OK on success.
 */
esp_err_t sdio_master_start(QueueHandle_t report_queue);

/**
 * @brief Stop the SDIO master task and release resources.
 */
void sdio_master_stop(void);

#ifdef __cplusplus
}
#endif
