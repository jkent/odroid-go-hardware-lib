#include <errno.h>
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


#define CONFIG_FILE "/spiffs/wifi.json"
#define BACKUP_CONFIG_FILE "/sdcard/wifi.json"

wifi_network_t **wifi_networks = NULL;
size_t wifi_network_count = 0;

static volatile wifi_state_t s_wifi_state = WIFI_STATE_DISABLED;
static bool s_ignore_disconnect = false;
static wifi_network_t *s_current_network = NULL;
ip4_addr_t s_wifi_ip = { 0 };
static wifi_scan_done_cb_t s_scan_done_cb = NULL;
static void *s_scan_done_arg = NULL;

void wifi_connect_network(wifi_network_t *network)
{
    if (s_wifi_state == WIFI_STATE_CONNECTED) {
        s_ignore_disconnect = true;
        ESP_ERROR_CHECK(esp_wifi_disconnect());
        s_wifi_state = WIFI_STATE_DISCONNECTED;
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
    s_wifi_state = WIFI_STATE_CONNECTING;

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
            esp_wifi_connect();
            break;

        case SYSTEM_EVENT_STA_CONNECTED:
            s_wifi_state = WIFI_STATE_CONNECTED;
            break;

        case SYSTEM_EVENT_STA_GOT_IP:
            s_wifi_ip = event->event_info.got_ip.ip_info.ip;
            break;

        case SYSTEM_EVENT_SCAN_DONE:
            if (s_scan_done_cb) {
                s_scan_done_cb(s_scan_done_arg);
            }
            break;

        case SYSTEM_EVENT_STA_DISCONNECTED:
            memset(&s_wifi_ip, 0, sizeof(s_wifi_ip));
            if (s_ignore_disconnect) {
                s_ignore_disconnect = false;
                break;
            }
            if (s_wifi_state == WIFI_STATE_CONNECTED) {
                s_wifi_state = WIFI_STATE_DISCONNECTED;
                ESP_ERROR_CHECK(esp_wifi_connect());
                break;
            }
            if (s_wifi_state != WIFI_STATE_DISABLED) {
                wifi_connect_network(get_next_network());
            }
            break;

        default:
            break;
    }
    return ESP_OK;
}

static void load_config(const char *path)
{
    const char *data;

    if (wifi_networks != NULL) {
        for (int i = 0; i < wifi_network_count; i++) {
            free(wifi_networks[i]);
        }
        free(wifi_networks);
        wifi_networks = NULL;
        wifi_network_count = 0;
    }

    data = json_fread(path);
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

static int json_printf_network(struct json_out *out, va_list *ap)
{
    wifi_network_t *network = va_arg(*ap, wifi_network_t *);
    char *authmode;
    switch (network->authmode) {
        case WIFI_AUTH_OPEN:
            authmode = "open";
            break;
        case WIFI_AUTH_WEP:
            authmode = "wep";
            break;
        case WIFI_AUTH_WPA_PSK:
            authmode = "wpa-psk";
            break;
        case WIFI_AUTH_WPA2_PSK:
            authmode = "wpa2-psk";
            break;
        case WIFI_AUTH_WPA_WPA2_PSK:
            authmode = "wpa/wpa2-psk";
            break;
        default:
            authmode = "unknown";
            break;
    }
    return json_printf(out, "{ssid: %Q, password: %Q, authmode: %Q}", network->ssid, network->password, authmode);
}

static int json_printf_networks(struct json_out *out, va_list *ap)
{
    int len = 0;
    wifi_network_t **networks = va_arg(*ap, wifi_network_t **);
    size_t network_count = va_arg(*ap, size_t);
    len += json_printf(out, "[");
    for (int i = 0; i < network_count; i++) {
        if (i > 0) {
            len += json_printf(out, ", ");
        }
        len += json_printf(out, "%M", json_printf_network, networks[i]);
    }
    len += json_printf(out, "]");
    return len;
}

static void write_config(const char *path)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return; /* fail silently */
    }
    struct json_out out = JSON_OUT_FILE(f);
    json_printf(&out, "{networks: %M}", json_printf_networks, wifi_networks, wifi_network_count);
    fclose(f);
}

static int compare_wifi_networks(const void *a, const void *b)
{
    const wifi_network_t *aa = (const wifi_network_t *)a;
    const wifi_network_t *bb = (const wifi_network_t *)b;

    return strcasecmp((const char *)aa->ssid, (const char *)bb->ssid);
}

