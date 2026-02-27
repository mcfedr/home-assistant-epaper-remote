#include <IPAddress.h> // fixes compilation issues with esp_websocket_client

#include "config.h"
#include "constants.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "managers/home_assistant.h"
#include "store.h"
#include <cJSON.h>
#include <cstdlib>
#include <cstring>

typedef struct home_assistant_context {
    EntityStore* store;
    Configuration* config;
    esp_websocket_client_handle_t client;
    ConnState state;
    SemaphoreHandle_t mutex;
    TaskHandle_t task;

    uint16_t event_id;         // counter to send events
    char* json_buffer;         // buffer for accumulating JSON data
    size_t json_buffer_len;    // current buffer length
    size_t json_buffer_cap;    // max buffer size
    uint8_t pending_discovery_command;
    bool dropping_oversized_payload;

    // Request IDs for discovery flow
    uint16_t floor_registry_request_id;
    uint16_t area_registry_request_id;
    uint16_t device_registry_request_id;
    uint16_t entity_registry_request_id;

    // Home Assistant sends updates by attribute only. We keep a local cache to
    // reconstruct a coherent value (on/off + brightness/percentage).
    uint8_t entity_count;
    const char* entity_ids[MAX_ENTITIES];
    bool entity_states[MAX_ENTITIES];
    int8_t entity_values[MAX_ENTITIES]; // -1 = unknown (handles binary-like entities)
    TickType_t last_command_sent_at_ms[MAX_ENTITIES];

    // Mapping floor_id -> floor index in store
    uint8_t floor_count;
    char floor_ids[MAX_FLOORS][MAX_ENTITY_ID_LEN];
    int8_t floor_store_indices[MAX_FLOORS];
    int8_t other_floor_idx;

    // Mapping area_id -> room index in store
    uint8_t area_count;
    char area_ids[MAX_ROOMS][MAX_ENTITY_ID_LEN];
    int8_t area_room_indices[MAX_ROOMS];

    // Mapping device_id -> room index in store
    uint16_t device_count;
    char device_ids[MAX_DEVICE_MAPPINGS][MAX_ENTITY_ID_LEN];
    int8_t device_room_indices[MAX_DEVICE_MAPPINGS];
} home_assistant_context_t;

static const char* TAG = "home_assistant";

enum DiscoveryCommand : uint8_t {
    DiscoveryCommandNone = 0,
    DiscoveryCommandRequestFloorRegistry = 1,
    DiscoveryCommandRequestAreaRegistry = 2,
    DiscoveryCommandRequestDeviceRegistry = 3,
    DiscoveryCommandRequestEntityRegistry = 4,
    DiscoveryCommandSubscribeEntities = 5,
};

static void hass_dispatch_discovery_command(home_assistant_context_t* hass);
void hass_cmd_subscribe(home_assistant_context_t* hass);

