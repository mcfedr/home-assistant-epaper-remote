#include <IPAddress.h> // fixes compilation issues with esp_websocket_client

#include "config.h"
#include "climate_value.h"
#include "constants.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "managers/home_assistant.h"
#include "store.h"
#include <cJSON.h>
#include <cctype>
#include <ctime>
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
    uint8_t entity_modes[MAX_ENTITIES]; // 0/1 for lights, ClimateMode value for climate
    int8_t entity_values[MAX_ENTITIES]; // brightness percentage or climate temp steps (-1 unknown)
    TickType_t last_command_sent_at_ms[MAX_ENTITIES];

    // Standby data entity IDs
    char standby_weather_entity_id[MAX_ENTITY_ID_LEN];
    char standby_energy_solar_entity_id[MAX_ENTITY_ID_LEN];
    char standby_energy_grid_entity_id[MAX_ENTITY_ID_LEN];
    char standby_energy_battery_usage_entity_id[MAX_ENTITY_ID_LEN];
    char standby_energy_battery_soc_entity_id[MAX_ENTITY_ID_LEN];
    char standby_energy_house_entity_id[MAX_ENTITY_ID_LEN];
    char standby_energy_grid_export_entity_id[MAX_ENTITY_ID_LEN];
    char standby_energy_battery_charge_entity_id[MAX_ENTITY_ID_LEN];
    uint16_t weather_forecast_request_id;
    bool weather_forecast_requested;
    uint32_t last_weather_forecast_request_ms;
    uint16_t energy_prefs_request_id;
    bool standby_energy_house_computed;

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

    struct StandbyEnergySeries {
        uint8_t count;
        char entity_ids[8][MAX_ENTITY_ID_LEN];
        bool values_valid[8];
        float values[8];
    } standby_solar_series, standby_grid_in_series, standby_grid_out_series, standby_battery_out_series, standby_battery_in_series;
} home_assistant_context_t;

static const char* TAG = "home_assistant";

enum DiscoveryCommand : uint8_t {
    DiscoveryCommandNone = 0,
    DiscoveryCommandRequestFloorRegistry = 1,
    DiscoveryCommandRequestAreaRegistry = 2,
    DiscoveryCommandRequestDeviceRegistry = 3,
    DiscoveryCommandRequestEntityRegistry = 4,
    DiscoveryCommandRequestEnergyPrefs = 5,
    DiscoveryCommandSubscribeEntities = 6,
};

static void hass_dispatch_discovery_command(home_assistant_context_t* hass);
void hass_cmd_request_energy_prefs(home_assistant_context_t* hass);
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

static const char* get_optional_string(cJSON* object, const char* key, const char* compact_key) {
    cJSON* item = cJSON_GetObjectItem(object, key);
    if (!cJSON_IsString(item) && compact_key) {
        item = cJSON_GetObjectItem(object, compact_key);
    }
    if (!cJSON_IsString(item) || !item->valuestring) {
        return nullptr;
    }
    return item->valuestring;
}

static const char* hass_entity_display_name_from_registry(cJSON* item) {
    cJSON* name_item = cJSON_GetObjectItem(item, "name");
    cJSON* original_name_item = cJSON_GetObjectItem(item, "original_name");
    cJSON* compact_name_item = cJSON_GetObjectItem(item, "en");
    if (cJSON_IsString(name_item) && name_item->valuestring[0] != '\0') {
        return name_item->valuestring;
    }
    if (cJSON_IsString(original_name_item) && original_name_item->valuestring[0] != '\0') {
        return original_name_item->valuestring;
    }
    if (cJSON_IsString(compact_name_item) && compact_name_item->valuestring[0] != '\0') {
        return compact_name_item->valuestring;
    }
    return nullptr;
}

static bool has_entity_id(const char* entity_id) {
    return entity_id && entity_id[0] != '\0';
}

static bool has_statistic_like_id(const char* statistic_id) {
    return has_entity_id(statistic_id) && strchr(statistic_id, '.') != nullptr;
}

static void copy_optional_entity_id(char* dst, size_t dst_len, const char* src) {
    if (has_entity_id(src)) {
        copy_string(dst, dst_len, src);
    } else {
        dst[0] = '\0';
    }
}

static bool parse_state_float(cJSON* state_item, float* out_value) {
    if (cJSON_IsNumber(state_item)) {
        *out_value = static_cast<float>(state_item->valuedouble);
        return true;
    }
    if (!cJSON_IsString(state_item) || !state_item->valuestring) {
        return false;
    }

    const char* value = state_item->valuestring;
    if (strcmp(value, "unknown") == 0 || strcmp(value, "unavailable") == 0 || strcmp(value, "none") == 0 ||
        strcmp(value, "None") == 0) {
        return false;
    }

    char* end = nullptr;
    float parsed = strtof(value, &end);
    if (end == value || (end && *end != '\0')) {
        return false;
    }

    *out_value = parsed;
    return true;
}

static void standby_energy_series_reset(home_assistant_context_t::StandbyEnergySeries* series) {
    if (!series) {
        return;
    }
    series->count = 0;
    memset(series->entity_ids, 0, sizeof(series->entity_ids));
    memset(series->values_valid, 0, sizeof(series->values_valid));
    memset(series->values, 0, sizeof(series->values));
}

static bool standby_energy_series_add_entity(home_assistant_context_t::StandbyEnergySeries* series, const char* entity_id) {
    if (!series || !has_statistic_like_id(entity_id)) {
        return false;
    }

    for (uint8_t idx = 0; idx < series->count; idx++) {
        if (strcmp(series->entity_ids[idx], entity_id) == 0) {
            return false;
        }
    }

    const size_t max_count = sizeof(series->entity_ids) / sizeof(series->entity_ids[0]);
    if (series->count >= max_count) {
        return false;
    }

    copy_string(series->entity_ids[series->count], sizeof(series->entity_ids[series->count]), entity_id);
    series->values_valid[series->count] = false;
    series->values[series->count] = 0.0f;
    series->count++;
    return true;
}

static int8_t standby_energy_series_find(const home_assistant_context_t::StandbyEnergySeries* series, const char* entity_id) {
    if (!series || !entity_id) {
        return -1;
    }

    for (uint8_t idx = 0; idx < series->count; idx++) {
        if (strcmp(series->entity_ids[idx], entity_id) == 0) {
            return static_cast<int8_t>(idx);
        }
    }
    return -1;
}

static bool standby_energy_series_set_value(home_assistant_context_t::StandbyEnergySeries* series, int8_t idx, bool valid, float value) {
    if (!series || idx < 0 || static_cast<uint8_t>(idx) >= series->count) {
        return false;
    }

    bool changed = false;
    if (series->values_valid[idx] != valid) {
        series->values_valid[idx] = valid;
        changed = true;
    }
    if (valid && (series->values[idx] < value - 0.05f || series->values[idx] > value + 0.05f)) {
        series->values[idx] = value;
        changed = true;
    }
    return changed;
}

static bool standby_energy_series_total(const home_assistant_context_t::StandbyEnergySeries* series, float* total) {
    if (!series || series->count == 0 || !total) {
        return false;
    }

    float sum = 0.0f;
    for (uint8_t idx = 0; idx < series->count; idx++) {
        if (!series->values_valid[idx]) {
            return false;
        }
        sum += series->values[idx];
    }
    *total = sum;
    return true;
}

