#include "wifi_stream.h"
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/sockets.h"

static const char *TAG = "wifi_stream";

#define DEFAULT_WIFI_SSID  "Saad"
#define DEFAULT_WIFI_PASS  "AndroiD@mekalil.123,./"

static int s_udp_sock = -1;
static struct sockaddr_in s_dest_addr;
static bool s_wifi_connected = false;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi Station started, connecting to %s...", DEFAULT_WIFI_SSID);
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "Wi-Fi connected to %s", DEFAULT_WIFI_SSID);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        ESP_LOGW(TAG, "Wi-Fi disconnected. Reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected = true;

        /* Create UDP broadcast socket */
        if (s_udp_sock >= 0) {
            close(s_udp_sock);
        }
        s_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (s_udp_sock >= 0) {
            int broadcast = 1;
            setsockopt(s_udp_sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

            /* Set non-blocking or short send timeout */
            struct timeval tv = { .tv_sec = 0, .tv_usec = 10000 };
            setsockopt(s_udp_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            memset(&s_dest_addr, 0, sizeof(s_dest_addr));
            s_dest_addr.sin_family = AF_INET;
            s_dest_addr.sin_port = htons(UDP_STREAM_PORT);
            s_dest_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
            ESP_LOGI(TAG, "Low-latency UDP streaming active on port %d", UDP_STREAM_PORT);
        }
    }
}

esp_err_t wifi_stream_init(void)
{
    ESP_LOGI(TAG, "Initializing Wi-Fi (ESP32-C6 Hosted)...");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = DEFAULT_WIFI_SSID,
            .password = DEFAULT_WIFI_PASS,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

bool wifi_stream_is_connected(void)
{
    return s_wifi_connected;
}

esp_err_t wifi_stream_send_pkt(const bridge_pkt_t *pkt)
{
    if (!s_wifi_connected || s_udp_sock < 0 || !pkt) {
        return ESP_ERR_INVALID_STATE;
    }

    int err = sendto(s_udp_sock, pkt, sizeof(bridge_pkt_t), 0,
                     (struct sockaddr *)&s_dest_addr, sizeof(s_dest_addr));
    if (err < 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