static void copy_string(char* dst, size_t dst_len, const char* src) {
    if (dst_len == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static void hass_reset_discovery_state(home_assistant_context_t* hass) {
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    hass->floor_registry_request_id = 0;
    hass->area_registry_request_id = 0;
    hass->device_registry_request_id = 0;
    hass->entity_registry_request_id = 0;
    hass->pending_discovery_command = DiscoveryCommandNone;
    hass->dropping_oversized_payload = false;
    hass->floor_count = 0;
    hass->area_count = 0;
    hass->device_count = 0;
    hass->entity_count = 0;
    hass->other_floor_idx = -1;
    memset(hass->floor_ids, 0, sizeof(hass->floor_ids));
    memset(hass->floor_store_indices, -1, sizeof(hass->floor_store_indices));
    memset(hass->area_ids, 0, sizeof(hass->area_ids));
    memset(hass->area_room_indices, -1, sizeof(hass->area_room_indices));
    memset(hass->device_ids, 0, sizeof(hass->device_ids));
    memset(hass->device_room_indices, -1, sizeof(hass->device_room_indices));
    memset(hass->entity_ids, 0, sizeof(hass->entity_ids));
    memset(hass->entity_states, 0, sizeof(hass->entity_states));
    memset(hass->entity_values, -1, sizeof(hass->entity_values));
    memset(hass->last_command_sent_at_ms, 0, sizeof(hass->last_command_sent_at_ms));
    xSemaphoreGive(hass->mutex);
}

static void hass_refresh_entities_from_store(home_assistant_context_t* hass) {
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    xSemaphoreTake(hass->store->mutex, portMAX_DELAY);

    hass->entity_count = hass->store->entity_count;
    for (uint8_t entity_idx = 0; entity_idx < hass->entity_count; entity_idx++) {
        hass->entity_ids[entity_idx] = hass->store->entities[entity_idx].entity_id;
        hass->entity_values[entity_idx] = -1;
        hass->entity_states[entity_idx] = false;
        hass->last_command_sent_at_ms[entity_idx] = 0;
    }

    xSemaphoreGive(hass->store->mutex);
    xSemaphoreGive(hass->mutex);
}

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
    char* request = cJSON_PrintUnformatted(root);
    esp_websocket_client_send_text(hass->client, request, strlen(request), portMAX_DELAY);
    cJSON_free(request);
    cJSON_Delete(root);
}

void hass_cmd_request_floor_registry(home_assistant_context_t* hass) {
    cJSON* root = cJSON_CreateObject();
    uint16_t request_id = hass_generate_event_id(hass);
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    hass->floor_registry_request_id = request_id;
    xSemaphoreGive(hass->mutex);

    cJSON_AddNumberToObject(root, "id", request_id);
    cJSON_AddStringToObject(root, "type", "config/floor_registry/list");

    char* request = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "Sending %s", request);
    esp_websocket_client_send_text(hass->client, request, strlen(request), portMAX_DELAY);
    cJSON_free(request);
    cJSON_Delete(root);
}

void hass_cmd_request_area_registry(home_assistant_context_t* hass) {
    cJSON* root = cJSON_CreateObject();
    uint16_t request_id = hass_generate_event_id(hass);
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    hass->area_registry_request_id = request_id;
    xSemaphoreGive(hass->mutex);

    cJSON_AddNumberToObject(root, "id", request_id);
    cJSON_AddStringToObject(root, "type", "config/area_registry/list");

    char* request = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "Sending %s", request);
    esp_websocket_client_send_text(hass->client, request, strlen(request), portMAX_DELAY);
    cJSON_free(request);
    cJSON_Delete(root);
}

void hass_cmd_request_entity_registry(home_assistant_context_t* hass) {
    cJSON* root = cJSON_CreateObject();
    uint16_t request_id = hass_generate_event_id(hass);
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    hass->entity_registry_request_id = request_id;
    xSemaphoreGive(hass->mutex);

    cJSON_AddNumberToObject(root, "id", request_id);
    cJSON_AddStringToObject(root, "type", "config/entity_registry/list_for_display");

    char* request = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "Sending %s", request);
    esp_websocket_client_send_text(hass->client, request, strlen(request), portMAX_DELAY);
    cJSON_free(request);
    cJSON_Delete(root);
}

void hass_cmd_request_device_registry(home_assistant_context_t* hass) {
    cJSON* root = cJSON_CreateObject();
    uint16_t request_id = hass_generate_event_id(hass);
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    hass->device_registry_request_id = request_id;
    xSemaphoreGive(hass->mutex);

    cJSON_AddNumberToObject(root, "id", request_id);
    cJSON_AddStringToObject(root, "type", "config/device_registry/list");

    char* request = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "Sending %s", request);
    esp_websocket_client_send_text(hass->client, request, strlen(request), portMAX_DELAY);
    cJSON_free(request);
    cJSON_Delete(root);
}

void hass_set_pending_discovery_command(home_assistant_context_t* hass, DiscoveryCommand command) {
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    hass->pending_discovery_command = command;
    xSemaphoreGive(hass->mutex);
    xTaskNotifyGive(hass->task);
}