static void forecast_weekday_label_from_datetime(const char* datetime_text, char* out, size_t out_len) {
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!datetime_text || strlen(datetime_text) < 10) {
        return;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    if (sscanf(datetime_text, "%d-%d-%d", &year, &month, &day) != 3) {
        return;
    }

    tm date = {};
    date.tm_year = year - 1900;
    date.tm_mon = month - 1;
    date.tm_mday = day;
    date.tm_hour = 12;
    if (mktime(&date) == static_cast<time_t>(-1)) {
        return;
    }

    static const char* kWeekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const int weekday = date.tm_wday;
    if (weekday < 0 || weekday > 6) {
        return;
    }
    copy_string(out, out_len, kWeekdays[weekday]);
}

static bool parse_forecast_day(cJSON* day_item, StandbyForecastDay* out_day) {
    if (!cJSON_IsObject(day_item) || !out_day) {
        return false;
    }

    *out_day = {};
    cJSON* datetime_item = cJSON_GetObjectItem(day_item, "datetime");
    if (!cJSON_IsString(datetime_item)) {
        datetime_item = cJSON_GetObjectItem(day_item, "date");
    }
    if (cJSON_IsString(datetime_item) && datetime_item->valuestring) {
        forecast_weekday_label_from_datetime(datetime_item->valuestring, out_day->day_label, sizeof(out_day->day_label));
    }

    cJSON* condition_item = cJSON_GetObjectItem(day_item, "condition");
    if (cJSON_IsString(condition_item) && condition_item->valuestring) {
        copy_string(out_day->condition, sizeof(out_day->condition), condition_item->valuestring);
    }

    cJSON* high_item = cJSON_GetObjectItem(day_item, "temperature");
    if (cJSON_IsNumber(high_item)) {
        out_day->high_valid = true;
        out_day->high_c = static_cast<float>(high_item->valuedouble);
    }

    cJSON* low_item = cJSON_GetObjectItem(day_item, "templow");
    if (!cJSON_IsNumber(low_item)) {
        low_item = cJSON_GetObjectItem(day_item, "temperature_low");
    }
    if (!cJSON_IsNumber(low_item)) {
        low_item = cJSON_GetObjectItem(day_item, "low_temperature");
    }
    if (cJSON_IsNumber(low_item)) {
        out_day->low_valid = true;
        out_day->low_c = static_cast<float>(low_item->valuedouble);
    }

    return out_day->condition[0] != '\0' || out_day->high_valid || out_day->low_valid;
}

static uint8_t parse_forecast_days(cJSON* forecast_array, StandbyForecastDay* days, uint8_t max_days) {
    if (!cJSON_IsArray(forecast_array) || !days || max_days == 0) {
        return 0;
    }

    uint8_t day_count = 0;
    cJSON* day_item = nullptr;
    cJSON_ArrayForEach(day_item, forecast_array) {
        if (day_count >= max_days) {
            break;
        }
        StandbyForecastDay parsed_day = {};
        if (!parse_forecast_day(day_item, &parsed_day)) {
            continue;
        }
        days[day_count++] = parsed_day;
    }
    return day_count;
}

