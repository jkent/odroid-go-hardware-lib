#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_event_loop.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"

#include "frozen.h"
#include "periodic.h"
#include "wifi.h"

volatile bool wifi_enabled = false;
volatile bool wifi_connected = false;
wifi_network_t **wifi_networks = NULL;
size_t wifi_network_count = 0;
ip4_addr_t wifi_ip = { 0 };

static bool s_init = false;
static bool s_started = false;
static bool s_ignore_disconnect = false;
static wifi_network_t *s_current_network = NULL;
static wifi_scan_done_cb_t s_scan_done_cb = NULL;
static void *s_scan_done_arg = NULL;

static void connect_network(wifi_network_t *network)
{
    if (wifi_connected) {
        s_ignore_disconnect = true;
        ESP_ERROR_CHECK(esp_wifi_disconnect());
    }

    if (network == NULL) {
        return;
    }

    wifi_config_t wifi_config = {
        .sta = {
            .scan_method = WIFI_FAST_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold.rssi = -127,
            .threshold.authmode = network->authmode,
        },
    };

    strncpy((char *)wifi_config.sta.ssid, network->ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, network->password, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_connect());

    s_current_network = network;
}

static wifi_network_t *get_next_network(void)
{
    if (wifi_networks == NULL || wifi_network_count == 0) {
        return NULL;
    }

    if (s_current_network == NULL) {
        return wifi_networks[0];
    }

    size_t i = 0;
    for (i = 0; i < wifi_network_count; i++) {
        if (wifi_networks[i] == s_current_network) {
            break;
        }
    }
    if (i >= wifi_network_count) {
        return NULL;
    }

    i = (i + 1) % wifi_network_count;
    return wifi_networks[i];
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id) {
        case SYSTEM_EVENT_STA_START:
            ESP_ERROR_CHECK(esp_wifi_connect());
            break;

        case SYSTEM_EVENT_STA_CONNECTED:
            wifi_connected = true;
            break;

        case SYSTEM_EVENT_STA_GOT_IP:
            wifi_ip = event->event_info.got_ip.ip_info.ip;
            break;

        case SYSTEM_EVENT_SCAN_DONE:
            if (s_scan_done_cb) {
                s_scan_done_cb(s_scan_done_arg);
            }
            break;

        case SYSTEM_EVENT_STA_DISCONNECTED:
            memset(&wifi_ip, 0, sizeof(wifi_ip));
            if (s_ignore_disconnect) {
                wifi_connected = false;
                break;
            }
            if (wifi_connected) {
                wifi_connected = false;
                ESP_ERROR_CHECK(esp_wifi_connect());
                break;
            }
            connect_network(get_next_network());
            break;

        default:
            break;
    }
    return ESP_OK;
}

static void load_config(void)
{
    const char *data;

    if (wifi_networks != NULL) {
        for (int i = 0; i < wifi_network_count; i++) {
            free(wifi_networks[i]);
        }
        free(wifi_networks);
        wifi_networks = NULL;
    }

    data = json_fread("/sdcard/wifi.json");
    if (data == NULL) {
        return;
    }

    struct json_token t;
    for (int i = 0; json_scanf_array_elem(data, strlen(data), ".networks", i, &t) > 0; i++) {
        char *ssid = NULL;
        char *password = NULL; 
        char *authmode = NULL;
        json_scanf(t.ptr, t.len, "{ssid: %Q, password: %Q, authmode: %Q}", &ssid, &password, &authmode);

        wifi_networks = realloc(wifi_networks, sizeof(wifi_network_t *) * (wifi_network_count + 1));
        assert(wifi_networks != NULL);
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
            if (strcasecmp(authmode, "open") == 0) {
                network->authmode = WIFI_AUTH_OPEN;
            } else if (strcasecmp(authmode, "wep") == 0) {
                network->authmode = WIFI_AUTH_WEP;
            } else if (strcasecmp(authmode, "wpa-psk") == 0) {
                network->authmode = WIFI_AUTH_WPA_PSK;
            } else if (strcasecmp(authmode, "wpa2-psk") == 0) {
                network->authmode = WIFI_AUTH_WPA2_PSK;
            } else if (strcasecmp(authmode, "wpa/wpa2-psk") == 0) {
                network->authmode = WIFI_AUTH_WPA_WPA2_PSK;
            }
            free(authmode);
        }

        wifi_networks[wifi_network_count] = network;
        wifi_network_count += 1;
    }
}

void wifi_network_add(wifi_network_t *network)
{
    wifi_networks = realloc(wifi_networks, sizeof(wifi_network_t *) * (wifi_network_count + 1));
    assert(wifi_networks != NULL);
    wifi_network_t *new_network = malloc(sizeof(wifi_network_t));
    assert(new_network != NULL);
    memcpy(new_network, network, sizeof(wifi_network_t));
    connect_network(new_network);
    wifi_networks[wifi_network_count] = new_network;
    wifi_network_count += 1;
}

void wifi_network_delete(wifi_network_t *network)
{
    for (int i = 0; i < wifi_network_count; i++) {
        if (memcmp(network, wifi_networks[i], sizeof(wifi_network_t)) == 0) {
            if (s_current_network == wifi_networks[i]) {
                s_current_network = NULL;
            }
            if (i < wifi_network_count - 1) {
                memmove(&wifi_networks[i], &wifi_networks[i + 1], sizeof(wifi_network_t *) * (wifi_network_count - i - 1));
            }
            wifi_networks = realloc(wifi_networks, sizeof(wifi_network_t *) * wifi_network_count - 1);
            assert(wifi_networks != NULL);
            break;
        }
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

    if (!s_init) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        cfg.nvs_enable = false;
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        s_init = true;
    }

    if (!s_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
        s_started = true;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_enabled = true;

    if (s_current_network == NULL) {
        s_current_network = get_next_network();
    }

    connect_network(s_current_network);
}

void wifi_disable(void)
{
    if (!wifi_enabled) {
        return;
    }

    wifi_enabled = false;
    if (wifi_connected) {
        ESP_ERROR_CHECK(esp_wifi_disconnect());
    }
    ESP_ERROR_CHECK(esp_wifi_stop());
    s_started = false;
}

void wifi_register_scan_done_callback(wifi_scan_done_cb_t cb, void *arg)
{
    s_scan_done_cb = cb;
    s_scan_done_arg = arg;
}