static void hass_dispatch_discovery_command(home_assistant_context_t* hass) {
    DiscoveryCommand command = DiscoveryCommandNone;
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    command = static_cast<DiscoveryCommand>(hass->pending_discovery_command);
    hass->pending_discovery_command = DiscoveryCommandNone;
    xSemaphoreGive(hass->mutex);

    switch (command) {
    case DiscoveryCommandRequestFloorRegistry:
        hass_cmd_request_floor_registry(hass);
        break;
    case DiscoveryCommandRequestAreaRegistry:
        hass_cmd_request_area_registry(hass);
        break;
    case DiscoveryCommandRequestDeviceRegistry:
        hass_cmd_request_device_registry(hass);
        break;
    case DiscoveryCommandRequestEntityRegistry:
        hass_cmd_request_entity_registry(hass);
        break;
    case DiscoveryCommandSubscribeEntities:
        hass_cmd_subscribe(hass);
        break;
    case DiscoveryCommandNone:
    default:
        break;
    }
}

void hass_cmd_subscribe(home_assistant_context_t* hass) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", hass_generate_event_id(hass));
    cJSON_AddStringToObject(root, "type", "subscribe_entities");

    cJSON* entity_ids = cJSON_CreateArray();
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    for (uint8_t idx = 0; idx < hass->entity_count; idx++) {
        cJSON_AddItemToArray(entity_ids, cJSON_CreateString(hass->entity_ids[idx]));
    }
    xSemaphoreGive(hass->mutex);
    cJSON_AddItemToObject(root, "entity_ids", entity_ids);

    char* request = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "Sending %s", request);
    esp_websocket_client_send_text(hass->client, request, strlen(request), portMAX_DELAY);
    cJSON_free(request);
    cJSON_Delete(root);
}

int16_t hass_match_entity(home_assistant_context_t* hass, const char* key) {
    int16_t result = -1;
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    for (uint8_t i = 0; i < hass->entity_count; i++) {
        if (strcmp(key, hass->entity_ids[i]) == 0) {
            result = i;
            break;
        }
    }
    xSemaphoreGive(hass->mutex);
    return result;
}

int16_t hass_find_floor_for_floor_id(home_assistant_context_t* hass, const char* floor_id) {
    int16_t floor_idx = -1;
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    for (uint8_t idx = 0; idx < hass->floor_count; idx++) {
        if (strcmp(hass->floor_ids[idx], floor_id) == 0) {
            floor_idx = hass->floor_store_indices[idx];
            break;
        }
    }
    xSemaphoreGive(hass->mutex);
    return floor_idx;
}

int16_t hass_ensure_other_floor(home_assistant_context_t* hass) {
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    int8_t existing_idx = hass->other_floor_idx;
    xSemaphoreGive(hass->mutex);
    if (existing_idx >= 0) {
        return existing_idx;
    }

    int8_t floor_idx = store_add_floor(hass->store, "Other Areas");
    if (floor_idx < 0) {
        return -1;
    }

    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    if (hass->other_floor_idx < 0) {
        hass->other_floor_idx = floor_idx;
    } else {
        floor_idx = hass->other_floor_idx;
    }
    xSemaphoreGive(hass->mutex);

    return floor_idx;
}

int16_t hass_find_room_for_area(home_assistant_context_t* hass, const char* area_id) {
    int16_t room_idx = -1;
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    for (uint8_t idx = 0; idx < hass->area_count; idx++) {
        if (strcmp(hass->area_ids[idx], area_id) == 0) {
            room_idx = hass->area_room_indices[idx];
            break;
        }
    }
    xSemaphoreGive(hass->mutex);
    return room_idx;
}

int16_t hass_find_room_for_device(home_assistant_context_t* hass, const char* device_id) {
    int16_t room_idx = -1;
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    for (uint16_t idx = 0; idx < hass->device_count; idx++) {
        if (strcmp(hass->device_ids[idx], device_id) == 0) {
            room_idx = hass->device_room_indices[idx];
            break;
        }
    }
    xSemaphoreGive(hass->mutex);
    return room_idx;
}