static bool contains_case_insensitive(const char* haystack, const char* needle) {
    if (!haystack || !needle || needle[0] == '\0') {
        return false;
    }

    const size_t needle_len = strlen(needle);
    const size_t haystack_len = strlen(haystack);
    if (needle_len > haystack_len) {
        return false;
    }

    for (size_t i = 0; i + needle_len <= haystack_len; i++) {
        bool match = true;
        for (size_t j = 0; j < needle_len; j++) {
            const char a = static_cast<char>(tolower(static_cast<unsigned char>(haystack[i + j])));
            const char b = static_cast<char>(tolower(static_cast<unsigned char>(needle[j])));
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

static bool cover_is_group_like_name(const char* text) {
    if (!text) {
        return false;
    }
    return contains_case_insensitive(text, "group") || contains_case_insensitive(text, "_group") ||
           contains_case_insensitive(text, "all_") || contains_case_insensitive(text, "_all") ||
           contains_case_insensitive(text, " all ") || contains_case_insensitive(text, "covers") ||
           contains_case_insensitive(text, "shutters");
}

static bool cover_is_projector_exception(const char* entity_id, const char* display_name) {
    return contains_case_insensitive(entity_id, "projector") || contains_case_insensitive(display_name, "projector");
}

static bool cover_entity_should_be_included(cJSON* item, const char* entity_id, const char* display_name) {
    if (cover_is_projector_exception(entity_id, display_name)) {
        return true;
    }

    const char* platform = get_optional_string(item, "platform", "pl");
    const char* integration = get_optional_string(item, "integration", "it");
    if ((platform && strcmp(platform, "group") == 0) || (integration && strcmp(integration, "group") == 0)) {
        return true;
    }

    return cover_is_group_like_name(entity_id) || cover_is_group_like_name(display_name);
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
    memset(hass->entity_modes, 0, sizeof(hass->entity_modes));
    memset(hass->entity_values, -1, sizeof(hass->entity_values));
    memset(hass->last_command_sent_at_ms, 0, sizeof(hass->last_command_sent_at_ms));
    copy_optional_entity_id(hass->standby_weather_entity_id, sizeof(hass->standby_weather_entity_id), hass->config->weather_entity_id);
    copy_optional_entity_id(hass->standby_energy_solar_entity_id, sizeof(hass->standby_energy_solar_entity_id),
                            hass->config->energy_solar_entity_id);
    copy_optional_entity_id(hass->standby_energy_grid_entity_id, sizeof(hass->standby_energy_grid_entity_id), hass->config->energy_grid_entity_id);
    copy_optional_entity_id(hass->standby_energy_battery_usage_entity_id, sizeof(hass->standby_energy_battery_usage_entity_id),
                            hass->config->energy_battery_usage_entity_id);
    copy_optional_entity_id(hass->standby_energy_battery_soc_entity_id, sizeof(hass->standby_energy_battery_soc_entity_id),
                            hass->config->energy_battery_soc_entity_id);
    copy_optional_entity_id(hass->standby_energy_house_entity_id, sizeof(hass->standby_energy_house_entity_id),
                            hass->config->energy_house_entity_id);
    hass->standby_energy_grid_export_entity_id[0] = '\0';
    hass->standby_energy_battery_charge_entity_id[0] = '\0';
    hass->energy_prefs_request_id = 0;
    hass->standby_energy_house_computed = false;
    standby_energy_series_reset(&hass->standby_solar_series);
    standby_energy_series_reset(&hass->standby_grid_in_series);
    standby_energy_series_reset(&hass->standby_grid_out_series);
    standby_energy_series_reset(&hass->standby_battery_out_series);
    standby_energy_series_reset(&hass->standby_battery_in_series);
    standby_energy_series_add_entity(&hass->standby_solar_series, hass->standby_energy_solar_entity_id);
    standby_energy_series_add_entity(&hass->standby_grid_in_series, hass->standby_energy_grid_entity_id);
    standby_energy_series_add_entity(&hass->standby_battery_out_series, hass->standby_energy_battery_usage_entity_id);
    hass->weather_forecast_request_id = 0;
    hass->weather_forecast_requested = false;
    hass->last_weather_forecast_request_ms = 0;
    xSemaphoreGive(hass->mutex);
}

static void hass_refresh_entities_from_store(home_assistant_context_t* hass) {
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    xSemaphoreTake(hass->store->mutex, portMAX_DELAY);

    hass->entity_count = hass->store->entity_count;
    for (uint8_t entity_idx = 0; entity_idx < hass->entity_count; entity_idx++) {
        hass->entity_ids[entity_idx] = hass->store->entities[entity_idx].entity_id;
        hass->entity_modes[entity_idx] = 0;
        hass->entity_values[entity_idx] = -1;
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
    case DiscoveryCommandRequestEnergyPrefs:
        hass_cmd_request_energy_prefs(hass);
        break;
    case DiscoveryCommandSubscribeEntities:
        hass_cmd_subscribe(hass);
        break;
    case DiscoveryCommandNone:
    default:
        break;
    }
}

static bool entity_id_already_added(const char* entity_id, const char* const* list, uint8_t list_count) {
    for (uint8_t i = 0; i < list_count; i++) {
        if (strcmp(list[i], entity_id) == 0) {
            return true;
        }
    }
    return false;
}

static void hass_cmd_request_weather_forecast(home_assistant_context_t* hass) {
    char weather_entity_id[MAX_ENTITY_ID_LEN] = {};
    uint16_t request_id = 0;

    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    if (!has_entity_id(hass->standby_weather_entity_id)) {
        xSemaphoreGive(hass->mutex);
        return;
    }
    if (hass->weather_forecast_requested) {
        xSemaphoreGive(hass->mutex);
        return;
    }

    copy_string(weather_entity_id, sizeof(weather_entity_id), hass->standby_weather_entity_id);
    request_id = hass->event_id++;
    hass->weather_forecast_request_id = request_id;
    hass->weather_forecast_requested = true;
    hass->last_weather_forecast_request_ms = static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
    xSemaphoreGive(hass->mutex);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", request_id);
    cJSON_AddStringToObject(root, "type", "call_service");
    cJSON_AddStringToObject(root, "domain", "weather");
    cJSON_AddStringToObject(root, "service", "get_forecasts");
    cJSON_AddBoolToObject(root, "return_response", true);

    cJSON* service_data = cJSON_CreateObject();
    cJSON_AddStringToObject(service_data, "type", "daily");
    cJSON_AddStringToObject(service_data, "entity_id", weather_entity_id);
    cJSON_AddItemToObject(root, "service_data", service_data);

    cJSON* target = cJSON_CreateObject();
    cJSON_AddStringToObject(target, "entity_id", weather_entity_id);
    cJSON_AddItemToObject(root, "target", target);

    char* request = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "Requesting weather forecast for %s", weather_entity_id);
    esp_websocket_client_send_text(hass->client, request, strlen(request), portMAX_DELAY);
    cJSON_free(request);
    cJSON_Delete(root);
}

void hass_cmd_request_energy_prefs(home_assistant_context_t* hass) {
    cJSON* root = cJSON_CreateObject();
    uint16_t request_id = hass_generate_event_id(hass);
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    hass->energy_prefs_request_id = request_id;
    xSemaphoreGive(hass->mutex);

    cJSON_AddNumberToObject(root, "id", request_id);
    cJSON_AddStringToObject(root, "type", "energy/get_prefs");

    char* request = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "Requesting energy preferences from Home Assistant");
    esp_websocket_client_send_text(hass->client, request, strlen(request), portMAX_DELAY);
    cJSON_free(request);
    cJSON_Delete(root);
}

static void hass_add_standby_entity_id(cJSON* entity_ids,
                                       const char* entity_id,
                                       const char** added_ids,
                                       size_t max_added_ids,
                                       uint16_t* added_id_count) {
    if (!has_entity_id(entity_id) || !added_id_count) {
        return;
    }
    if (*added_id_count >= max_added_ids) {
        return;
    }
    if (entity_id_already_added(entity_id, added_ids, static_cast<uint8_t>(*added_id_count))) {
        return;
    }

    cJSON_AddItemToArray(entity_ids, cJSON_CreateString(entity_id));
    ESP_LOGI(TAG, "Subscribing standby entity %s", entity_id);
    added_ids[*added_id_count] = entity_id;
    (*added_id_count)++;
}

static void hass_add_standby_series_ids(cJSON* entity_ids,
                                        const home_assistant_context_t::StandbyEnergySeries* series,
                                        const char** added_ids,
                                        size_t max_added_ids,
                                        uint16_t* added_id_count) {
    if (!series || !added_id_count) {
        return;
    }
    for (uint8_t idx = 0; idx < series->count; idx++) {
        hass_add_standby_entity_id(entity_ids, series->entity_ids[idx], added_ids, max_added_ids, added_id_count);
    }
}

static void hass_update_standby_energy_metrics(home_assistant_context_t* hass) {
    bool solar_valid = false;
    bool grid_in_valid = false;
    bool battery_out_valid = false;
    bool grid_out_valid = false;
    bool battery_in_valid = false;
    float solar_value = 0.0f;
    float grid_in_value = 0.0f;
    float battery_out_value = 0.0f;
    float grid_out_value = 0.0f;
    float battery_in_value = 0.0f;
    bool house_computed = false;

    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    solar_valid = standby_energy_series_total(&hass->standby_solar_series, &solar_value);
    grid_in_valid = standby_energy_series_total(&hass->standby_grid_in_series, &grid_in_value);
    battery_out_valid = standby_energy_series_total(&hass->standby_battery_out_series, &battery_out_value);
    grid_out_valid = standby_energy_series_total(&hass->standby_grid_out_series, &grid_out_value);
    battery_in_valid = standby_energy_series_total(&hass->standby_battery_in_series, &battery_in_value);
    house_computed = hass->standby_energy_house_computed;
    xSemaphoreGive(hass->mutex);

    store_set_standby_energy_metric(hass->store, StandbyEnergyMetric::SolarGeneration, solar_valid, solar_value);
    store_set_standby_energy_metric(hass->store, StandbyEnergyMetric::GridInput, grid_in_valid, grid_in_value);
    store_set_standby_energy_metric(hass->store, StandbyEnergyMetric::GridExport, grid_out_valid, grid_out_value);
    store_set_standby_energy_metric(hass->store, StandbyEnergyMetric::BatteryUsage, battery_out_valid, battery_out_value);
    store_set_standby_energy_metric(hass->store, StandbyEnergyMetric::BatteryChargeEnergy, battery_in_valid, battery_in_value);

    if (house_computed) {
        bool house_valid = false;
        float house_value = 0.0f;
        if (solar_valid) {
            house_value += solar_value;
            house_valid = true;
        }
        if (grid_in_valid) {
            house_value += grid_in_value;
            house_valid = true;
        }
        if (battery_out_valid) {
            house_value += battery_out_value;
            house_valid = true;
        }
        if (grid_out_valid) {
            house_value -= grid_out_value;
        }
        if (battery_in_valid) {
            house_value -= battery_in_value;
        }
        if (house_valid && house_value < 0.0f) {
            house_value = 0.0f;
        }
        store_set_standby_energy_metric(hass->store, StandbyEnergyMetric::HouseUsage, house_valid, house_value);
    }
}

void hass_cmd_subscribe(home_assistant_context_t* hass) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", hass_generate_event_id(hass));
    cJSON_AddStringToObject(root, "type", "subscribe_entities");

    cJSON* entity_ids = cJSON_CreateArray();
    constexpr size_t max_added_ids = MAX_ENTITIES + 48;
    const char* added_ids[max_added_ids] = {};
    uint16_t added_id_count = 0;
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    for (uint8_t idx = 0; idx < hass->entity_count; idx++) {
        hass_add_standby_entity_id(entity_ids, hass->entity_ids[idx], added_ids, max_added_ids, &added_id_count);
    }
    hass_add_standby_entity_id(entity_ids, hass->standby_weather_entity_id, added_ids, max_added_ids, &added_id_count);
    hass_add_standby_series_ids(entity_ids, &hass->standby_solar_series, added_ids, max_added_ids, &added_id_count);
    hass_add_standby_series_ids(entity_ids, &hass->standby_grid_in_series, added_ids, max_added_ids, &added_id_count);
    hass_add_standby_series_ids(entity_ids, &hass->standby_grid_out_series, added_ids, max_added_ids, &added_id_count);
    hass_add_standby_series_ids(entity_ids, &hass->standby_battery_out_series, added_ids, max_added_ids, &added_id_count);
    hass_add_standby_series_ids(entity_ids, &hass->standby_battery_in_series, added_ids, max_added_ids, &added_id_count);
    hass_add_standby_entity_id(entity_ids, hass->standby_energy_battery_soc_entity_id, added_ids, max_added_ids, &added_id_count);
    if (!hass->standby_energy_house_computed) {
        hass_add_standby_entity_id(entity_ids, hass->standby_energy_house_entity_id, added_ids, max_added_ids, &added_id_count);
    }
    xSemaphoreGive(hass->mutex);
    cJSON_AddItemToObject(root, "entity_ids", entity_ids);

    char* request = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "Sending %s", request);
    esp_websocket_client_send_text(hass->client, request, strlen(request), portMAX_DELAY);
    cJSON_free(request);
    cJSON_Delete(root);

    hass_cmd_request_weather_forecast(hass);
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

    int8_t floor_idx = store_add_floor(hass->store, "Other Areas", nullptr);
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

