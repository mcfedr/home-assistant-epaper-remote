#include <IPAddress.h> // fixes compilation issues with esp_websocket_client

#include "config.h"
#include "constants.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "managers/home_assistant.h"
#include "store.h"
#include <cJSON.h>

typedef struct home_assistant_context {
    EntityStore* store;
    Configuration* config;
    esp_websocket_client_handle_t client;
    ConnState state;
    SemaphoreHandle_t mutex;
    TaskHandle_t task;

    uint16_t event_id;                      // counter to send events
    char json_buffer[HASS_MAX_JSON_BUFFER]; // Buffer for accumulating JSON data
    size_t json_buffer_len;                 // Current buffer length

    // Home assistant sends its updates per attribute. This means
    // that if the a value is updated on an device in an off state,
    // we will only receive the "brightness" or "percentage" field,
    // but the actual "off" state was sent in a previous update.
    // To handle that properly, we need to store those values.
    uint8_t entity_count;
    const char* entity_ids[MAX_ENTITIES];
    bool entity_states[MAX_ENTITIES];
    int8_t entity_values[MAX_ENTITIES]; // -1 = unknown (handles switches)
    TickType_t last_command_sent_at_ms[MAX_ENTITIES];
} home_assistant_context_t;

static const char* TAG = "home_assistant";

void hass_update_state(home_assistant_context_t* hass, ConnState state) {
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    ConnState previous_state = hass->state;
    hass->state = state;
    xSemaphoreGive(hass->mutex);

    if (previous_state == state) {
        return;
    }

    // Update the UI state
    if (state == ConnState::Initializing) {
        // initial state at boot time, do nothing
    } else if (state == ConnState::ConnectionError && previous_state == ConnState::InvalidCredentials) {
        // keep invalid credentials in the UI, do nothing
    } else {
        store_set_hass_state(hass->store, state);
    }

    // Notify the main thread
    xTaskNotifyGive(hass->task);
}

uint16_t hass_generate_event_id(home_assistant_context_t* hass) {
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    uint16_t event_id = hass->event_id++;
    xSemaphoreGive(hass->mutex);

    return event_id;
}

void hass_cmd_authenticate(home_assistant_context_t* hass) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "auth");
    cJSON_AddStringToObject(root, "access_token", hass->config->home_assistant_token);
    const char* request = cJSON_PrintUnformatted(root);
    esp_websocket_client_send_text(hass->client, request, strlen(request), portMAX_DELAY);
    cJSON_Delete(root);
}

int16_t hass_match_entity(home_assistant_context_t* hass, char* key) {
    for (uint8_t i = 0; i < hass->entity_count; i++) {
        if (strcmp(key, hass->entity_ids[i]) == 0) {
            return i;
        }
    }

    return -1;
}

void hass_parse_entity_update(home_assistant_context_t* hass, uint8_t widget_idx, cJSON* item) {
    // Parse state
    cJSON* state = cJSON_GetObjectItem(item, "s");
    if (cJSON_IsString(state)) {
        if (strcmp(state->valuestring, "on") == 0) {
            hass->entity_states[widget_idx] = true;
        } else if (strcmp(state->valuestring, "off") == 0) {
            hass->entity_states[widget_idx] = false;
        }
    }

    // Parse attributes
    cJSON* attributes = cJSON_GetObjectItem(item, "a");
    if (cJSON_IsObject(attributes)) {
        // Extract percentage if available
        cJSON* percentage = cJSON_GetObjectItem(attributes, "percentage");
        if (cJSON_IsNumber(percentage)) {
            hass->entity_values[widget_idx] = percentage->valueint;
        }

        // Extract brightness
        cJSON* brightness = cJSON_GetObjectItem(attributes, "brightness");
        if (cJSON_IsNumber(brightness)) {
            hass->entity_values[widget_idx] = brightness->valueint * 100 / 254;
        }

        // Extract off_brightness
        cJSON* off_brightness = cJSON_GetObjectItem(attributes, "off_brightness");
        if (cJSON_IsNumber(off_brightness)) {
            hass->entity_values[widget_idx] = off_brightness->valueint * 100 / 254;
        }
    }

    // Update the full state
    TickType_t now = xTaskGetTickCount();
    if ((now - hass->last_command_sent_at_ms[widget_idx]) < pdMS_TO_TICKS(HASS_IGNORE_UPDATE_DELAY_MS)) {
        ESP_LOGI(TAG, "Ignoring update of entity %s", hass->entity_ids[widget_idx]);
    } else {
        uint8_t value = 0;
        if (hass->entity_states[widget_idx]) {
            if (hass->entity_values[widget_idx] == -1) {
                // Unknown value, probably a switch
                value = 1;
            } else {
                value = hass->entity_values[widget_idx];
            }
        }

        ESP_LOGI(TAG, "Setting value of widget %d to %d", widget_idx, value);
        store_update_value(hass->store, widget_idx, value);
    }
}