void hass_parse_entity_update(home_assistant_context_t* hass, uint8_t widget_idx, cJSON* item) {
    bool entity_state = false;
    int8_t entity_value = -1;

    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    entity_state = hass->entity_states[widget_idx];
    entity_value = hass->entity_values[widget_idx];

    cJSON* state = cJSON_GetObjectItem(item, "s");
    if (cJSON_IsString(state)) {
        if (strcmp(state->valuestring, "on") == 0) {
            entity_state = true;
        } else if (strcmp(state->valuestring, "off") == 0) {
            entity_state = false;
        }
    }

    cJSON* attributes = cJSON_GetObjectItem(item, "a");
    if (cJSON_IsObject(attributes)) {
        cJSON* percentage = cJSON_GetObjectItem(attributes, "percentage");
        if (cJSON_IsNumber(percentage)) {
            entity_value = percentage->valueint;
        }

        cJSON* brightness = cJSON_GetObjectItem(attributes, "brightness");
        if (cJSON_IsNumber(brightness)) {
            entity_value = brightness->valueint * 100 / 254;
        }

        cJSON* off_brightness = cJSON_GetObjectItem(attributes, "off_brightness");
        if (cJSON_IsNumber(off_brightness)) {
            entity_value = off_brightness->valueint * 100 / 254;
        }
    }

    hass->entity_states[widget_idx] = entity_state;
    hass->entity_values[widget_idx] = entity_value;

    TickType_t now = xTaskGetTickCount();
    bool ignore_update =
        (now - hass->last_command_sent_at_ms[widget_idx]) < pdMS_TO_TICKS(HASS_IGNORE_UPDATE_DELAY_MS);

    const char* entity_id = hass->entity_ids[widget_idx];
    xSemaphoreGive(hass->mutex);

    if (ignore_update) {
        ESP_LOGI(TAG, "Ignoring update of entity %s", entity_id);
        return;
    }

    uint8_t value = 0;
    if (entity_state) {
        value = entity_value < 0 ? 1 : static_cast<uint8_t>(entity_value);
    }

    ESP_LOGI(TAG, "Setting value of widget %d to %d", widget_idx, value);
    store_update_value(hass->store, widget_idx, value);
}

void hass_handle_entity_update(home_assistant_context_t* hass, cJSON* event) {
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

    hass_update_state(hass, ConnState::Up);
}

void hass_parse_floor_registry(home_assistant_context_t* hass, cJSON* result) {
    if (!cJSON_IsArray(result)) {
        return;
    }

    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, result) {
        cJSON* floor_id_item = cJSON_GetObjectItem(item, "floor_id");
        if (!cJSON_IsString(floor_id_item)) {
            floor_id_item = cJSON_GetObjectItem(item, "id");
        }
        if (!cJSON_IsString(floor_id_item)) {
            floor_id_item = cJSON_GetObjectItem(item, "fi");
        }

        cJSON* name_item = cJSON_GetObjectItem(item, "name");
        if (!cJSON_IsString(name_item)) {
            name_item = cJSON_GetObjectItem(item, "n");
        }

        if (!cJSON_IsString(floor_id_item) || !cJSON_IsString(name_item)) {
            continue;
        }

        int8_t floor_idx = store_add_floor(hass->store, name_item->valuestring);
        if (floor_idx < 0) {
            ESP_LOGW(TAG, "Skipping floor %s: floor limit reached", floor_id_item->valuestring);
            continue;
        }

        xSemaphoreTake(hass->mutex, portMAX_DELAY);
        if (hass->floor_count < MAX_FLOORS) {
            uint8_t idx = hass->floor_count++;
            copy_string(hass->floor_ids[idx], sizeof(hass->floor_ids[idx]), floor_id_item->valuestring);
            hass->floor_store_indices[idx] = floor_idx;
        }
        xSemaphoreGive(hass->mutex);
    }
}