static void hass_parse_weather_entity_update(home_assistant_context_t* hass, cJSON* item) {
    const char* condition = "";
    bool has_temperature = false;
    float temperature_c = 0.0f;

    cJSON* state = cJSON_GetObjectItem(item, "s");
    if (cJSON_IsString(state) && state->valuestring) {
        condition = state->valuestring;
    }

    cJSON* attributes = cJSON_GetObjectItem(item, "a");
    if (cJSON_IsObject(attributes)) {
        cJSON* temperature = cJSON_GetObjectItem(attributes, "temperature");
        if (cJSON_IsNumber(temperature)) {
            has_temperature = true;
            temperature_c = static_cast<float>(temperature->valuedouble);
        }

        cJSON* forecast = cJSON_GetObjectItem(attributes, "forecast");
        StandbyForecastDay forecast_days[MAX_STANDBY_FORECAST_DAYS] = {};
        uint8_t day_count = parse_forecast_days(forecast, forecast_days, MAX_STANDBY_FORECAST_DAYS);
        if (day_count > 0) {
            store_set_standby_forecast(hass->store, forecast_days, day_count);
        }
    }

    store_set_standby_weather(hass->store, condition, has_temperature, temperature_c);
}

static void hass_parse_standby_entity_update(home_assistant_context_t* hass, const char* entity_id, cJSON* item) {
    if (!entity_id || !item || !cJSON_IsObject(item)) {
        return;
    }

    bool is_weather = false;
    bool is_battery_soc = false;
    bool is_house_direct = false;
    bool house_computed = false;
    bool series_changed = false;
    int8_t solar_idx = -1;
    int8_t grid_in_idx = -1;
    int8_t grid_out_idx = -1;
    int8_t battery_out_idx = -1;
    int8_t battery_in_idx = -1;

    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    is_weather = has_entity_id(hass->standby_weather_entity_id) && strcmp(entity_id, hass->standby_weather_entity_id) == 0;
    is_battery_soc = has_entity_id(hass->standby_energy_battery_soc_entity_id) && strcmp(entity_id, hass->standby_energy_battery_soc_entity_id) == 0;
    is_house_direct = has_entity_id(hass->standby_energy_house_entity_id) && strcmp(entity_id, hass->standby_energy_house_entity_id) == 0;
    house_computed = hass->standby_energy_house_computed;
    solar_idx = standby_energy_series_find(&hass->standby_solar_series, entity_id);
    grid_in_idx = standby_energy_series_find(&hass->standby_grid_in_series, entity_id);
    grid_out_idx = standby_energy_series_find(&hass->standby_grid_out_series, entity_id);
    battery_out_idx = standby_energy_series_find(&hass->standby_battery_out_series, entity_id);
    battery_in_idx = standby_energy_series_find(&hass->standby_battery_in_series, entity_id);
    xSemaphoreGive(hass->mutex);

    if (is_weather) {
        hass_parse_weather_entity_update(hass, item);
        return;
    }

    float value = 0.0f;
    bool valid = parse_state_float(cJSON_GetObjectItem(item, "s"), &value);
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    if (solar_idx >= 0) {
        series_changed = standby_energy_series_set_value(&hass->standby_solar_series, solar_idx, valid, value) || series_changed;
    }
    if (grid_in_idx >= 0) {
        series_changed = standby_energy_series_set_value(&hass->standby_grid_in_series, grid_in_idx, valid, value) || series_changed;
    }
    if (grid_out_idx >= 0) {
        series_changed = standby_energy_series_set_value(&hass->standby_grid_out_series, grid_out_idx, valid, value) || series_changed;
    }
    if (battery_out_idx >= 0) {
        series_changed = standby_energy_series_set_value(&hass->standby_battery_out_series, battery_out_idx, valid, value) || series_changed;
    }
    if (battery_in_idx >= 0) {
        series_changed = standby_energy_series_set_value(&hass->standby_battery_in_series, battery_in_idx, valid, value) || series_changed;
    }
    xSemaphoreGive(hass->mutex);

    if (series_changed) {
        hass_update_standby_energy_metrics(hass);
    }

    if (is_battery_soc) {
        store_set_standby_energy_metric(hass->store, StandbyEnergyMetric::BatteryCharge, valid, value);
    } else if (is_house_direct && !house_computed) {
        store_set_standby_energy_metric(hass->store, StandbyEnergyMetric::HouseUsage, valid, value);
    }
}

