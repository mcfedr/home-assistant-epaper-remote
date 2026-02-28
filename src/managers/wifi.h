#pragma once
#include "config.h"
#include "store.h"

void launch_wifi(Configuration* config, EntityStore* store);
void wifi_poll();
void wifi_request_scan();
bool wifi_connect_to_network(const char* ssid, const char* password);
bool wifi_reset_to_default();
