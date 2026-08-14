#include "managers/harness.h"
#include "managers/beacon.h"
#include "boards.h"
#include "constants.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <Arduino.h>
#include <cJSON.h>
#include <cstring>

static const char* TAG = "harness";

#ifdef TARGET_LILYGO_T5_S3_PRO
static const char* BOARD_NAME = "lilygo-t5-s3";
#endif
#ifdef TARGET_M5PAPER_S3
static const char* BOARD_NAME = "m5-papers3";
#endif

struct SyntheticGesture {
    bool active;
    bool started; // the touch task polls every 200ms when idle; the clock starts on first consumption
    uint16_t x1;
    uint16_t y1;
    uint16_t x2;
    uint16_t y2;
    uint32_t duration_ms;
    uint32_t start_ms;
    uint32_t queued_ms;
};

static SyntheticGesture gesture = {};
static SemaphoreHandle_t gesture_mutex = nullptr;
static HarnessTaskArgs* harness_ctx = nullptr;
static uint8_t* framebuffer_copy = nullptr;

bool harness_get_samples(BBCapTouch* bbct, TOUCHINFO* ti) {
    bool synthetic = false;

    if (gesture_mutex != nullptr) {
        xSemaphoreTake(gesture_mutex, portMAX_DELAY);
        if (gesture.active) {
            if (!gesture.started) {
                gesture.started = true;
                gesture.start_ms = millis();
            }
            const uint32_t elapsed = millis() - gesture.start_ms;
            if (elapsed > gesture.duration_ms) {
                gesture.active = false;
            } else {
                const float t = gesture.duration_ms == 0 ? 1.0f : static_cast<float>(elapsed) / gesture.duration_ms;
                const int32_t dx = static_cast<int32_t>(gesture.x2) - static_cast<int32_t>(gesture.x1);
                const int32_t dy = static_cast<int32_t>(gesture.y2) - static_cast<int32_t>(gesture.y1);
                ti->count = 1;
                ti->x[0] = static_cast<uint16_t>(gesture.x1 + dx * t);
                ti->y[0] = static_cast<uint16_t>(gesture.y1 + dy * t);
                synthetic = true;
            }
        }
        xSemaphoreGive(gesture_mutex);
    }

    if (synthetic) {
        // Emulate the I2C sampling cadence; the touch task loop has no delay of
        // its own while samples are being returned.
        vTaskDelay(pdMS_TO_TICKS(HARNESS_SAMPLE_INTERVAL_MS));
        return true;
    }

    return bbct->getSamples(ti);
}

static const char* reset_reason_name() {
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
        return "poweron";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "int_watchdog";
    case ESP_RST_TASK_WDT:
        return "task_watchdog";
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_DEEPSLEEP:
        return "deepsleep";
    default:
        return "other";
    }
}

static const char* conn_state_name(ConnState state) {
    switch (state) {
    case ConnState::Initializing:
        return "initializing";
    case ConnState::InvalidCredentials:
        return "invalid_credentials";
    case ConnState::ConnectionError:
        return "connection_error";
    case ConnState::Up:
        return "up";
    }
    return "unknown";
}

static const char* ui_mode_name(UiMode mode) {
    switch (mode) {
    case UiMode::Blank:
        return "Blank";
    case UiMode::Boot:
        return "Boot";
    case UiMode::GenericError:
        return "GenericError";
    case UiMode::WifiDisconnected:
        return "WifiDisconnected";
    case UiMode::HassDisconnected:
        return "HassDisconnected";
    case UiMode::HassInvalidKey:
        return "HassInvalidKey";
    case UiMode::FloorList:
        return "FloorList";
    case UiMode::RoomList:
        return "RoomList";
    case UiMode::RoomControls:
        return "RoomControls";
    case UiMode::Standby:
        return "Standby";
    case UiMode::SettingsMenu:
        return "SettingsMenu";
    case UiMode::WifiSettings:
        return "WifiSettings";
    case UiMode::WifiPassword:
        return "WifiPassword";
    }
    return "unknown";
}

static const char* command_type_name(CommandType type) {
    switch (type) {
    case CommandType::SetLightBrightnessPercentage:
        return "light";
    case CommandType::SetClimateModeAndTemperature:
        return "climate";
    case CommandType::SetCoverOpenClose:
        return "cover";
    case CommandType::SetFanSpeedPercentage:
        return "fan";
    case CommandType::SwitchOnOff:
        return "switch";
    case CommandType::AutomationOnOff:
        return "automation";
    case CommandType::RefreshStandbyBatterySoc:
        return "battery_soc";
    case CommandType::ValveOpenClose:
        return "valve";
    }
    return "unknown";
}