static void hass_parse_weather_forecast_result(home_assistant_context_t* hass, cJSON* result_item) {
    if (!cJSON_IsObject(result_item)) {
        return;
    }

    char weather_entity_id[MAX_ENTITY_ID_LEN] = {};
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    copy_string(weather_entity_id, sizeof(weather_entity_id), hass->standby_weather_entity_id);
    xSemaphoreGive(hass->mutex);
    if (!has_entity_id(weather_entity_id)) {
        return;
    }

    cJSON* forecast_array = nullptr;
    cJSON* response = cJSON_GetObjectItem(result_item, "response");
    if (cJSON_IsObject(response)) {
        cJSON* weather_result = cJSON_GetObjectItem(response, weather_entity_id);
        if (cJSON_IsObject(weather_result)) {
            forecast_array = cJSON_GetObjectItem(weather_result, "forecast");
        }

        if (!cJSON_IsArray(forecast_array)) {
            cJSON* response_item = nullptr;
            cJSON_ArrayForEach(response_item, response) {
                if (!cJSON_IsObject(response_item)) {
                    continue;
                }
                cJSON* candidate = cJSON_GetObjectItem(response_item, "forecast");
                if (cJSON_IsArray(candidate)) {
                    forecast_array = candidate;
                    break;
                }
            }
        }
    }

    if (!cJSON_IsArray(forecast_array)) {
        cJSON* direct_forecast = cJSON_GetObjectItem(result_item, "forecast");
        if (cJSON_IsArray(direct_forecast)) {
            forecast_array = direct_forecast;
        }
    }

    StandbyForecastDay forecast_days[MAX_STANDBY_FORECAST_DAYS] = {};
    uint8_t day_count = parse_forecast_days(forecast_array, forecast_days, MAX_STANDBY_FORECAST_DAYS);
    if (day_count > 0) {
        store_set_standby_forecast(hass->store, forecast_days, day_count);
    }
}

static void hass_energy_add_stat_from_key(home_assistant_context_t::StandbyEnergySeries* series, cJSON* object, const char* key) {
    if (!series || !cJSON_IsObject(object)) {
        return;
    }
    cJSON* item = cJSON_GetObjectItem(object, key);
    if (cJSON_IsString(item) && item->valuestring) {
        standby_energy_series_add_entity(series, item->valuestring);
    }
}

static void hass_energy_add_grid_legacy_flow(home_assistant_context_t::StandbyEnergySeries* series, cJSON* source, const char* flow_key, const char* key) {
    cJSON* flow_array = cJSON_GetObjectItem(source, flow_key);
    if (!cJSON_IsArray(flow_array)) {
        return;
    }

    cJSON* flow_item = nullptr;
    cJSON_ArrayForEach(flow_item, flow_array) {
        hass_energy_add_stat_from_key(series, flow_item, key);
    }
}

static void hass_apply_energy_preferences(home_assistant_context_t* hass,
                                          const home_assistant_context_t::StandbyEnergySeries* solar_series,
                                          const home_assistant_context_t::StandbyEnergySeries* grid_in_series,
                                          const home_assistant_context_t::StandbyEnergySeries* grid_out_series,
                                          const home_assistant_context_t::StandbyEnergySeries* battery_out_series,
                                          const home_assistant_context_t::StandbyEnergySeries* battery_in_series) {
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    hass->standby_solar_series = *solar_series;
    hass->standby_grid_in_series = *grid_in_series;
    hass->standby_grid_out_series = *grid_out_series;
    hass->standby_battery_out_series = *battery_out_series;
    hass->standby_battery_in_series = *battery_in_series;

    const bool has_discovered_sources = hass->standby_solar_series.count > 0 || hass->standby_grid_in_series.count > 0 ||
                                        hass->standby_grid_out_series.count > 0 || hass->standby_battery_out_series.count > 0 ||
                                        hass->standby_battery_in_series.count > 0;
    hass->standby_energy_house_computed = has_discovered_sources;

    if (hass->standby_solar_series.count > 0) {
        copy_string(hass->standby_energy_solar_entity_id, sizeof(hass->standby_energy_solar_entity_id), hass->standby_solar_series.entity_ids[0]);
    }
    if (hass->standby_grid_in_series.count > 0) {
        copy_string(hass->standby_energy_grid_entity_id, sizeof(hass->standby_energy_grid_entity_id), hass->standby_grid_in_series.entity_ids[0]);
    }
    if (hass->standby_battery_out_series.count > 0) {
        copy_string(hass->standby_energy_battery_usage_entity_id, sizeof(hass->standby_energy_battery_usage_entity_id),
                    hass->standby_battery_out_series.entity_ids[0]);
    }
    if (hass->standby_grid_out_series.count > 0) {
        copy_string(hass->standby_energy_grid_export_entity_id, sizeof(hass->standby_energy_grid_export_entity_id),
                    hass->standby_grid_out_series.entity_ids[0]);
    } else {
        hass->standby_energy_grid_export_entity_id[0] = '\0';
    }
    if (hass->standby_battery_in_series.count > 0) {
        copy_string(hass->standby_energy_battery_charge_entity_id, sizeof(hass->standby_energy_battery_charge_entity_id),
                    hass->standby_battery_in_series.entity_ids[0]);
    } else {
        hass->standby_energy_battery_charge_entity_id[0] = '\0';
    }
    xSemaphoreGive(hass->mutex);

    if (solar_series->count > 0) {
        ESP_LOGI(TAG, "Energy source (solar): %s", solar_series->entity_ids[0]);
    }
    if (grid_in_series->count > 0) {
        ESP_LOGI(TAG, "Energy source (grid in): %s", grid_in_series->entity_ids[0]);
    }
    if (grid_out_series->count > 0) {
        ESP_LOGI(TAG, "Energy source (grid out): %s", grid_out_series->entity_ids[0]);
    }
    if (battery_out_series->count > 0) {
        ESP_LOGI(TAG, "Energy source (battery out): %s", battery_out_series->entity_ids[0]);
    }
    if (battery_in_series->count > 0) {
        ESP_LOGI(TAG, "Energy source (battery in): %s", battery_in_series->entity_ids[0]);
    }

    hass_update_standby_energy_metrics(hass);
}

