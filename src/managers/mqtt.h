#pragma once
#include "config.h"
#include "store.h"

struct MqttTaskArgs {
    Configuration* config;
    EntityStore* store;
};

void launch_mqtt(MqttTaskArgs* args);
const char* mqtt_status();                 // "off" | "connecting" | "connected" | "error"
void mqtt_publish_now(EntityStore* store); // immediate state publish (pre-sleep telemetry)