void hass_parse_area_registry(home_assistant_context_t* hass, cJSON* result) {
    if (!cJSON_IsArray(result)) {
        return;
    }

    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, result) {
        cJSON* area_id_item = cJSON_GetObjectItem(item, "area_id");
        if (!cJSON_IsString(area_id_item)) {
            area_id_item = cJSON_GetObjectItem(item, "ai");
        }

        cJSON* name_item = cJSON_GetObjectItem(item, "name");
        if (!cJSON_IsString(name_item)) {
            name_item = cJSON_GetObjectItem(item, "n");
        }

        cJSON* floor_id_item = cJSON_GetObjectItem(item, "floor_id");
        if (!cJSON_IsString(floor_id_item)) {
            floor_id_item = cJSON_GetObjectItem(item, "fl");
        }

        if (!cJSON_IsString(area_id_item) || !cJSON_IsString(name_item)) {
            continue;
        }

        int16_t floor_idx = -1;
        if (cJSON_IsString(floor_id_item)) {
            floor_idx = hass_find_floor_for_floor_id(hass, floor_id_item->valuestring);
        }
        if (floor_idx < 0) {
            floor_idx = hass_ensure_other_floor(hass);
        }
        if (floor_idx < 0) {
            ESP_LOGW(TAG, "Skipping area %s: no floor slot available", area_id_item->valuestring);
            continue;
        }

        int8_t room_idx = store_add_room(hass->store, name_item->valuestring, static_cast<int8_t>(floor_idx));
        if (room_idx < 0) {
            ESP_LOGW(TAG, "Skipping area %s: room limit reached", area_id_item->valuestring);
            continue;
        }

        xSemaphoreTake(hass->mutex, portMAX_DELAY);
        if (hass->area_count < MAX_ROOMS) {
            uint8_t area_idx = hass->area_count++;
            copy_string(hass->area_ids[area_idx], sizeof(hass->area_ids[area_idx]), area_id_item->valuestring);
            hass->area_room_indices[area_idx] = room_idx;
        }
        xSemaphoreGive(hass->mutex);
    }
}

void hass_parse_device_registry(home_assistant_context_t* hass, cJSON* result) {
    if (!cJSON_IsArray(result)) {
        return;
    }

    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, result) {
        cJSON* device_id_item = cJSON_GetObjectItem(item, "id");
        cJSON* area_id_item = cJSON_GetObjectItem(item, "area_id");
        if (!cJSON_IsString(device_id_item) || !cJSON_IsString(area_id_item)) {
            continue;
        }

        int16_t room_idx = hass_find_room_for_area(hass, area_id_item->valuestring);
        if (room_idx < 0) {
            continue;
        }

        xSemaphoreTake(hass->mutex, portMAX_DELAY);
        if (hass->device_count < MAX_DEVICE_MAPPINGS) {
            uint16_t device_idx = hass->device_count++;
            copy_string(hass->device_ids[device_idx], sizeof(hass->device_ids[device_idx]), device_id_item->valuestring);
            hass->device_room_indices[device_idx] = room_idx;
        }
        xSemaphoreGive(hass->mutex);
    }
}