void hass_handle_entity_update(home_assistant_context_t* hass, cJSON* event) {
    // Handle initial update
    cJSON* initial_values = cJSON_GetObjectItem(event, "a");
    if (cJSON_IsObject(initial_values)) {
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, initial_values) {
            int16_t entity_id = hass_match_entity(hass, item->string);
            if (entity_id != -1) {
                ESP_LOGI(TAG, "Found initial value for widget %d (%s)", entity_id, item->string);
                hass_parse_entity_update(hass, entity_id, item);
            }
        }
    }

    // Handle changes
    cJSON* changes = cJSON_GetObjectItem(event, "c");
    if (cJSON_IsObject(changes)) {
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, changes) {
            int16_t entity_id = hass_match_entity(hass, item->string);
            if (entity_id != -1) {
                cJSON* plus_value = cJSON_GetObjectItem(item, "+");
                if (cJSON_IsObject(plus_value)) {
                    ESP_LOGI(TAG, "Found update for widget %d (%s)", entity_id, item->string);
                    hass_parse_entity_update(hass, entity_id, plus_value);
                }
            }
        }
    }

    // Update the state once we've received the first update
    hass_update_state(hass, ConnState::Up);
}

void hass_cmd_subscribe(home_assistant_context_t* hass) {
    cJSON *root, *trigger, *entity_ids;
    root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", hass_generate_event_id(hass));
    cJSON_AddStringToObject(root, "type", "subscribe_entities");
    cJSON_AddItemToObject(root, "entity_ids", cJSON_CreateStringArray(hass->entity_ids, hass->entity_count));

    // Send the data
    const char* request = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "%s", request);
    esp_websocket_client_send_text(hass->client, request, strlen(request), portMAX_DELAY);

    // Cleanup
    cJSON_Delete(root);
}

void hass_handle_server_payload(home_assistant_context_t* hass, cJSON* json) {
    cJSON* type_item = cJSON_GetObjectItem(json, "type");
    if (cJSON_IsString(type_item) && (type_item->valuestring != NULL)) {
        ESP_LOGI(TAG, "Received Home Assistant message of type %s", type_item->valuestring);

        if (strcmp(type_item->valuestring, "auth_required") == 0) {
            ESP_LOGI(TAG, "Logging in to home assistant...");
            hass_cmd_authenticate(hass);
        } else if (strcmp(type_item->valuestring, "auth_invalid") == 0) {
            ESP_LOGI(TAG, "Updating state to InvalidCredentials");
            hass_update_state(hass, ConnState::InvalidCredentials);
        } else if (strcmp(type_item->valuestring, "auth_ok") == 0) {
            ESP_LOGI(TAG, "Authentication successful, subscribing to events");
            hass_cmd_subscribe(hass);
        } else if (strcmp(type_item->valuestring, "event") == 0) {
            // it's nested soo far
            cJSON* event = cJSON_GetObjectItem(json, "event");
            if (cJSON_IsObject(event)) {
                ESP_LOGI(TAG, "Handling state update");
                hass_handle_entity_update(hass, event);
            }
        } else {
            ESP_LOGI(TAG, "Ignoring HASS event type %s", type_item->valuestring);
        }
    }
}

