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

volatile bool wifi_enabled;
volatile bool wifi_connected;
wifi_network_t **wifi_networks;
size_t wifi_network_count;
ip4_addr_t my_ip;

void wifi_init(void);
void wifi_enable(void);
void wifi_disable(void);
void wifi_network_add(wifi_network_t *network);
void wifi_network_delete(wifi_network_t *network);
