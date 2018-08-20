#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_wifi.h"
#include "lwip/ip4_addr.h"


typedef struct wifi_network_t {
    char ssid[33];
    char password[65];
    wifi_auth_mode_t authmode;
} wifi_network_t;

typedef enum wifi_state_t {
    WIFI_STATE_DISABLED,
    WIFI_STATE_DISCONNECTED,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
} wifi_state_t;

typedef void (*wifi_scan_done_cb_t)(void *arg);

volatile wifi_state_t wifi_state;
wifi_network_t **wifi_networks;
size_t wifi_network_count;
ip4_addr_t wifi_ip;

void wifi_init(void);
void wifi_enable(void);
void wifi_disable(void);
void wifi_network_add(wifi_network_t *network);
void wifi_network_delete(wifi_network_t *network);
void wifi_register_scan_done_callback(wifi_scan_done_cb_t cb, void *arg);