static void hass_ws_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
    home_assistant_context_t* hass = (home_assistant_context_t*)handler_args;
    esp_websocket_event_data_t* data = (esp_websocket_event_data_t*)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Received WEBSOCKET_EVENT_CONNECTED");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Received WEBSOCKET_EVENT_DISCONNECTED");
        hass_update_state(hass, ConnState::ConnectionError);
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGI(TAG, "Received WEBSOCKET_EVENT_ERROR");
        hass_update_state(hass, ConnState::ConnectionError);
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0 || data->op_code == 1) {
            // Data or continuation
            ESP_LOGI(TAG, "Received WEBSOCKET_EVENT_DATA with op_code=%d", data->op_code);

            xSemaphoreTake(hass->mutex, portMAX_DELAY);

            if (data->payload_offset == 0) {
                hass->json_buffer_len = 0;
            }

            if (data->payload_offset != hass->json_buffer_len) {
                ESP_LOGE(TAG, "out of sync message, ignoring");
                xSemaphoreGive(hass->mutex);
                return;
            }

            if (hass->json_buffer_len + data->data_len >= HASS_MAX_JSON_BUFFER) {
                ESP_LOGE(TAG, "JSON buffer overflow, discarding messages");
                hass->json_buffer_len = 0;
                xSemaphoreGive(hass->mutex);
                return;
            }

            memcpy(hass->json_buffer + hass->json_buffer_len, data->data_ptr, data->data_len);
            hass->json_buffer_len += data->data_len;

            cJSON* json = nullptr;
            if (hass->json_buffer_len == data->payload_len && hass->json_buffer_len > 0) {
                json = cJSON_ParseWithLength(hass->json_buffer, hass->json_buffer_len);
                if (!json) {
                    ESP_LOGE(TAG, "JSON parsing failed");
                }
            }
            xSemaphoreGive(hass->mutex);

            if (json) {
                hass_handle_server_payload(hass, json);
                cJSON_Delete(json);
            }
        } else if (data->op_code == 8) {
            ESP_LOGI(TAG, "Received Connection Close frame");
            hass_update_state(hass, ConnState::ConnectionError);
        }

        break;
    default:
        ESP_LOGI(TAG, "Unknown event type %d", event_id);
    }
}

void hass_send_command(home_assistant_context_t* hass, Command* cmd) {
    // Build json
    cJSON *root, *service_data;
    root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", hass_generate_event_id(hass));
    cJSON_AddStringToObject(root, "type", "call_service");

    switch (cmd->type) {
    case CommandType::SetLightBrightnessPercentage:
        cJSON_AddStringToObject(root, "domain", "light");
        cJSON_AddStringToObject(root, "service", "turn_on");
        cJSON_AddItemToObject(root, "service_data", service_data = cJSON_CreateObject());
        cJSON_AddStringToObject(service_data, "entity_id", cmd->entity_id);
        cJSON_AddNumberToObject(service_data, "brightness_pct", cmd->value);
        break;
    case CommandType::SetFanSpeedPercentage:
        cJSON_AddStringToObject(root, "domain", "fan");
        cJSON_AddStringToObject(root, "service", "set_percentage");
        cJSON_AddItemToObject(root, "service_data", service_data = cJSON_CreateObject());
        cJSON_AddStringToObject(service_data, "entity_id", cmd->entity_id);
        cJSON_AddNumberToObject(service_data, "percentage", cmd->value);
        break;
    case CommandType::SwitchOnOff:
        cJSON_AddStringToObject(root, "domain", "switch");
        if (cmd->value == 0) {
            cJSON_AddStringToObject(root, "service", "turn_off");
        } else {
            cJSON_AddStringToObject(root, "service", "turn_on");
        }
        cJSON_AddItemToObject(root, "service_data", service_data = cJSON_CreateObject());
        cJSON_AddStringToObject(service_data, "entity_id", cmd->entity_id);
        break;
    case CommandType::AutomationOnOff:
        cJSON_AddStringToObject(root, "domain", "automation");
        if (cmd->value == 0) {
            cJSON_AddStringToObject(root, "service", "turn_off");
        } else {
            cJSON_AddStringToObject(root, "service", "turn_on");
        }
        cJSON_AddItemToObject(root, "service_data", service_data = cJSON_CreateObject());
        cJSON_AddStringToObject(service_data, "entity_id", cmd->entity_id);
        break;
    default:
        ESP_LOGI(TAG, "Service type not supported");
        cJSON_Delete(root);
        return;
    }

    // Send the data
    const char* request = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "Sending %s", request);

    hass->last_command_sent_at_ms[cmd->entity_idx] = xTaskGetTickCount();
    esp_websocket_client_send_text(hass->client, request, strlen(request), portMAX_DELAY);

    // Cleanup
    cJSON_Delete(root);
}