static void hass_parse_energy_preferences_result(home_assistant_context_t* hass, cJSON* result_item) {
    if (!cJSON_IsObject(result_item)) {
        return;
    }

    cJSON* energy_sources = cJSON_GetObjectItem(result_item, "energy_sources");
    if (!cJSON_IsArray(energy_sources)) {
        ESP_LOGW(TAG, "Energy preferences response has no energy_sources array");
        return;
    }

    auto* solar_series = static_cast<home_assistant_context_t::StandbyEnergySeries*>(malloc(sizeof(home_assistant_context_t::StandbyEnergySeries)));
    auto* grid_in_series =
        static_cast<home_assistant_context_t::StandbyEnergySeries*>(malloc(sizeof(home_assistant_context_t::StandbyEnergySeries)));
    auto* grid_out_series =
        static_cast<home_assistant_context_t::StandbyEnergySeries*>(malloc(sizeof(home_assistant_context_t::StandbyEnergySeries)));
    auto* battery_out_series =
        static_cast<home_assistant_context_t::StandbyEnergySeries*>(malloc(sizeof(home_assistant_context_t::StandbyEnergySeries)));
    auto* battery_in_series =
        static_cast<home_assistant_context_t::StandbyEnergySeries*>(malloc(sizeof(home_assistant_context_t::StandbyEnergySeries)));
    if (!solar_series || !grid_in_series || !grid_out_series || !battery_out_series || !battery_in_series) {
        free(solar_series);
        free(grid_in_series);
        free(grid_out_series);
        free(battery_out_series);
        free(battery_in_series);
        ESP_LOGW(TAG, "Insufficient memory to parse energy preferences");
        return;
    }

    standby_energy_series_reset(solar_series);
    standby_energy_series_reset(grid_in_series);
    standby_energy_series_reset(grid_out_series);
    standby_energy_series_reset(battery_out_series);
    standby_energy_series_reset(battery_in_series);

    cJSON* source = nullptr;
    cJSON_ArrayForEach(source, energy_sources) {
        if (!cJSON_IsObject(source)) {
            continue;
        }
        cJSON* type_item = cJSON_GetObjectItem(source, "type");
        if (!cJSON_IsString(type_item) || !type_item->valuestring) {
            continue;
        }

        const char* source_type = type_item->valuestring;
        if (strcmp(source_type, "solar") == 0) {
            hass_energy_add_stat_from_key(solar_series, source, "stat_energy_from");
            continue;
        }
        if (strcmp(source_type, "battery") == 0) {
            hass_energy_add_stat_from_key(battery_out_series, source, "stat_energy_from");
            hass_energy_add_stat_from_key(battery_in_series, source, "stat_energy_to");
            continue;
        }
        if (strcmp(source_type, "grid") == 0) {
            // New unified format.
            hass_energy_add_stat_from_key(grid_in_series, source, "stat_energy_from");
            hass_energy_add_stat_from_key(grid_out_series, source, "stat_energy_to");
            // Legacy format still appears in older setups.
            hass_energy_add_grid_legacy_flow(grid_in_series, source, "flow_from", "stat_energy_from");
            hass_energy_add_grid_legacy_flow(grid_out_series, source, "flow_to", "stat_energy_to");
        }
    }

    if (solar_series->count == 0 && grid_in_series->count == 0 && grid_out_series->count == 0 && battery_out_series->count == 0 &&
        battery_in_series->count == 0) {
        ESP_LOGW(TAG, "No usable energy entities discovered from energy/get_prefs");
        free(solar_series);
        free(grid_in_series);
        free(grid_out_series);
        free(battery_out_series);
        free(battery_in_series);
        return;
    }

    hass_apply_energy_preferences(hass, solar_series, grid_in_series, grid_out_series, battery_out_series, battery_in_series);
    free(solar_series);
    free(grid_in_series);
    free(grid_out_series);
    free(battery_out_series);
    free(battery_in_series);
}

