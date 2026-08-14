#include "managers/mqtt.h"
#include "boards.h"
#include "constants.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include <Arduino.h>
#include <WiFi.h>
#include <cJSON.h>

static const char* TAG = "mqtt";

static const char* AVAILABILITY_TOPIC = "epaper-remote/availability";
static const char* STATE_TOPIC = "epaper-remote/state";

static const char* volatile g_mqtt_status = "off";
static esp_mqtt_client_handle_t g_client = nullptr;
static volatile bool g_connected = false;

static void mqtt_add_device_block(cJSON* root) {
    cJSON* device = cJSON_AddObjectToObject(root, "device");
    cJSON* ids = cJSON_AddArrayToObject(device, "identifiers");
    cJSON_AddItemToArray(ids, cJSON_CreateString("epaper-remote"));
    cJSON_AddStringToObject(device, "name", "ePaper Remote");
    cJSON_AddStringToObject(device, "manufacturer", "Lilygo / M5Stack");
#ifdef TARGET_LILYGO_T5_S3_PRO
    cJSON_AddStringToObject(device, "model", "T5 E-Paper S3 Pro");
#else
    cJSON_AddStringToObject(device, "model", "M5Paper S3");
#endif
    cJSON_AddStringToObject(device, "sw_version", __DATE__ " " __TIME__);
}

static void mqtt_publish_discovery_sensor(const char* key, const char* name, const char* device_class, const char* unit,
                                          const char* value_template, bool diagnostic) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", name);
    char unique_id[64];
    snprintf(unique_id, sizeof(unique_id), "epaper_remote_%s", key);
    cJSON_AddStringToObject(root, "unique_id", unique_id);
    cJSON_AddStringToObject(root, "state_topic", STATE_TOPIC);
    cJSON_AddStringToObject(root, "availability_topic", AVAILABILITY_TOPIC);
    cJSON_AddStringToObject(root, "value_template", value_template);
    if (device_class != nullptr) {
        cJSON_AddStringToObject(root, "device_class", device_class);
    }
    if (unit != nullptr) {
        cJSON_AddStringToObject(root, "unit_of_measurement", unit);
        cJSON_AddStringToObject(root, "state_class", "measurement");
    }
    if (diagnostic) {
        cJSON_AddStringToObject(root, "entity_category", "diagnostic");
    }
    mqtt_add_device_block(root);

    char topic[96];
    snprintf(topic, sizeof(topic), "homeassistant/sensor/epaper_remote/%s/config", key);
    char* payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload != nullptr) {
        esp_mqtt_client_publish(g_client, topic, payload, 0, 1, 1); // retained
        cJSON_free(payload);
    }
}

static void mqtt_publish_discovery() {
    mqtt_publish_discovery_sensor("battery", "Battery", "battery", "%", "{{ value_json.battery_pct }}", false);
    mqtt_publish_discovery_sensor("battery_voltage", "Battery voltage", "voltage", "V", "{{ value_json.battery_v }}", true);
    mqtt_publish_discovery_sensor("battery_current", "Battery current", "current", "mA", "{{ value_json.battery_ma }}", true);
    mqtt_publish_discovery_sensor("rssi", "Wi-Fi signal", "signal_strength", "dBm", "{{ value_json.rssi }}", true);
    mqtt_publish_discovery_sensor("internal_heap", "Internal heap free", nullptr, "B", "{{ value_json.internal_free }}", true);
    mqtt_publish_discovery_sensor("uptime", "Uptime", "duration", "s", "{{ value_json.uptime_s }}", true);
}

static void mqtt_publish_state(EntityStore* store) {
    if (!g_connected) {
        return;
    }

    cJSON* root = cJSON_CreateObject();
    BatteryStatus battery;
    store_get_battery(store, &battery);
    if (battery.valid) {
        cJSON_AddNumberToObject(root, "battery_pct", battery.pct);
        cJSON_AddNumberToObject(root, "battery_v", battery.millivolts / 1000.0);
        cJSON_AddNumberToObject(root, "battery_ma", battery.milliamps);
    }
    cJSON_AddNumberToObject(root, "rssi", WiFi.RSSI());
    cJSON_AddNumberToObject(root, "internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "uptime_s", millis() / 1000);

    char* payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload != nullptr) {
        esp_mqtt_client_publish(g_client, STATE_TOPIC, payload, 0, 0, 0);
        cJSON_free(payload);
    }
}

static void mqtt_event_handler(void* args, esp_event_base_t base, int32_t event_id, void* event_data) {
    switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
    case MQTT_EVENT_CONNECTED:
        g_connected = true;
        g_mqtt_status = "connected";
        ESP_LOGI(TAG, "connected, publishing discovery");
        esp_mqtt_client_publish(g_client, AVAILABILITY_TOPIC, "online", 0, 1, 1);
        mqtt_publish_discovery();
        break;
    case MQTT_EVENT_DISCONNECTED:
        g_connected = false;
        g_mqtt_status = "connecting";
        break;
    case MQTT_EVENT_ERROR:
        g_mqtt_status = "error";
        break;
    default:
        break;
    }
}

static void mqtt_task(void* arg) {
    MqttTaskArgs* ctx = static_cast<MqttTaskArgs*>(arg);

    store_wait_for_wifi_up(ctx->store);

    esp_mqtt_client_config_t config = {};
    config.broker.address.uri = ctx->config->mqtt_uri;
    config.credentials.client_id = "epaper-remote";
    config.session.last_will.topic = AVAILABILITY_TOPIC;
    config.session.last_will.msg = "offline";
    config.session.last_will.qos = 1;
    config.session.last_will.retain = 1;
    config.session.keepalive = 30;
    config.buffer.size = 2048;

    g_client = esp_mqtt_client_init(&config);
    if (g_client == nullptr) {
        ESP_LOGE(TAG, "client init failed");
        g_mqtt_status = "error";
        vTaskDelete(nullptr);
        return;
    }
    esp_mqtt_client_register_event(g_client, static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID), mqtt_event_handler, nullptr);
    g_mqtt_status = "connecting";
    esp_mqtt_client_start(g_client);

    while (true) {
        mqtt_publish_state(ctx->store);
        vTaskDelay(pdMS_TO_TICKS(MQTT_PUBLISH_INTERVAL_MS));
    }
}

void launch_mqtt(MqttTaskArgs* args) {
    if (args->config->mqtt_uri == nullptr || args->config->mqtt_uri[0] == '\0') {
        return;
    }
    xTaskCreate(mqtt_task, "mqtt", 4096, args, 1, nullptr);
}

const char* mqtt_status() {
    return g_mqtt_status;
}
