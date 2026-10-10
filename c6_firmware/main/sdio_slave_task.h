/**
 * @file sdio_slave_task.h
 * @brief SDIO slave receiver for ESP32-C6.
 */
#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the SDIO slave and start the receive task.
 *
 * @param report_queue  Queue where received bridge_pkt_t items are posted.
 * @return ESP_OK on success.
 */
esp_err_t sdio_slave_task_start(QueueHandle_t report_queue);

/**
 * @brief Stop the SDIO slave task.
 */
void sdio_slave_task_stop(void);

#ifdef __cplusplus
}
#endif