void hass_parse_entity_update(home_assistant_context_t* hass, uint8_t widget_idx, cJSON* item) {
    uint8_t entity_mode = 0;
    int8_t entity_value = -1;
    CommandType command_type = CommandType::SetLightBrightnessPercentage;
    uint8_t climate_mode_mask = CLIMATE_MODE_MASK_DEFAULT;
    uint8_t previous_climate_mode_mask = CLIMATE_MODE_MASK_DEFAULT;
    bool climate_hvac_modes_known = false;
    bool climate_is_ac = false;
    bool previous_climate_hvac_modes_known = false;
    bool previous_climate_is_ac = false;

    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    entity_mode = hass->entity_modes[widget_idx];
    entity_value = hass->entity_values[widget_idx];
    xSemaphoreTake(hass->store->mutex, portMAX_DELAY);
    command_type = hass->store->entities[widget_idx].command_type;
    if (command_type == CommandType::SetClimateModeAndTemperature) {
        previous_climate_mode_mask = climate_normalize_mode_mask(hass->store->entities[widget_idx].climate_mode_mask);
        climate_mode_mask = previous_climate_mode_mask;
        previous_climate_hvac_modes_known = hass->store->entities[widget_idx].climate_hvac_modes_known;
        previous_climate_is_ac = hass->store->entities[widget_idx].climate_is_ac;
        climate_hvac_modes_known = previous_climate_hvac_modes_known;
        climate_is_ac = previous_climate_is_ac;
    }
    xSemaphoreGive(hass->store->mutex);

    cJSON* state = cJSON_GetObjectItem(item, "s");
    cJSON* attributes = cJSON_GetObjectItem(item, "a");

    uint8_t value = 0;
    if (command_type == CommandType::SetClimateModeAndTemperature) {
        ClimateMode mode = static_cast<ClimateMode>(entity_mode);
        if (mode != ClimateMode::Off && mode != ClimateMode::Heat && mode != ClimateMode::Cool) {
            mode = ClimateMode::Off;
        }

        if (cJSON_IsObject(attributes)) {
            cJSON* hvac_modes = cJSON_GetObjectItem(attributes, "hvac_modes");
            if (cJSON_IsArray(hvac_modes)) {
                climate_hvac_modes_known = true;
                uint8_t parsed_mode_mask = 0;
                cJSON* hvac_mode_item = nullptr;
                cJSON_ArrayForEach(hvac_mode_item, hvac_modes) {
                    if (!cJSON_IsString(hvac_mode_item)) {
                        continue;
                    }

                    const char* hvac_mode = hvac_mode_item->valuestring;
                    if (strcmp(hvac_mode, "off") == 0) {
                        parsed_mode_mask |= CLIMATE_MODE_MASK_OFF;
                    } else if (strcmp(hvac_mode, "heat") == 0 || strcmp(hvac_mode, "heating") == 0) {
                        parsed_mode_mask |= CLIMATE_MODE_MASK_HEAT;
                    } else if (strcmp(hvac_mode, "cool") == 0 || strcmp(hvac_mode, "cooling") == 0) {
                        parsed_mode_mask |= CLIMATE_MODE_MASK_COOL;
                    } else if (strcmp(hvac_mode, "heat_cool") == 0) {
                        parsed_mode_mask |= CLIMATE_MODE_MASK_HEAT | CLIMATE_MODE_MASK_COOL;
                    }
                }

                climate_is_ac = (parsed_mode_mask & CLIMATE_MODE_MASK_COOL) != 0;
                if ((parsed_mode_mask & (CLIMATE_MODE_MASK_HEAT | CLIMATE_MODE_MASK_COOL)) != 0) {
                    climate_mode_mask = climate_normalize_mode_mask(parsed_mode_mask);
                }
            }
        }

        if (cJSON_IsString(state)) {
            if (strcmp(state->valuestring, "off") == 0) {
                mode = ClimateMode::Off;
            } else if (strcmp(state->valuestring, "heat") == 0 || strcmp(state->valuestring, "heating") == 0) {
                mode = ClimateMode::Heat;
            } else if (strcmp(state->valuestring, "cool") == 0 || strcmp(state->valuestring, "cooling") == 0) {
                mode = ClimateMode::Cool;
            }
        }
        if (!climate_is_mode_supported(climate_mode_mask, mode)) {
            mode = climate_default_enabled_mode(climate_mode_mask);
        }

        uint8_t temp_steps = entity_value >= 0 ? climate_clamp_temp_steps(entity_value) : climate_celsius_to_steps(20.0f);
        if (cJSON_IsObject(attributes)) {
            cJSON* target_temp = cJSON_GetObjectItem(attributes, "temperature");
            if (!cJSON_IsNumber(target_temp)) {
                target_temp = cJSON_GetObjectItem(attributes, "target_temp_low");
            }
            if (cJSON_IsNumber(target_temp)) {
                temp_steps = climate_celsius_to_steps(static_cast<float>(target_temp->valuedouble));
            }
        }

        entity_mode = static_cast<uint8_t>(mode);
        entity_value = static_cast<int8_t>(temp_steps);
        value = climate_pack_value(mode, temp_steps);
    } else if (command_type == CommandType::SetCoverOpenClose) {
        bool is_open = entity_mode != 0;
        if (cJSON_IsString(state)) {
            if (strcmp(state->valuestring, "open") == 0 || strcmp(state->valuestring, "opening") == 0) {
                is_open = true;
            } else if (strcmp(state->valuestring, "closed") == 0 || strcmp(state->valuestring, "closing") == 0) {
                is_open = false;
            }
        }
        entity_mode = is_open ? 1 : 0;
        value = is_open ? 1 : 0;
    } else {
        bool is_on = entity_mode != 0;

        if (cJSON_IsString(state)) {
            if (strcmp(state->valuestring, "on") == 0) {
                is_on = true;
            } else if (strcmp(state->valuestring, "off") == 0) {
                is_on = false;
            }
        }

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

        entity_mode = is_on ? 1 : 0;
        if (is_on) {
            value = entity_value < 0 ? 1 : static_cast<uint8_t>(entity_value);
        }
    }

    hass->entity_modes[widget_idx] = entity_mode;
    hass->entity_values[widget_idx] = entity_value;
    if (command_type == CommandType::SetClimateModeAndTemperature) {
        xSemaphoreTake(hass->store->mutex, portMAX_DELAY);
        hass->store->entities[widget_idx].climate_mode_mask = climate_mode_mask;
        hass->store->entities[widget_idx].climate_hvac_modes_known = climate_hvac_modes_known;
        hass->store->entities[widget_idx].climate_is_ac = climate_is_ac;
        xSemaphoreGive(hass->store->mutex);
    }

    TickType_t now = xTaskGetTickCount();
    bool ignore_update = (now - hass->last_command_sent_at_ms[widget_idx]) < pdMS_TO_TICKS(HASS_IGNORE_UPDATE_DELAY_MS);
    const char* entity_id = hass->entity_ids[widget_idx];
    xSemaphoreGive(hass->mutex);

    if (ignore_update) {
        ESP_LOGI(TAG, "Ignoring update of entity %s", entity_id);
        return;
    }

    ESP_LOGI(TAG, "Setting value of widget %d to %d", widget_idx, value);
    store_update_value(hass->store, widget_idx, value);
    if (command_type == CommandType::SetClimateModeAndTemperature &&
        (climate_mode_mask != previous_climate_mode_mask || climate_hvac_modes_known != previous_climate_hvac_modes_known ||
         climate_is_ac != previous_climate_is_ac)) {
        ESP_LOGI(TAG, "Climate visibility updated for %s: hvac_modes_known=%d, is_ac=%d", entity_id, climate_hvac_modes_known ? 1 : 0,
                 climate_is_ac ? 1 : 0);
        store_bump_rooms_revision(hass->store);
    }
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
            hass_parse_standby_entity_update(hass, item->string, item);
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
            cJSON* plus_value = cJSON_GetObjectItem(item, "+");
            if (cJSON_IsObject(plus_value)) {
                hass_parse_standby_entity_update(hass, item->string, plus_value);
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
        const char* floor_id = get_optional_string(item, "floor_id", nullptr);
        if (!floor_id) {
            floor_id = get_optional_string(item, "id", "fi");
        }
        const char* floor_name = get_optional_string(item, "name", "n");
        const char* floor_icon = get_optional_string(item, "icon", "ic");

        if (!floor_id || !floor_name) {
            continue;
        }

        ESP_LOGI(TAG, "[ICON] floor '%s' (id=%s) icon=%s", floor_name, floor_id, floor_icon ? floor_icon : "(none)");

        int8_t floor_idx = store_add_floor(hass->store, floor_name, floor_icon);
        if (floor_idx < 0) {
            ESP_LOGW(TAG, "Skipping floor %s: floor limit reached", floor_id);
            continue;
        }

        xSemaphoreTake(hass->mutex, portMAX_DELAY);
        if (hass->floor_count < MAX_FLOORS) {
            uint8_t idx = hass->floor_count++;
            copy_string(hass->floor_ids[idx], sizeof(hass->floor_ids[idx]), floor_id);
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
        const char* area_id = get_optional_string(item, "area_id", "ai");
        const char* area_name = get_optional_string(item, "name", "n");
        const char* floor_id = get_optional_string(item, "floor_id", "fl");
        const char* area_icon = get_optional_string(item, "icon", "ic");

        if (!area_id || !area_name) {
            continue;
        }

        ESP_LOGI(TAG, "[ICON] room '%s' (area_id=%s, floor_id=%s) icon=%s", area_name, area_id, floor_id ? floor_id : "(none)",
                 area_icon ? area_icon : "(none)");

        int16_t floor_idx = -1;
        if (floor_id) {
            floor_idx = hass_find_floor_for_floor_id(hass, floor_id);
        }
        if (floor_idx < 0) {
            floor_idx = hass_ensure_other_floor(hass);
        }
        if (floor_idx < 0) {
            ESP_LOGW(TAG, "Skipping area %s: no floor slot available", area_id);
            continue;
        }

        int8_t room_idx = store_add_room(hass->store, area_name, area_icon, static_cast<int8_t>(floor_idx));
        if (room_idx < 0) {
            ESP_LOGW(TAG, "Skipping area %s: room limit reached", area_id);
            continue;
        }

        xSemaphoreTake(hass->mutex, portMAX_DELAY);
        if (hass->area_count < MAX_ROOMS) {
            uint8_t area_idx = hass->area_count++;
            copy_string(hass->area_ids[area_idx], sizeof(hass->area_ids[area_idx]), area_id);
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

        if (!cJSON_IsString(entity_id_item)) {
            continue;
        }
        if (cJSON_IsString(hidden_by_item) || cJSON_IsString(disabled_by_item) || cJSON_IsTrue(hidden_bool_item)) {
            continue;
        }

        const char* display_name = hass_entity_display_name_from_registry(item);
        if (strncmp(entity_id_item->valuestring, "weather.", 8) == 0) {
            xSemaphoreTake(hass->mutex, portMAX_DELAY);
            if (!has_entity_id(hass->standby_weather_entity_id)) {
                copy_string(hass->standby_weather_entity_id, sizeof(hass->standby_weather_entity_id), entity_id_item->valuestring);
                ESP_LOGI(TAG, "Auto-selected weather entity %s for standby screen", hass->standby_weather_entity_id);
            }
            xSemaphoreGive(hass->mutex);
        }

        CommandType command_type;
        if (strncmp(entity_id_item->valuestring, "light.", 6) == 0) {
            command_type = CommandType::SetLightBrightnessPercentage;
        } else if (strncmp(entity_id_item->valuestring, "climate.", 8) == 0) {
            command_type = CommandType::SetClimateModeAndTemperature;
        } else if (strncmp(entity_id_item->valuestring, "cover.", 6) == 0) {
            if (!cover_entity_should_be_included(item, entity_id_item->valuestring, display_name)) {
                ESP_LOGI(TAG, "Skipping non-group cover %s", entity_id_item->valuestring);
                continue;
            }
            command_type = CommandType::SetCoverOpenClose;
        } else {
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

        EntityConfig entity = {
            .entity_id = entity_id_item->valuestring,
            .command_type = command_type,
        };
        if (store_add_entity_to_room(hass->store, room_idx, entity, display_name) < 0) {
            ESP_LOGW(TAG, "Skipping entity %s: limits reached", entity_id_item->valuestring);
        }
    }
}

void hass_start_discovery(home_assistant_context_t* hass) {
    ESP_LOGI(TAG, "Starting room entity discovery");
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
    uint16_t weather_forecast_request_id = 0;
    uint16_t energy_prefs_request_id = 0;
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    floor_request_id = hass->floor_registry_request_id;
    area_request_id = hass->area_registry_request_id;
    device_request_id = hass->device_registry_request_id;
    entity_request_id = hass->entity_registry_request_id;
    weather_forecast_request_id = hass->weather_forecast_request_id;
    energy_prefs_request_id = hass->energy_prefs_request_id;
    xSemaphoreGive(hass->mutex);

    if (response_id == weather_forecast_request_id) {
        xSemaphoreTake(hass->mutex, portMAX_DELAY);
        hass->weather_forecast_requested = false;
        hass->weather_forecast_request_id = 0;
        xSemaphoreGive(hass->mutex);
        if (!success) {
            ESP_LOGW(TAG, "Weather forecast request failed");
        } else {
            hass_parse_weather_forecast_result(hass, result_item);
        }
        return;
    }

    if (response_id == energy_prefs_request_id) {
        if (!success) {
            ESP_LOGW(TAG, "Energy preferences request failed, keeping configured standby entities");
        } else {
            hass_parse_energy_preferences_result(hass, result_item);
        }
        hass_set_pending_discovery_command(hass, DiscoveryCommandSubscribeEntities);
        return;
    }

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
        const uint8_t entity_count = hass->entity_count;
        xSemaphoreGive(hass->mutex);
        if (entity_count == 0) {
            ESP_LOGW(TAG, "No light/climate/cover entities discovered for mapped rooms");
        }
        hass_set_pending_discovery_command(hass, DiscoveryCommandRequestEnergyPrefs);
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

static void hass_send_call_service(home_assistant_context_t* hass, const char* domain, const char* service, cJSON* service_data) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", hass_generate_event_id(hass));
    cJSON_AddStringToObject(root, "type", "call_service");
    cJSON_AddStringToObject(root, "domain", domain);
    cJSON_AddStringToObject(root, "service", service);
    cJSON_AddItemToObject(root, "service_data", service_data);

    char* request = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "Sending %s", request);
    esp_websocket_client_send_text(hass->client, request, strlen(request), portMAX_DELAY);
    cJSON_free(request);
    cJSON_Delete(root);
}

static const char* climate_mode_service_value(ClimateMode mode) {
    switch (mode) {
    case ClimateMode::Heat:
        return "heat";
    case ClimateMode::Cool:
        return "cool";
    case ClimateMode::Off:
    default:
        return "off";
    }
}

void hass_send_command(home_assistant_context_t* hass, Command* cmd) {
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    hass->last_command_sent_at_ms[cmd->entity_idx] = xTaskGetTickCount();
    xSemaphoreGive(hass->mutex);

    switch (cmd->type) {
    case CommandType::SetLightBrightnessPercentage: {
        cJSON* service_data = cJSON_CreateObject();
        cJSON_AddStringToObject(service_data, "entity_id", cmd->entity_id);
        if (cmd->value == 0) {
            hass_send_call_service(hass, "light", "turn_off", service_data);
        } else {
            cJSON_AddNumberToObject(service_data, "brightness_pct", cmd->value);
            hass_send_call_service(hass, "light", "turn_on", service_data);
        }
        break;
    }
    case CommandType::SetClimateModeAndTemperature: {
        ClimateMode mode = climate_unpack_mode(cmd->value);
        float target_c = climate_steps_to_celsius(climate_unpack_temp_steps(cmd->value));

        cJSON* mode_service_data = cJSON_CreateObject();
        cJSON_AddStringToObject(mode_service_data, "entity_id", cmd->entity_id);
        cJSON_AddStringToObject(mode_service_data, "hvac_mode", climate_mode_service_value(mode));
        hass_send_call_service(hass, "climate", "set_hvac_mode", mode_service_data);

        if (mode != ClimateMode::Off) {
            cJSON* temp_service_data = cJSON_CreateObject();
            cJSON_AddStringToObject(temp_service_data, "entity_id", cmd->entity_id);
            cJSON_AddNumberToObject(temp_service_data, "temperature", target_c);
            hass_send_call_service(hass, "climate", "set_temperature", temp_service_data);
        }
        break;
    }
    case CommandType::SetCoverOpenClose: {
        cJSON* service_data = cJSON_CreateObject();
        cJSON_AddStringToObject(service_data, "entity_id", cmd->entity_id);
        hass_send_call_service(hass, "cover", cmd->value == 0 ? "close_cover" : "open_cover", service_data);
        break;
    }
    case CommandType::SetFanSpeedPercentage: {
        cJSON* service_data = cJSON_CreateObject();
        cJSON_AddStringToObject(service_data, "entity_id", cmd->entity_id);
        cJSON_AddNumberToObject(service_data, "percentage", cmd->value);
        hass_send_call_service(hass, "fan", "set_percentage", service_data);
        break;
    }
    case CommandType::SwitchOnOff: {
        cJSON* service_data = cJSON_CreateObject();
        cJSON_AddStringToObject(service_data, "entity_id", cmd->entity_id);
        hass_send_call_service(hass, "switch", cmd->value == 0 ? "turn_off" : "turn_on", service_data);
        break;
    }
    case CommandType::AutomationOnOff: {
        cJSON* service_data = cJSON_CreateObject();
        cJSON_AddStringToObject(service_data, "entity_id", cmd->entity_id);
        hass_send_call_service(hass, "automation", cmd->value == 0 ? "turn_off" : "turn_on", service_data);
        break;
    }
    default:
        ESP_LOGI(TAG, "Service type not supported");
        break;
    }
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
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

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
            const uint32_t now_ms = static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
            const bool standby_active = store_is_standby_active(store);
            bool weather_forecast_requested = false;
            uint32_t last_weather_forecast_request_ms = 0;
            xSemaphoreTake(hass->mutex, portMAX_DELAY);
            weather_forecast_requested = hass->weather_forecast_requested;
            last_weather_forecast_request_ms = hass->last_weather_forecast_request_ms;
            xSemaphoreGive(hass->mutex);
            if (standby_active && !weather_forecast_requested &&
                (last_weather_forecast_request_ms == 0 ||
                 static_cast<uint32_t>(now_ms - last_weather_forecast_request_ms) >= STANDBY_REFRESH_INTERVAL_MS)) {
                hass_cmd_request_weather_forecast(hass);
            }
            while (store_get_pending_command(store, &command)) {
                hass_send_command(hass, &command);
                store_ack_pending_command(store, &command);
                vTaskDelay(pdMS_TO_TICKS(HASS_TASK_SEND_DELAY_MS));
            }
        }
    }
}
