#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event_loop.h"
#include "esp_wifi.h"

#include "frozen.h"
#include "wifi.h"


typedef struct wifi_network_t {
    uint8_t ssid[32];
    uint8_t password[64];
    uint32_t age;
} wifi_network_t;

bool wifi_enabled = false;
bool wifi_connected = false;

static wifi_network_t **s_networks = NULL;
static size_t s_network_count = 0;
static wifi_network_t *s_current_network = NULL;

static void connect_next_network(void)
{
    if (s_networks == NULL) {
        return;
    }

    uint32_t min_age = UINT32_MAX;
    wifi_network_t *next_network = NULL;

    if (s_current_network == NULL) {
        next_network = s_networks[0];
    } else {
        size_t last_index;
        for (size_t i = 0; i < s_network_count; i++) {
            if (s_networks[i] == s_current_network) {
                last_index = 0;
            } else {
                min_age = min_age > s_networks[i]->age ? s_networks[i]->age : min_age;
            }
        }

        for (size_t count = 0; count < s_network_count; count++) {
            size_t i = (last_index + 1 + count) % s_network_count;
            if (s_networks[i]->age <= min_age) {
                next_network = s_networks[i];
                break;
            }
        }
    }

    s_current_network = next_network;
    wifi_config_t wifi_config = {
        .sta = {
            .scan_method = WIFI_FAST_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold.rssi = -127,
            .threshold.authmode = WIFI_AUTH_OPEN,
        },
    };
    strncpy((char *)wifi_config.sta.ssid, (char *)next_network->ssid, sizeof(next_network->ssid));
    strncpy((char *)wifi_config.sta.password, (char *)next_network->password, sizeof(next_network->password));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id) {
        case SYSTEM_EVENT_STA_START:
            connect_next_network();
            ESP_ERROR_CHECK(esp_wifi_connect());
            break;

        case SYSTEM_EVENT_STA_GOT_IP:
            if (s_current_network) {
                s_current_network->age = 0;
            }
            wifi_connected = true;
            break;

        case SYSTEM_EVENT_STA_DISCONNECTED:
            wifi_connected = false;
            if (wifi_enabled) {
                if (s_current_network) {
                    s_current_network->age += 1;
                }
                connect_next_network();
            }
            break;

        default:
            break;
    }
    return ESP_OK;
}

static void load_config(void)
{
    const char *data;

    if (s_networks != NULL) {
        for (int i = 0; i < s_network_count; i++) {
            free(s_networks[i]);
        }
        free(s_networks);
        s_networks = NULL;
    }

    data = json_fread("/sdcard/wifi.json");
    if (data == NULL) {
        return;
    }

    struct json_token t;
    for (int i = 0; json_scanf_array_elem(data, strlen(data), ".networks", i, &t) > 0; i++) {
        char *ssid, *password;
        json_scanf(t.ptr, t.len, "{ssid: %Q, password: %Q}", &ssid, &password);

        s_networks = realloc(s_networks, sizeof(wifi_network_t *) * (s_network_count + 1));
        assert(s_networks != NULL);
        wifi_network_t *network = calloc(1, sizeof(wifi_network_t));
        assert(network != NULL);

        strncpy((char *)network->ssid, ssid, sizeof(network->ssid));
        strncpy((char *)network->password, password, sizeof(network->password));
        s_networks[s_network_count] = network;
        s_network_count += 1;

        free(ssid);
        free(password);
    }
}

void wifi_init(void)
{
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));
    load_config();
}

void wifi_enable(void)
{
    if (wifi_enabled) {
        return;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_enabled = true;
}

void wifi_disable(void)
{
    if (!wifi_enabled) {
        return;
    }

    wifi_enabled = false;
    ESP_ERROR_CHECK(esp_wifi_stop());
}
