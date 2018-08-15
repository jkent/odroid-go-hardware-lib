#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event_loop.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"

#include "frozen.h"
#include "wifi.h"


bool wifi_enabled = false;
bool wifi_connected = false;
ip4_addr_t my_ip;

static wifi_network_t **s_networks = NULL;
static size_t s_network_count = 0;
static wifi_network_t *s_current_network = NULL;

static void connect_next_network(void)
{
    if (s_networks == NULL) {
        return;
    }

    if (s_current_network == NULL) {
        s_current_network = s_networks[0];
    } else {
        size_t current_index;
        for (current_index = 0; current_index < s_network_count; current_index++) {
            if (s_networks[current_index] == s_current_network) {
                break;
            }
        }
        if (current_index >= s_network_count) {
            return;
        }

        size_t next_index = (current_index + 1) % s_network_count;
        s_current_network = s_networks[next_index];
    }

    wifi_config_t wifi_config = {
        .sta = {
            .scan_method = WIFI_FAST_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold.rssi = -127,
            .threshold.authmode = s_current_network->authmode,
        },
    };
    strncpy((char *)wifi_config.sta.ssid, (char *)s_current_network->ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, (char *)s_current_network->password, sizeof(wifi_config.sta.password));
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
            wifi_connected = true;
            my_ip = event->event_info.got_ip.ip_info.ip;
            break;

        case SYSTEM_EVENT_STA_DISCONNECTED:
            wifi_connected = false;
            if (wifi_enabled) {
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
        char *ssid, *password, *authmode;
        json_scanf(t.ptr, t.len, "{ssid: %Q, password: %Q, authmode: %Q}", &ssid, &password, &authmode);

        s_networks = realloc(s_networks, sizeof(wifi_network_t *) * (s_network_count + 1));
        assert(s_networks != NULL);
        wifi_network_t *network = calloc(1, sizeof(wifi_network_t));
        assert(network != NULL);

        if (ssid) {
            strncpy(network->ssid, ssid, sizeof(network->ssid));
            network->ssid[sizeof(network->ssid) - 1] = '\0';
            free(ssid);
        }

        if (password) {
            strncpy(network->password, password, sizeof(network->password));
            network->password[sizeof(network->password) - 1] = '\0';
            free(password);
        }

        if (authmode) {
            if (strcmp(authmode, "Open") == 0) {
                network->authmode = WIFI_AUTH_OPEN;
            } else if (strcmp(authmode, "WEP") == 0) {
                network->authmode = WIFI_AUTH_WEP;
            } else if (strcmp(authmode, "WPA-PSK") == 0) {
                network->authmode = WIFI_AUTH_WPA_PSK;
            } else if (strcmp(authmode, "WPA2-PSK") == 0) {
                network->authmode = WIFI_AUTH_WPA2_PSK;
            }
            free(authmode);
        }

        s_networks[s_network_count] = network;
        s_network_count += 1;
    }
}

void wifi_network_add(wifi_network_t *network)
{
    s_networks = realloc(s_networks, sizeof(wifi_network_t *) * (s_network_count + 1));
    assert(s_networks != NULL);
    s_networks[s_network_count] = malloc(sizeof(wifi_network_t));
    assert(s_networks[s_network_count] != NULL);
    memcpy(s_networks[s_network_count], network, sizeof(wifi_network_t));
    s_network_count += 1;
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
    cfg.nvs_enable = false;
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