void home_assistant_task(void* arg) {
    HomeAssistantTaskArgs* ctx = static_cast<HomeAssistantTaskArgs*>(arg);
    EntityStore* store = ctx->store;

    ESP_LOGI(TAG, "Waiting for wifi...");
    store_wait_for_wifi_up(store);
    ESP_LOGI(TAG, "Wifi is up, connecting...");

    esp_websocket_client_config_t client_config = {
        .uri = ctx->config->home_assistant_url,
        .disable_auto_reconnect = true,
        .cert_pem = ctx->config->root_ca,
    };
    home_assistant_context_t* hass = new home_assistant_context_t{};
    hass->store = store;
    hass->config = ctx->config;
    hass->client = esp_websocket_client_init(&client_config);
    hass->mutex = xSemaphoreCreateMutex();
    hass->event_id = 1;
    hass->entity_count = store->entity_count;
    hass->task = xTaskGetCurrentTaskHandle();
    for (uint8_t entity_idx = 0; entity_idx < store->entity_count; entity_idx++) {
        hass->entity_ids[entity_idx] = store->entities[entity_idx].entity_id;
        hass->entity_values[entity_idx] = -1;
    }

    esp_websocket_register_events(hass->client, WEBSOCKET_EVENT_ANY, hass_ws_event_handler, (void*)hass);
    esp_err_t err = esp_websocket_client_start(hass->client);
    ESP_LOGI(TAG, "esp_websocket_client_start returned: %s", esp_err_to_name(err));

    // Main loop monitoring the state and sending commands
    Command command;
    bool previous_connect_failed = false;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        xSemaphoreTake(hass->mutex, portMAX_DELAY);
        ConnState state = hass->state;
        xSemaphoreGive(hass->mutex);

        // Reconnect if the client was disconnected
        if (state == ConnState::InvalidCredentials || state == ConnState::ConnectionError) {
            ESP_LOGI(TAG, "Client is no longer connected, reconnecting...");

            // Close the client
            err = esp_websocket_client_close(hass->client, portMAX_DELAY);
            ESP_LOGI(TAG, "esp_websocket_client_close returned %s", esp_err_to_name(err));

            // Wait until wifi is up
            store_wait_for_wifi_up(store);

            // If previous connection failed, wait 10s to avoid spamming
            // home assistant
            if (previous_connect_failed) {
                ESP_LOGI(TAG, "Waiting 10 seconds");
                vTaskDelay(pdMS_TO_TICKS(HASS_RECONNECT_DELAY_MS));
            }
            previous_connect_failed = true;

            // Reset the state and restart the client
            ESP_LOGI(TAG, "attempting to reconnect to home assistant");
            hass->state = ConnState::Initializing;
            hass->event_id = 1;
            store_flush_pending_commands(hass->store);

            // Restart the client
            err = esp_websocket_client_start(hass->client);
            ESP_LOGI(TAG, "esp_websocket_client_start returned %s", esp_err_to_name(err));
        }

        // Launch commands
        else if (state == ConnState::Up) {
            previous_connect_failed = false;
            while (store_get_pending_command(store, &command)) {
                hass_send_command(hass, &command);
                store_ack_pending_command(store, &command);
                vTaskDelay(pdMS_TO_TICKS(HASS_TASK_SEND_DELAY_MS));
            }
        }
    }
}