static int find_last_wifi_network(wifi_network_t *network)
{
    int i;
    for (i = 0; i < wifi_network_count; i++) {
        int n = compare_wifi_networks(network, wifi_networks[i]);
        if (n == 0) {
            int last_seen = i;
            i += 1;
            while (i < wifi_network_count) {
                n = compare_wifi_networks(network, wifi_networks[i]);
                if (n == 0) {
                    last_seen = i;
                } else {
                    break;
                }
                i += 1;
            }
            return last_seen;
        }
        if (n < 0) {
            break;
        }
    }
    return -(i + 1);
}

size_t wifi_network_add(wifi_network_t *network)
{
    int i = find_last_wifi_network(network);
    if (i >= 0) {
        i += 1; /* insert after last */
    } else {
        i = -(i + 1);
    }

    wifi_networks = realloc(wifi_networks, sizeof(wifi_network_t *) * (wifi_network_count + 1));
    assert(wifi_networks != NULL);
    memmove(&wifi_networks[i + 1], &wifi_networks[i], sizeof(wifi_network_t *) * (wifi_network_count - i));
    wifi_networks[i] = malloc(sizeof(wifi_network_t));
    assert(wifi_networks[i] != NULL);
    memcpy(wifi_networks[i], network, sizeof(wifi_network_t));
    wifi_network_count += 1;

    write_config(CONFIG_FILE ".new");
    remove(CONFIG_FILE);
    rename(CONFIG_FILE ".new", CONFIG_FILE);

    if (s_wifi_state != WIFI_STATE_DISABLED && s_wifi_state != WIFI_STATE_CONNECTED) {
        wifi_connect_network(wifi_networks[wifi_network_count - 1]);
    }

    return i;
}

int wifi_network_delete(wifi_network_t *network)
{
    size_t i;
    for (i = 0; i < wifi_network_count; i++) {
        if (network == wifi_networks[i]) {
            break;
        }
    }
    if (i >= wifi_network_count) {
        return -1;
    }

    if (s_current_network == wifi_networks[i]) {
        if (s_wifi_state == WIFI_STATE_CONNECTED) {
            wifi_network_t *next = get_next_network();
            if (next == s_current_network) {
                next = NULL;
            }
            wifi_connect_network(next);
        }
    }

    if (i < wifi_network_count - 1) {
        memmove(&wifi_networks[i], &wifi_networks[i + 1], sizeof(wifi_network_t *) * (wifi_network_count - i - 1));
    }
    wifi_networks = realloc(wifi_networks, sizeof(wifi_network_t *) * wifi_network_count - 1);
    assert(wifi_networks != NULL);
    wifi_network_count -= 1;

    write_config(CONFIG_FILE ".new");
    remove(CONFIG_FILE);
    rename(CONFIG_FILE ".new", CONFIG_FILE);

    return i;
}

wifi_network_t *wifi_network_iterate(wifi_network_t *network)
{
    size_t i;

    if (network == NULL) {
        return wifi_network_count > 0 ? wifi_networks[0] : NULL;
    }

    for (i = 0; i < wifi_network_count; i++) {
        if (network == wifi_networks[i]) {
            break;
        }
    }

    if (i + 1 < wifi_network_count) {
        return wifi_networks[i + 1];
    } else {
        return NULL;
    }
}

wifi_state_t wifi_get_state(void)
{
    return s_wifi_state;
}

ip4_addr_t wifi_get_ip(void)
{
    return s_wifi_ip;
}

void wifi_init(void)
{
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));
    load_config(CONFIG_FILE);
}

void wifi_enable(void)
{
    static bool wifi_init = false;

    if (s_wifi_state != WIFI_STATE_DISABLED) {
        return;
    }

    if (!wifi_init) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        cfg.nvs_enable = false;
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        wifi_init = true;
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MAX_MODEM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    s_wifi_state = WIFI_STATE_DISCONNECTED;

    if (s_current_network == NULL) {
        s_current_network = get_next_network();
    }

    wifi_connect_network(s_current_network);
}

void wifi_disable(void)
{
    if (s_wifi_state == WIFI_STATE_DISABLED) {
        return;
    }

    if (s_wifi_state == WIFI_STATE_CONNECTED) {
        s_ignore_disconnect = true;
        ESP_ERROR_CHECK(esp_wifi_disconnect());
    }

    ESP_ERROR_CHECK(esp_wifi_stop());
    s_wifi_state = WIFI_STATE_DISABLED;
}

void wifi_register_scan_done_callback(wifi_scan_done_cb_t cb, void *arg)
{
    s_scan_done_cb = cb;
    s_scan_done_arg = arg;
}

void wifi_backup_config(void)
{
    write_config(BACKUP_CONFIG_FILE);
}

void wifi_restore_config(void)
{
    load_config(BACKUP_CONFIG_FILE);
    write_config(CONFIG_FILE ".new");
    remove(CONFIG_FILE);
    rename(CONFIG_FILE ".new", CONFIG_FILE);
}
