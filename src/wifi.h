#pragma once

#include <stdbool.h>

#include "esp_wifi.h"
#include "lwip/ip4_addr.h"


typedef struct wifi_network_t {
    char ssid[33];
    char password[65];
    wifi_auth_mode_t authmode;
} wifi_network_t;

bool wifi_enabled;
bool wifi_connected;
ip4_addr_t my_ip;

void wifi_init(void);
void wifi_enable(void);
void wifi_disable(void);
void wifi_network_add(wifi_network_t *network);