void hass_parse_entity_registry(home_assistant_context_t* hass, cJSON* result) {
    cJSON* entities = nullptr;
    if (cJSON_IsArray(result)) {
        entities = result;
    } else if (cJSON_IsObject(result)) {
        // list_for_display response: { entity_categories: {...}, entities: [...] }
        cJSON* compact_entities = cJSON_GetObjectItem(result, "entities");
        if (cJSON_IsArray(compact_entities)) {
            entities = compact_entities;
        }
    }

    if (!cJSON_IsArray(entities)) {
        return;
    }

    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, entities) {
        cJSON* entity_id_item = cJSON_GetObjectItem(item, "entity_id");
        if (!cJSON_IsString(entity_id_item)) {
            entity_id_item = cJSON_GetObjectItem(item, "ei");
        }

        cJSON* area_id_item = cJSON_GetObjectItem(item, "area_id");
        if (!cJSON_IsString(area_id_item)) {
            area_id_item = cJSON_GetObjectItem(item, "ai");
        }
        cJSON* device_id_item = cJSON_GetObjectItem(item, "device_id");
        if (!cJSON_IsString(device_id_item)) {
            device_id_item = cJSON_GetObjectItem(item, "di");
        }

        cJSON* hidden_by_item = cJSON_GetObjectItem(item, "hidden_by");
        cJSON* hidden_bool_item = cJSON_GetObjectItem(item, "hb");
        cJSON* disabled_by_item = cJSON_GetObjectItem(item, "disabled_by");

        if (!cJSON_IsString(entity_id_item) || strncmp(entity_id_item->valuestring, "light.", 6) != 0) {
            continue;
        }
        if (cJSON_IsString(hidden_by_item) || cJSON_IsString(disabled_by_item) || cJSON_IsTrue(hidden_bool_item)) {
            continue;
        }

        int16_t room_idx = -1;
        if (cJSON_IsString(area_id_item)) {
            room_idx = hass_find_room_for_area(hass, area_id_item->valuestring);
        }
        if (room_idx < 0 && cJSON_IsString(device_id_item)) {
            room_idx = hass_find_room_for_device(hass, device_id_item->valuestring);
        }
        if (room_idx < 0) {
            continue;
        }

        const char* display_name = nullptr;
        cJSON* name_item = cJSON_GetObjectItem(item, "name");
        cJSON* original_name_item = cJSON_GetObjectItem(item, "original_name");
        cJSON* compact_name_item = cJSON_GetObjectItem(item, "en");
        if (cJSON_IsString(name_item) && name_item->valuestring[0] != '\0') {
            display_name = name_item->valuestring;
        } else if (cJSON_IsString(original_name_item) && original_name_item->valuestring[0] != '\0') {
            display_name = original_name_item->valuestring;
        } else if (cJSON_IsString(compact_name_item) && compact_name_item->valuestring[0] != '\0') {
            display_name = compact_name_item->valuestring;
        }

        EntityConfig entity = {
            .entity_id = entity_id_item->valuestring,
            .command_type = CommandType::SetLightBrightnessPercentage,
        };
        if (store_add_entity_to_room(hass->store, room_idx, entity, display_name) < 0) {
            ESP_LOGW(TAG, "Skipping light %s: limits reached", entity_id_item->valuestring);
        }
    }
}

void hass_start_discovery(home_assistant_context_t* hass) {
    ESP_LOGI(TAG, "Starting room/light discovery");
    hass_reset_discovery_state(hass);
    store_begin_room_sync(hass->store);
    hass_set_pending_discovery_command(hass, DiscoveryCommandRequestFloorRegistry);
}

void hass_handle_result(home_assistant_context_t* hass, cJSON* json) {
    cJSON* id_item = cJSON_GetObjectItem(json, "id");
    cJSON* success_item = cJSON_GetObjectItem(json, "success");
    cJSON* result_item = cJSON_GetObjectItem(json, "result");
    if (!cJSON_IsNumber(id_item) || !cJSON_IsBool(success_item)) {
        return;
    }

    uint16_t response_id = static_cast<uint16_t>(id_item->valueint);
    bool success = cJSON_IsTrue(success_item);

    uint16_t floor_request_id = 0;
    uint16_t area_request_id = 0;
    uint16_t device_request_id = 0;
    uint16_t entity_request_id = 0;
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    floor_request_id = hass->floor_registry_request_id;
    area_request_id = hass->area_registry_request_id;
    device_request_id = hass->device_registry_request_id;
    entity_request_id = hass->entity_registry_request_id;
    xSemaphoreGive(hass->mutex);

    if (response_id == floor_request_id) {
        if (!success) {
            ESP_LOGW(TAG, "Floor registry request failed, using only 'Other Areas'");
        } else {
            hass_parse_floor_registry(hass, result_item);
        }
        hass_set_pending_discovery_command(hass, DiscoveryCommandRequestAreaRegistry);
        return;
    }

    if (response_id == area_request_id) {
        if (!success) {
            ESP_LOGE(TAG, "Area registry request failed");
            hass_update_state(hass, ConnState::ConnectionError);
            return;
        }
        hass_parse_area_registry(hass, result_item);
        hass_set_pending_discovery_command(hass, DiscoveryCommandRequestDeviceRegistry);
        return;
    }

    if (response_id == device_request_id) {
        if (!success) {
            ESP_LOGE(TAG, "Device registry request failed");
            hass_update_state(hass, ConnState::ConnectionError);
            return;
        }
        hass_parse_device_registry(hass, result_item);
        hass_set_pending_discovery_command(hass, DiscoveryCommandRequestEntityRegistry);
        return;
    }

    if (response_id == entity_request_id) {
        if (!success) {
            ESP_LOGE(TAG, "Entity registry request failed");
            hass_update_state(hass, ConnState::ConnectionError);
            return;
        }

        hass_parse_entity_registry(hass, result_item);
        hass_refresh_entities_from_store(hass);
        store_finish_room_sync(hass->store);

        xSemaphoreTake(hass->mutex, portMAX_DELAY);
        uint8_t entity_count = hass->entity_count;
        xSemaphoreGive(hass->mutex);
        if (entity_count > 0) {
            hass_set_pending_discovery_command(hass, DiscoveryCommandSubscribeEntities);
        } else {
            ESP_LOGW(TAG, "No light entities discovered for mapped rooms");
            hass_update_state(hass, ConnState::Up);
        }
        return;
    }
}

