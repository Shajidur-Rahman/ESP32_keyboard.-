#pragma once

#include "esp_err.h"
#include "bridge_protocol.h"
#include <stdbool.h>

#define UDP_STREAM_PORT 5000

esp_err_t wifi_stream_init(void);
bool      wifi_stream_is_connected(void);
esp_err_t wifi_stream_send_pkt(const bridge_pkt_t *pkt);