static esp_err_t send_json(httpd_req_t* req, cJSON* root) {
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}

static esp_err_t send_simple_error(httpd_req_t* req, httpd_err_code_t code, const char* message) {
    httpd_resp_send_err(req, code, message);
    return ESP_OK; // Error already delivered, keep the connection handling happy
}

static esp_err_t health_get_handler(httpd_req_t* req) {
    HarnessInfoSnapshot info;
    store_get_harness_info(harness_ctx->store, &info);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddStringToObject(root, "board", BOARD_NAME);
    cJSON_AddNumberToObject(root, "uptime_ms", millis());
    cJSON_AddNumberToObject(root, "heap_free", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddStringToObject(root, "beacon", beacon_status());
    cJSON_AddStringToObject(root, "reset_reason", reset_reason_name());

    BatteryStatus battery;
    store_get_battery(harness_ctx->store, &battery);
    if (battery.valid) {
        cJSON_AddNumberToObject(root, "battery_pct", battery.pct);
        cJSON_AddNumberToObject(root, "battery_mv", battery.millivolts);
        cJSON_AddNumberToObject(root, "battery_ma", battery.milliamps);
    }
    cJSON_AddStringToObject(root, "wifi", conn_state_name(info.wifi));
    cJSON_AddStringToObject(root, "home_assistant", conn_state_name(info.home_assistant));
    cJSON_AddStringToObject(root, "ip", info.ip_address);
    cJSON_AddStringToObject(root, "ssid", info.ssid);
    cJSON_AddNumberToObject(root, "rssi", info.rssi);
    return send_json(req, root);
}

static esp_err_t state_get_handler(httpd_req_t* req) {
    static uint32_t ui_state_version = 0;
    static UIState ui_state = {};
    static FloorListSnapshot floor_list = {};
    static RoomListSnapshot room_list = {};

    ui_state_copy(harness_ctx->shared_state, &ui_state_version, &ui_state);

    HarnessInfoSnapshot info;
    store_get_harness_info(harness_ctx->store, &info);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mode", ui_mode_name(ui_state.mode));
    cJSON_AddNumberToObject(root, "version", ui_state_version);
    cJSON_AddNumberToObject(root, "selected_floor", ui_state.selected_floor);
    cJSON_AddNumberToObject(root, "selected_room", ui_state.selected_room);
    cJSON_AddNumberToObject(root, "floor_list_page", ui_state.floor_list_page);
    cJSON_AddNumberToObject(root, "room_list_page", ui_state.room_list_page);
    cJSON_AddNumberToObject(root, "room_controls_page", ui_state.room_controls_page);
    cJSON_AddNumberToObject(root, "rooms_revision", ui_state.rooms_revision);
    cJSON_AddNumberToObject(root, "settings_revision", ui_state.settings_revision);
    cJSON_AddNumberToObject(root, "standby_revision", ui_state.standby_revision);
    cJSON_AddStringToObject(root, "wifi", conn_state_name(info.wifi));
    cJSON_AddStringToObject(root, "home_assistant", conn_state_name(info.home_assistant));
    cJSON_AddNumberToObject(root, "device_room", store_get_device_room(harness_ctx->store));

    store_get_floor_list_snapshot(harness_ctx->store, &floor_list);
    cJSON* floors = cJSON_AddArrayToObject(root, "floors");
    for (uint8_t idx = 0; idx < floor_list.floor_count; idx++) {
        cJSON* floor = cJSON_CreateObject();
        cJSON_AddStringToObject(floor, "name", floor_list.floor_names[idx]);
        cJSON_AddStringToObject(floor, "icon", floor_list.floor_icons[idx]);
        cJSON_AddItemToArray(floors, floor);
    }

    cJSON* rooms = cJSON_AddArrayToObject(root, "rooms");
    if (ui_state.selected_floor >= 0 && store_get_room_list_snapshot(harness_ctx->store, ui_state.selected_floor, &room_list)) {
        for (uint8_t idx = 0; idx < room_list.room_count; idx++) {
            cJSON* room = cJSON_CreateObject();
            cJSON_AddStringToObject(room, "name", room_list.room_names[idx]);
            cJSON_AddStringToObject(room, "icon", room_list.room_icons[idx]);
            cJSON_AddItemToArray(rooms, room);
        }
    }

    // Copy the widget layout under the epaper mutex: ui_task rebuilds the Screen
    // while drawing, holding the same mutex.
    size_t widget_count = 0;
    uint8_t entity_ids[MAX_WIDGETS_PER_SCREEN];
    Rect rects[MAX_WIDGETS_PER_SCREEN];
    uint8_t values[MAX_WIDGETS_PER_SCREEN];
    xSemaphoreTake(harness_ctx->store->epaper_mutex, portMAX_DELAY);
    widget_count = harness_ctx->screen->widget_count;
    memcpy(entity_ids, harness_ctx->screen->entity_ids, sizeof(entity_ids));
    memcpy(rects, harness_ctx->screen->widget_rects, sizeof(rects));
    xSemaphoreGive(harness_ctx->store->epaper_mutex);
    memcpy(values, ui_state.widget_values, sizeof(values));

    cJSON* widgets = cJSON_AddArrayToObject(root, "widgets");
    for (size_t idx = 0; idx < widget_count; idx++) {
        HarnessWidgetEntity entity;
        store_get_harness_entity(harness_ctx->store, entity_ids[idx], &entity);

        cJSON* widget = cJSON_CreateObject();
        cJSON_AddStringToObject(widget, "entity_id", entity.entity_id);
        cJSON_AddStringToObject(widget, "name", entity.display_name);
        cJSON_AddStringToObject(widget, "type", command_type_name(entity.command_type));
        cJSON_AddNumberToObject(widget, "value", values[idx]);
        cJSON* rect = cJSON_AddObjectToObject(widget, "rect");
        cJSON_AddNumberToObject(rect, "x", rects[idx].x);
        cJSON_AddNumberToObject(rect, "y", rects[idx].y);
        cJSON_AddNumberToObject(rect, "w", rects[idx].w);
        cJSON_AddNumberToObject(rect, "h", rects[idx].h);
        cJSON_AddItemToArray(widgets, widget);
    }

    return send_json(req, root);
}

static cJSON* read_json_body(httpd_req_t* req) {
    char body[HARNESS_MAX_BODY_LEN];
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        return nullptr;
    }
    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            return nullptr;
        }
        received += ret;
    }
    body[received] = '\0';
    return cJSON_Parse(body);
}

