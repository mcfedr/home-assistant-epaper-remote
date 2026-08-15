#pragma once
#include "config.h"
#include "store.h"

void launch_wifi(Configuration* config, EntityStore* store);
void wifi_poll();
void wifi_debug_report(char* out, size_t out_len);
void wifi_force_recovery();
void wifi_deauth(); // clean AP disconnect before restart/deep sleep
void wifi_request_scan();
bool wifi_connect_to_network(const char* ssid, const char* password);
bool wifi_reset_to_default();
bool wifi_find_saved_password(const char* ssid, char* pass_out, size_t pass_len);
bool wifi_reconnect();
