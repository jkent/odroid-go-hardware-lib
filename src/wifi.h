#pragma once

#include <stdbool.h>

#include "lwip/ip4_addr.h"


bool wifi_enabled;
bool wifi_connected;
ip4_addr_t my_ip;

void wifi_init(void);
void wifi_enable(void);
void wifi_disable(void);