static bool get_uint16(cJSON* root, const char* key, uint16_t max_exclusive, uint16_t* out) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble >= max_exclusive) {
        return false;
    }
    *out = static_cast<uint16_t>(item->valuedouble);
    return true;
}

static esp_err_t queue_gesture(httpd_req_t* req, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint32_t duration_ms) {
    bool queued = false;
    xSemaphoreTake(gesture_mutex, portMAX_DELAY);
    const bool stale_unstarted = gesture.active && !gesture.started && millis() - gesture.queued_ms > 2000;
    if (!gesture.active || stale_unstarted) {
        gesture.x1 = x1;
        gesture.y1 = y1;
        gesture.x2 = x2;
        gesture.y2 = y2;
        gesture.duration_ms = duration_ms;
        gesture.started = false;
        gesture.queued_ms = millis();
        gesture.active = true;
        queued = true;
    }
    xSemaphoreGive(gesture_mutex);

    if (!queued) {
        return send_simple_error(req, HTTPD_400_BAD_REQUEST, "gesture already active");
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

static esp_err_t tap_post_handler(httpd_req_t* req) {
    cJSON* body = read_json_body(req);
    if (body == nullptr) {
        return send_simple_error(req, HTTPD_400_BAD_REQUEST, "invalid JSON body");
    }

    uint16_t x, y;
    uint32_t hold_ms = HARNESS_TAP_HOLD_MS;
    bool valid = get_uint16(body, "x", DISPLAY_WIDTH, &x) && get_uint16(body, "y", DISPLAY_HEIGHT, &y);
    cJSON* hold = cJSON_GetObjectItem(body, "hold_ms");
    if (cJSON_IsNumber(hold) && hold->valuedouble > 0 && hold->valuedouble <= 5000) {
        hold_ms = static_cast<uint32_t>(hold->valuedouble);
    }
    cJSON_Delete(body);

    if (!valid) {
        return send_simple_error(req, HTTPD_400_BAD_REQUEST, "x/y missing or out of range");
    }
    return queue_gesture(req, x, y, x, y, hold_ms);
}

static esp_err_t swipe_post_handler(httpd_req_t* req) {
    cJSON* body = read_json_body(req);
    if (body == nullptr) {
        return send_simple_error(req, HTTPD_400_BAD_REQUEST, "invalid JSON body");
    }

    uint16_t x1, y1, x2, y2;
    uint32_t duration_ms = HARNESS_SWIPE_DURATION_MS;
    bool valid = get_uint16(body, "x1", DISPLAY_WIDTH, &x1) && get_uint16(body, "y1", DISPLAY_HEIGHT, &y1) &&
                 get_uint16(body, "x2", DISPLAY_WIDTH, &x2) && get_uint16(body, "y2", DISPLAY_HEIGHT, &y2);
    cJSON* duration = cJSON_GetObjectItem(body, "duration_ms");
    if (cJSON_IsNumber(duration) && duration->valuedouble > 0 && duration->valuedouble <= 5000) {
        duration_ms = static_cast<uint32_t>(duration->valuedouble);
    }
    cJSON_Delete(body);

    if (!valid) {
        return send_simple_error(req, HTTPD_400_BAD_REQUEST, "x1/y1/x2/y2 missing or out of range");
    }
    return queue_gesture(req, x1, y1, x2, y2, duration_ms);
}

static esp_err_t home_post_handler(httpd_req_t* req) {
    // Without this the standby timer can re-fire immediately after going home
    store_note_interaction(harness_ctx->store, millis());
    store_go_home(harness_ctx->store);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

static esp_err_t screenshot_get_handler(httpd_req_t* req) {
    constexpr uint16_t native_width = DISPLAY_HEIGHT; // buffer stays landscape; drawing is rotated
    constexpr uint16_t native_height = DISPLAY_WIDTH;
    constexpr size_t len_1bpp = (native_width / 8) * native_height;
    constexpr size_t len_4bpp = (native_width / 2) * native_height;

    xSemaphoreTake(harness_ctx->store->epaper_mutex, portMAX_DELAY);
    const int mode = harness_ctx->epaper->getMode();
    const size_t len = mode == BB_MODE_1BPP ? len_1bpp : len_4bpp;
    memcpy(framebuffer_copy, harness_ctx->epaper->currentBuffer(), len);
    xSemaphoreGive(harness_ctx->store->epaper_mutex);

    char header_value[16];
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "X-EPD-Mode", mode == BB_MODE_1BPP ? "1bpp" : "4bpp");
    snprintf(header_value, sizeof(header_value), "%u", native_width);
    httpd_resp_set_hdr(req, "X-EPD-Native-Width", header_value);
    char header_value2[16];
    snprintf(header_value2, sizeof(header_value2), "%u", native_height);
    httpd_resp_set_hdr(req, "X-EPD-Native-Height", header_value2);
    httpd_resp_set_hdr(req, "X-EPD-Rotation", "90");
    return httpd_resp_send(req, reinterpret_cast<const char*>(framebuffer_copy), len);
}

static void harness_task(void* arg) {
    HarnessTaskArgs* ctx = static_cast<HarnessTaskArgs*>(arg);

    store_wait_for_wifi_up(ctx->store);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HARNESS_HTTP_PORT;
    config.stack_size = HARNESS_HTTPD_STACK;
    config.lru_purge_enable = true;

    httpd_handle_t server = nullptr;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/health", .method = HTTP_GET, .handler = health_get_handler, .user_ctx = nullptr},
        {.uri = "/state", .method = HTTP_GET, .handler = state_get_handler, .user_ctx = nullptr},
        {.uri = "/screenshot", .method = HTTP_GET, .handler = screenshot_get_handler, .user_ctx = nullptr},
        {.uri = "/tap", .method = HTTP_POST, .handler = tap_post_handler, .user_ctx = nullptr},
        {.uri = "/swipe", .method = HTTP_POST, .handler = swipe_post_handler, .user_ctx = nullptr},
        {.uri = "/home", .method = HTTP_POST, .handler = home_post_handler, .user_ctx = nullptr},
    };
    for (const httpd_uri_t& route : routes) {
        httpd_register_uri_handler(server, &route);
    }

    char logged_ip[MAX_WIFI_IP_LEN] = "";
    while (true) {
        HarnessInfoSnapshot info;
        store_get_harness_info(ctx->store, &info);
        if (info.wifi_connected && strcmp(logged_ip, info.ip_address) != 0) {
            strlcpy(logged_ip, info.ip_address, sizeof(logged_ip));
            ESP_LOGI(TAG, "harness ready at http://%s:%u/", logged_ip, HARNESS_HTTP_PORT);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void launch_harness(HarnessTaskArgs* args) {
    harness_ctx = args;
    gesture_mutex = xSemaphoreCreateMutex();

    constexpr size_t framebuffer_len = (DISPLAY_HEIGHT / 2) * DISPLAY_WIDTH; // 4bpp is the larger mode
    framebuffer_copy = static_cast<uint8_t*>(heap_caps_malloc(framebuffer_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (framebuffer_copy == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate screenshot buffer, harness disabled");
        return;
    }

    xTaskCreate(harness_task, "harness", 4096, args, 1, nullptr);
}