void hass_handle_server_payload(home_assistant_context_t* hass, cJSON* json) {
    cJSON* type_item = cJSON_GetObjectItem(json, "type");
    if (!cJSON_IsString(type_item) || type_item->valuestring == nullptr) {
        return;
    }

    ESP_LOGI(TAG, "Received Home Assistant message of type %s", type_item->valuestring);
    if (strcmp(type_item->valuestring, "auth_required") == 0) {
        ESP_LOGI(TAG, "Logging in to home assistant...");
        hass_cmd_authenticate(hass);
    } else if (strcmp(type_item->valuestring, "auth_invalid") == 0) {
        ESP_LOGI(TAG, "Updating state to InvalidCredentials");
        hass_update_state(hass, ConnState::InvalidCredentials);
    } else if (strcmp(type_item->valuestring, "auth_ok") == 0) {
        ESP_LOGI(TAG, "Authentication successful, loading rooms and entities");
        hass_start_discovery(hass);
    } else if (strcmp(type_item->valuestring, "result") == 0) {
        hass_handle_result(hass, json);
    } else if (strcmp(type_item->valuestring, "event") == 0) {
        cJSON* event = cJSON_GetObjectItem(json, "event");
        if (cJSON_IsObject(event)) {
            hass_handle_entity_update(hass, event);
        }
    } else {
        ESP_LOGI(TAG, "Ignoring HASS event type %s", type_item->valuestring);
    }
}

