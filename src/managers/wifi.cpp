#include "wifi.h"
#include "config.h"
#include "store.h"
#include <WiFi.h>

static const char* TAG = "wifi";

void launch_wifi(Configuration* config, EntityStore* store) {
    WiFi.onEvent([store](WiFiEvent_t event, WiFiEventInfo_t info) {
        ESP_LOGI(TAG, "received wifi event: %d", event);

        switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            ESP_LOGI(TAG, "obtained IP address");
            store_set_wifi_state(store, ConnState::Up);
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        case ARDUINO_EVENT_WIFI_STA_LOST_IP:
            ESP_LOGI(TAG, "disconnected");
            store_set_wifi_state(store, ConnState::ConnectionError);
            break;
        }
    });

    WiFi.mode(WIFI_STA);
    WiFi.begin(config->wifi_ssid, config->wifi_password);
}