static void hass_ws_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
    home_assistant_context_t* hass = static_cast<home_assistant_context_t*>(handler_args);
    esp_websocket_event_data_t* data = static_cast<esp_websocket_event_data_t*>(event_data);

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
            xSemaphoreTake(hass->mutex, portMAX_DELAY);

            if (data->payload_offset == 0) {
                hass->json_buffer_len = 0;
                hass->dropping_oversized_payload = false;
            }

            if (hass->dropping_oversized_payload) {
                xSemaphoreGive(hass->mutex);
                return;
            }

            const size_t chunk_end = data->payload_offset + data->data_len;
            if (hass->json_buffer == nullptr || chunk_end > hass->json_buffer_cap) {
                ESP_LOGE(TAG, "JSON buffer overflow, discarding message payload_len=%d", data->payload_len);
                hass->dropping_oversized_payload = true;
                hass->json_buffer_len = 0;
                xSemaphoreGive(hass->mutex);
                return;
            }

            memcpy(hass->json_buffer + data->payload_offset, data->data_ptr, data->data_len);
            if (chunk_end > hass->json_buffer_len) {
                hass->json_buffer_len = chunk_end;
            }

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
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", hass_generate_event_id(hass));
    cJSON_AddStringToObject(root, "type", "call_service");

    cJSON* service_data = cJSON_CreateObject();
    cJSON_AddStringToObject(service_data, "entity_id", cmd->entity_id);

    switch (cmd->type) {
    case CommandType::SetLightBrightnessPercentage:
        cJSON_AddStringToObject(root, "domain", "light");
        if (cmd->value == 0) {
            cJSON_AddStringToObject(root, "service", "turn_off");
        } else {
            cJSON_AddStringToObject(root, "service", "turn_on");
            cJSON_AddNumberToObject(service_data, "brightness_pct", cmd->value);
        }
        break;
    case CommandType::SetFanSpeedPercentage:
        cJSON_AddStringToObject(root, "domain", "fan");
        cJSON_AddStringToObject(root, "service", "set_percentage");
        cJSON_AddNumberToObject(service_data, "percentage", cmd->value);
        break;
    case CommandType::SwitchOnOff:
        cJSON_AddStringToObject(root, "domain", "switch");
        cJSON_AddStringToObject(root, "service", cmd->value == 0 ? "turn_off" : "turn_on");
        break;
    case CommandType::AutomationOnOff:
        cJSON_AddStringToObject(root, "domain", "automation");
        cJSON_AddStringToObject(root, "service", cmd->value == 0 ? "turn_off" : "turn_on");
        break;
    default:
        ESP_LOGI(TAG, "Service type not supported");
        cJSON_Delete(service_data);
        cJSON_Delete(root);
        return;
    }
    cJSON_AddItemToObject(root, "service_data", service_data);

    char* request = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "Sending %s", request);

    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    hass->last_command_sent_at_ms[cmd->entity_idx] = xTaskGetTickCount();
    xSemaphoreGive(hass->mutex);
    esp_websocket_client_send_text(hass->client, request, strlen(request), portMAX_DELAY);
    cJSON_free(request);
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
    hass->task = xTaskGetCurrentTaskHandle();
    hass->json_buffer_cap = HASS_MAX_JSON_BUFFER;
    hass->json_buffer = static_cast<char*>(heap_caps_malloc(hass->json_buffer_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (hass->json_buffer == nullptr) {
        ESP_LOGW(TAG, "PSRAM allocation failed, falling back to regular heap for JSON buffer");
        hass->json_buffer = static_cast<char*>(malloc(hass->json_buffer_cap));
    }
    if (hass->json_buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate JSON buffer, cannot start Home Assistant client");
        hass_update_state(hass, ConnState::ConnectionError);
        vTaskDelete(nullptr);
    }
    hass->event_id = 1;
    hass_reset_discovery_state(hass);

    esp_websocket_register_events(hass->client, WEBSOCKET_EVENT_ANY, hass_ws_event_handler, static_cast<void*>(hass));
    esp_err_t err = esp_websocket_client_start(hass->client);
    ESP_LOGI(TAG, "esp_websocket_client_start returned: %s", esp_err_to_name(err));

    Command command;
    bool previous_connect_failed = false;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        xSemaphoreTake(hass->mutex, portMAX_DELAY);
        ConnState state = hass->state;
        xSemaphoreGive(hass->mutex);

        if (state == ConnState::InvalidCredentials || state == ConnState::ConnectionError) {
            ESP_LOGI(TAG, "Client is no longer connected, reconnecting...");

            err = esp_websocket_client_close(hass->client, portMAX_DELAY);
            ESP_LOGI(TAG, "esp_websocket_client_close returned %s", esp_err_to_name(err));

            store_wait_for_wifi_up(store);

            if (previous_connect_failed) {
                ESP_LOGI(TAG, "Waiting 10 seconds");
                vTaskDelay(pdMS_TO_TICKS(HASS_RECONNECT_DELAY_MS));
            }
            previous_connect_failed = true;

            ESP_LOGI(TAG, "Attempting to reconnect to home assistant");
            xSemaphoreTake(hass->mutex, portMAX_DELAY);
            hass->state = ConnState::Initializing;
            hass->event_id = 1;
            xSemaphoreGive(hass->mutex);
            hass_reset_discovery_state(hass);
            store_flush_pending_commands(hass->store);

            err = esp_websocket_client_start(hass->client);
            ESP_LOGI(TAG, "esp_websocket_client_start returned %s", esp_err_to_name(err));
        } else {
            hass_dispatch_discovery_command(hass);
        }

        if (state == ConnState::Up) {
            previous_connect_failed = false;
            while (store_get_pending_command(store, &command)) {
                hass_send_command(hass, &command);
                store_ack_pending_command(store, &command);
                vTaskDelay(pdMS_TO_TICKS(HASS_TASK_SEND_DELAY_MS));
            }
        }
    }
}
