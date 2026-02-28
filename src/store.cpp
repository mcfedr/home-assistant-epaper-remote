#include "store.h"
#include "boards.h"
#include "climate_value.h"
#include "esp_log.h"
#include "esp_system.h"
#include <cctype>
#include <cstring>

static const char* TAG = "store";

static void notify_ui(EntityStore* store) {
    if (store->ui_task) {
        xTaskNotifyGive(store->ui_task);
    }
}

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

static void fallback_entity_name(const char* entity_id, char* out, size_t out_len) {
    if (out_len == 0) {
        return;
    }

    const char* name = entity_id;
    const char* dot = strchr(entity_id, '.');
    if (dot && dot[1]) {
        name = dot + 1;
    }

    size_t out_idx = 0;
    bool upper_next = true;
    for (size_t i = 0; name[i] != '\0' && out_idx + 1 < out_len; i++) {
        char ch = name[i];
        if (ch == '_' || ch == '-') {
            if (out_idx > 0 && out[out_idx - 1] != ' ') {
                out[out_idx++] = ' ';
            }
            upper_next = true;
            continue;
        }

        if (upper_next) {
            out[out_idx++] = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
            upper_next = false;
        } else {
            out[out_idx++] = ch;
        }
    }
    out[out_idx] = '\0';
}

static bool is_separator(char ch) {
    return ch == ' ' || ch == '_' || ch == '-';
}

static bool starts_with_room_prefix(const char* name, const char* room_name, size_t* prefix_end) {
    size_t i = 0;
    size_t j = 0;

    while (name[i] == ' ') {
        i++;
    }
    while (room_name[j] == ' ') {
        j++;
    }

    while (room_name[j] != '\0') {
        if (name[i] == '\0') {
            return false;
        }
        if (tolower(static_cast<unsigned char>(name[i])) != tolower(static_cast<unsigned char>(room_name[j]))) {
            return false;
        }
        i++;
        j++;
    }

    if (name[i] == '\0') {
        *prefix_end = i;
        return true;
    }

    if (!is_separator(name[i])) {
        return false;
    }

    while (is_separator(name[i])) {
        i++;
    }

    *prefix_end = i;
    return true;
}

static void trim_entity_name_for_room(const char* display_name, const char* room_name, char* out, size_t out_len) {
    if (!display_name || display_name[0] == '\0') {
        out[0] = '\0';
        return;
    }

    size_t prefix_end = 0;
    if (room_name && room_name[0] != '\0' && starts_with_room_prefix(display_name, room_name, &prefix_end) &&
        display_name[prefix_end] != '\0') {
        copy_string(out, out_len, display_name + prefix_end);
        return;
    }

    copy_string(out, out_len, display_name);
}

static int16_t find_entity_index(const EntityStore* store, const char* entity_id) {
    for (uint8_t i = 0; i < store->entity_count; i++) {
        if (strcmp(store->entities[i].entity_id, entity_id) == 0) {
            return i;
        }
    }
    return -1;
}

static uint8_t list_page_count(uint8_t item_count) {
    if (item_count == 0) {
        return 1;
    }
    return static_cast<uint8_t>((item_count + ROOM_LIST_ROOMS_PER_PAGE - 1) / ROOM_LIST_ROOMS_PER_PAGE);
}

static uint8_t wifi_list_page_count(uint8_t item_count) {
    if (item_count == 0) {
        return 1;
    }
    return static_cast<uint8_t>((item_count + WIFI_NETWORKS_PER_PAGE - 1) / WIFI_NETWORKS_PER_PAGE);
}

static bool float_approx_equal(float a, float b, float epsilon = 0.05f) {
    float delta = a - b;
    if (delta < 0.0f) {
        delta = -delta;
    }
    return delta <= epsilon;
}

static bool standby_forecast_day_equal(const StandbyForecastDay& a, const StandbyForecastDay& b) {
    return strcmp(a.day_label, b.day_label) == 0 && strcmp(a.condition, b.condition) == 0 && a.high_valid == b.high_valid &&
           (!a.high_valid || float_approx_equal(a.high_c, b.high_c)) && a.low_valid == b.low_valid &&
           (!a.low_valid || float_approx_equal(a.low_c, b.low_c));
}

static uint8_t room_count_for_floor_locked(const EntityStore* store, int8_t floor_idx) {
    if (floor_idx < 0 || floor_idx >= static_cast<int8_t>(store->floor_count)) {
        return 0;
    }

    uint8_t count = 0;
    for (uint8_t room_idx = 0; room_idx < store->room_count; room_idx++) {
        if (store->rooms[room_idx].floor_idx == floor_idx) {
            count++;
        }
    }
    return count;
}

static bool entity_visible_in_room_controls_locked(const EntityStore* store, uint8_t entity_idx) {
    const HomeAssistantEntity& entity = store->entities[entity_idx];
    if (entity.command_type == CommandType::SetClimateModeAndTemperature) {
        return entity.climate_hvac_modes_known && entity.climate_is_ac;
    }
    return true;
}

static uint16_t room_controls_light_height_locked(const EntityStore* store, int8_t room_idx) {
    (void)store;
    (void)room_idx;
    return ROOM_CONTROLS_LIGHT_MIN_HEIGHT;
}

static uint8_t room_controls_page_count_locked(const EntityStore* store, int8_t room_idx) {
    if (room_idx < 0 || room_idx >= static_cast<int8_t>(store->room_count)) {
        return 1;
    }

    const Room& room = store->rooms[room_idx];
    if (room.entity_count == 0) {
        return 1;
    }

    const uint16_t light_height = room_controls_light_height_locked(store, room_idx);
    const uint16_t display_bottom = DISPLAY_HEIGHT - ROOM_CONTROLS_BOTTOM_PADDING;

    uint8_t page_count = 1;
    uint16_t pos_y = ROOM_CONTROLS_ITEM_START_Y;
    uint8_t light_col = 0;
    bool impossible = false;

    auto start_new_page = [&]() {
        if (page_count < 255) {
            page_count++;
        }
        pos_y = ROOM_CONTROLS_ITEM_START_Y;
        light_col = 0;
    };

    auto place_entity = [&](CommandType command_type) {
        const bool is_cover = command_type == CommandType::SetCoverOpenClose;
        const bool is_climate = command_type == CommandType::SetClimateModeAndTemperature;
        const bool is_light = !is_climate && !is_cover;
        const uint16_t full_height = is_climate ? ROOM_CONTROLS_CLIMATE_HEIGHT : ROOM_CONTROLS_COVER_HEIGHT;
        while (true) {
            if (!is_light) {
                uint16_t row_y = pos_y;
                if (light_col != 0) {
                    row_y = static_cast<uint16_t>(row_y + light_height + ROOM_CONTROLS_ITEM_GAP);
                }
                if (row_y + full_height <= display_bottom) {
                    pos_y = static_cast<uint16_t>(row_y + full_height + ROOM_CONTROLS_ITEM_GAP);
                    light_col = 0;
                    return;
                }
                if (row_y == ROOM_CONTROLS_ITEM_START_Y && light_col == 0) {
                    impossible = true;
                    return;
                }
                start_new_page();
            } else {
                if (pos_y + light_height <= display_bottom) {
                    if (light_col == 0) {
                        light_col = 1;
                    } else {
                        light_col = 0;
                        pos_y = static_cast<uint16_t>(pos_y + light_height + ROOM_CONTROLS_ITEM_GAP);
                    }
                    return;
                }
                if (pos_y == ROOM_CONTROLS_ITEM_START_Y && light_col == 0) {
                    impossible = true;
                    return;
                }
                start_new_page();
            }
        }
    };

    for (uint8_t pass = 0; pass < 3 && !impossible; pass++) {
        for (uint8_t i = 0; i < room.entity_count && !impossible; i++) {
            uint8_t entity_idx = room.entity_ids[i];
            if (!entity_visible_in_room_controls_locked(store, entity_idx)) {
                continue;
            }
            CommandType command_type = store->entities[entity_idx].command_type;
            const uint8_t group = command_type == CommandType::SetClimateModeAndTemperature
                                      ? 0
                                      : (command_type == CommandType::SetCoverOpenClose ? 1 : 2);
            if (group != pass) {
                continue;
            }
            place_entity(command_type);
        }
    }

    return page_count;
}

void store_init(EntityStore* store) {
    store->mutex = xSemaphoreCreateMutex();
    store->event_group = xEventGroupCreate();
    store->last_interaction_ms = static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
    store->standby_last_refresh_ms = store->last_interaction_ms;
}

void store_set_wifi_state(EntityStore* store, ConnState state) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    ConnState previous_state = store->wifi;
    store->wifi = state;
    if (previous_state != state) {
        store->settings_revision++;
    }
    xSemaphoreGive(store->mutex);

    if (state != previous_state) {
        if (state == ConnState::Up) {
            xEventGroupSetBits(store->event_group, BIT_WIFI_UP);
        } else {
            xEventGroupClearBits(store->event_group, BIT_WIFI_UP);
        }
        notify_ui(store);
    }
}

void store_set_hass_state(EntityStore* store, ConnState state) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    ConnState previous_state = store->home_assistant;
    store->home_assistant = state;
    xSemaphoreGive(store->mutex);

    if (state != previous_state) {
        notify_ui(store);
    }
}

void store_update_value(EntityStore* store, uint8_t entity_idx, uint8_t value) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    HomeAssistantEntity& entity = store->entities[entity_idx];
    uint8_t previous_value = entity.current_value;
    entity.current_value = value;
    xSemaphoreGive(store->mutex);

    if (previous_value != value) {
        notify_ui(store);
    }
}

void store_send_command(EntityStore* store, uint8_t entity_idx, uint8_t value) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    HomeAssistantEntity& entity = store->entities[entity_idx];
    entity.current_value = value;
    entity.command_value = value;
    entity.command_pending = true;
    xSemaphoreGive(store->mutex);

    ESP_LOGI(TAG, "Sending command to update entity %s to value %d", store->entities[entity_idx].entity_id, value);

    if (store->home_assistant_task) {
        xTaskNotifyGive(store->home_assistant_task);
    }
    notify_ui(store);
}

bool store_get_pending_command(EntityStore* store, Command* command) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);

    for (uint8_t entity_idx = 0; entity_idx < store->entity_count; ++entity_idx) {
        HomeAssistantEntity& entity = store->entities[entity_idx];
        if (entity.command_pending) {
            command->entity_id = entity.entity_id;
            command->entity_idx = entity_idx;
            command->type = entity.command_type;
            command->value = entity.command_value;
            xSemaphoreGive(store->mutex);
            return true;
        }
    }

    xSemaphoreGive(store->mutex);
    return false;
}

void store_ack_pending_command(EntityStore* store, const Command* command) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);

    HomeAssistantEntity& entity = store->entities[command->entity_idx];
    if (entity.command_value == command->value) {
        entity.command_pending = false;
    }

    xSemaphoreGive(store->mutex);
}

void store_begin_room_sync(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    store->floor_count = 0;
    store->room_count = 0;
    store->entity_count = 0;
    store->selected_floor = -1;
    store->floor_list_page = 0;
    store->selected_room = -1;
    store->room_list_page = 0;
    store->room_controls_page = 0;
    store->rooms_loaded = false;
    store->rooms_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
}

void store_finish_room_sync(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    uint8_t floor_pages = list_page_count(store->floor_count);
    if (store->floor_list_page >= floor_pages) {
        store->floor_list_page = floor_pages - 1;
    }

    if (store->selected_floor >= static_cast<int8_t>(store->floor_count)) {
        store->selected_floor = -1;
    }

    if (store->selected_floor >= 0) {
        uint8_t room_pages = list_page_count(room_count_for_floor_locked(store, store->selected_floor));
        if (store->room_list_page >= room_pages) {
            store->room_list_page = room_pages - 1;
        }
    } else {
        store->room_list_page = 0;
        store->selected_room = -1;
    }

    if (store->selected_room >= 0) {
        if (store->selected_room >= static_cast<int8_t>(store->room_count) ||
            store->rooms[store->selected_room].floor_idx != store->selected_floor) {
            store->selected_room = -1;
            store->room_controls_page = 0;
        } else {
            uint8_t room_controls_pages = room_controls_page_count_locked(store, store->selected_room);
            if (store->room_controls_page >= room_controls_pages) {
                store->room_controls_page = room_controls_pages - 1;
            }
        }
    } else {
        store->room_controls_page = 0;
    }

    store->rooms_loaded = true;
    store->rooms_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
}

int8_t store_add_floor(EntityStore* store, const char* floor_name, const char* icon_name) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);

    if (store->floor_count >= MAX_FLOORS) {
        xSemaphoreGive(store->mutex);
        return -1;
    }

    uint8_t idx = store->floor_count++;
    Floor& floor = store->floors[idx];
    memset(&floor, 0, sizeof(Floor));
    copy_string(floor.name, sizeof(floor.name), floor_name);
    copy_string(floor.icon, sizeof(floor.icon), icon_name);

    xSemaphoreGive(store->mutex);
    return static_cast<int8_t>(idx);
}

int8_t store_add_room(EntityStore* store, const char* room_name, const char* icon_name, int8_t floor_idx) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);

    if (floor_idx < 0 || floor_idx >= static_cast<int8_t>(store->floor_count)) {
        xSemaphoreGive(store->mutex);
        return -1;
    }

    if (store->room_count >= MAX_ROOMS) {
        xSemaphoreGive(store->mutex);
        return -1;
    }

    uint8_t idx = store->room_count++;
    Room& room = store->rooms[idx];
    memset(&room, 0, sizeof(Room));
    copy_string(room.name, sizeof(room.name), room_name);
    copy_string(room.icon, sizeof(room.icon), icon_name);
    room.floor_idx = floor_idx;

    xSemaphoreGive(store->mutex);
    return static_cast<int8_t>(idx);
}

int16_t store_find_room(EntityStore* store, const char* room_name) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);

    int16_t result = -1;
    for (uint8_t idx = 0; idx < store->room_count; idx++) {
        if (strcmp(store->rooms[idx].name, room_name) == 0) {
            result = idx;
            break;
        }
    }

    xSemaphoreGive(store->mutex);
    return result;
}

int8_t store_add_entity_to_room(EntityStore* store, uint8_t room_idx, EntityConfig entity, const char* display_name) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);

    if (room_idx >= store->room_count) {
        xSemaphoreGive(store->mutex);
        return -1;
    }

    const char* room_name = store->rooms[room_idx].name;
    int16_t entity_idx = find_entity_index(store, entity.entity_id);
    if (entity_idx == -1) {
        if (store->entity_count >= MAX_ENTITIES) {
            xSemaphoreGive(store->mutex);
            return -1;
        }
        entity_idx = store->entity_count++;
        HomeAssistantEntity& new_entity = store->entities[entity_idx];
        memset(&new_entity, 0, sizeof(HomeAssistantEntity));
        copy_string(new_entity.entity_id, sizeof(new_entity.entity_id), entity.entity_id);
        if (display_name && display_name[0]) {
            char trimmed_name[MAX_ENTITY_NAME_LEN];
            trim_entity_name_for_room(display_name, room_name, trimmed_name, sizeof(trimmed_name));
            copy_string(new_entity.display_name, sizeof(new_entity.display_name), trimmed_name);
        } else {
            fallback_entity_name(entity.entity_id, new_entity.display_name, sizeof(new_entity.display_name));
        }
        new_entity.command_type = entity.command_type;
        if (new_entity.command_type == CommandType::SetClimateModeAndTemperature) {
            new_entity.climate_mode_mask = CLIMATE_MODE_MASK_DEFAULT;
            new_entity.climate_hvac_modes_known = false;
            new_entity.climate_is_ac = false;
            new_entity.current_value = climate_pack_value(ClimateMode::Off, climate_celsius_to_steps(20.0f));
        }
    } else if (display_name && display_name[0]) {
        char trimmed_name[MAX_ENTITY_NAME_LEN];
        trim_entity_name_for_room(display_name, room_name, trimmed_name, sizeof(trimmed_name));
        copy_string(store->entities[entity_idx].display_name, sizeof(store->entities[entity_idx].display_name), trimmed_name);
    }

    Room& room = store->rooms[room_idx];
    for (uint8_t i = 0; i < room.entity_count; i++) {
        if (room.entity_ids[i] == entity_idx) {
            xSemaphoreGive(store->mutex);
            return static_cast<int8_t>(entity_idx);
        }
    }

    if (room.entity_count >= MAX_ENTITIES) {
        xSemaphoreGive(store->mutex);
        return -1;
    }

    room.entity_ids[room.entity_count++] = static_cast<uint8_t>(entity_idx);
    xSemaphoreGive(store->mutex);
    return static_cast<int8_t>(entity_idx);
}

bool store_select_room(EntityStore* store, int8_t room_idx) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);

    if (room_idx < -1 || room_idx >= static_cast<int8_t>(store->room_count)) {
        xSemaphoreGive(store->mutex);
        return false;
    }

    if (room_idx >= 0) {
        if (store->selected_floor < 0 || store->selected_floor >= static_cast<int8_t>(store->floor_count) ||
            store->rooms[room_idx].floor_idx != store->selected_floor) {
            xSemaphoreGive(store->mutex);
            return false;
        }
    }

    if (store->selected_room == room_idx) {
        xSemaphoreGive(store->mutex);
        return true;
    }

    store->selected_room = room_idx;
    store->room_controls_page = 0;
    store->rooms_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
    return true;
}

bool store_select_floor(EntityStore* store, int8_t floor_idx) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);

    if (floor_idx < -1 || floor_idx >= static_cast<int8_t>(store->floor_count)) {
        xSemaphoreGive(store->mutex);
        return false;
    }

    if (store->selected_floor == floor_idx) {
        xSemaphoreGive(store->mutex);
        return true;
    }

    store->selected_floor = floor_idx;
    store->selected_room = -1;
    store->room_list_page = 0;
    store->room_controls_page = 0;
    store->rooms_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
    return true;
}

bool store_go_home(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);

    const bool changed = store->selected_floor != -1 || store->selected_room != -1 || store->floor_list_page != 0 ||
                         store->room_list_page != 0 || store->room_controls_page != 0 || store->settings_mode != SettingsMode::None ||
                         store->standby_active;

    store->selected_floor = -1;
    store->selected_room = -1;
    store->floor_list_page = 0;
    store->room_list_page = 0;
    store->room_controls_page = 0;
    store->settings_mode = SettingsMode::None;
    if (store->standby_active) {
        store->standby_active = false;
        store->standby_data_dirty = false;
        store->standby_revision++;
    }

    if (changed) {
        store->rooms_revision++;
        store->settings_revision++;
    }

    xSemaphoreGive(store->mutex);

    if (changed) {
        notify_ui(store);
    }

    return true;
}

bool store_shift_floor_list_page(EntityStore* store, int8_t delta) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);

    int16_t page = static_cast<int16_t>(store->floor_list_page) + delta;
    uint8_t pages = list_page_count(store->floor_count);
    if (page < 0) {
        page = 0;
    } else if (page >= pages) {
        page = pages - 1;
    }

    if (store->floor_list_page == static_cast<uint8_t>(page)) {
        xSemaphoreGive(store->mutex);
        return false;
    }

    store->floor_list_page = static_cast<uint8_t>(page);
    store->rooms_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
    return true;
}

bool store_shift_room_list_page(EntityStore* store, int8_t delta) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);

    if (store->selected_floor < 0 || store->selected_floor >= static_cast<int8_t>(store->floor_count)) {
        xSemaphoreGive(store->mutex);
        return false;
    }

    int16_t page = static_cast<int16_t>(store->room_list_page) + delta;
    uint8_t pages = list_page_count(room_count_for_floor_locked(store, store->selected_floor));
    if (page < 0) {
        page = 0;
    } else if (page >= pages) {
        page = pages - 1;
    }

    if (store->room_list_page == static_cast<uint8_t>(page)) {
        xSemaphoreGive(store->mutex);
        return false;
    }

    store->room_list_page = static_cast<uint8_t>(page);
    store->rooms_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
    return true;
}

bool store_shift_room_controls_page(EntityStore* store, int8_t delta) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);

    if (store->selected_room < 0 || store->selected_room >= static_cast<int8_t>(store->room_count)) {
        xSemaphoreGive(store->mutex);
        return false;
    }

    int16_t page = static_cast<int16_t>(store->room_controls_page) + delta;
    uint8_t pages = room_controls_page_count_locked(store, store->selected_room);
    if (page < 0) {
        page = 0;
    } else if (page >= pages) {
        page = pages - 1;
    }

    if (store->room_controls_page == static_cast<uint8_t>(page)) {
        xSemaphoreGive(store->mutex);
        return false;
    }

    store->room_controls_page = static_cast<uint8_t>(page);
    store->rooms_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
    return true;
}

uint8_t store_get_room_count(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    uint8_t room_count = room_count_for_floor_locked(store, store->selected_floor);
    xSemaphoreGive(store->mutex);
    return room_count;
}

void store_get_floor_list_snapshot(EntityStore* store, FloorListSnapshot* snapshot) {
    memset(snapshot, 0, sizeof(FloorListSnapshot));
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    snapshot->floor_count = store->floor_count;
    for (uint8_t floor_idx = 0; floor_idx < store->floor_count; floor_idx++) {
        copy_string(snapshot->floor_names[floor_idx], MAX_FLOOR_NAME_LEN, store->floors[floor_idx].name);
        copy_string(snapshot->floor_icons[floor_idx], MAX_ICON_NAME_LEN, store->floors[floor_idx].icon);
    }
    xSemaphoreGive(store->mutex);
}

bool store_get_room_list_snapshot(EntityStore* store, int8_t floor_idx, RoomListSnapshot* snapshot) {
    memset(snapshot, 0, sizeof(RoomListSnapshot));
    xSemaphoreTake(store->mutex, portMAX_DELAY);

    if (floor_idx < 0 || floor_idx >= static_cast<int8_t>(store->floor_count)) {
        xSemaphoreGive(store->mutex);
        return false;
    }

    copy_string(snapshot->floor_name, sizeof(snapshot->floor_name), store->floors[floor_idx].name);
    for (uint8_t room_idx = 0; room_idx < store->room_count; room_idx++) {
        if (store->rooms[room_idx].floor_idx != floor_idx) {
            continue;
        }

        uint8_t snapshot_idx = snapshot->room_count++;
        snapshot->room_indices[snapshot_idx] = static_cast<int8_t>(room_idx);
        copy_string(snapshot->room_names[snapshot_idx], MAX_ROOM_NAME_LEN, store->rooms[room_idx].name);
        copy_string(snapshot->room_icons[snapshot_idx], MAX_ICON_NAME_LEN, store->rooms[room_idx].icon);
    }
    xSemaphoreGive(store->mutex);
    return true;
}

bool store_get_room_controls_snapshot(EntityStore* store, int8_t room_idx, RoomControlsSnapshot* snapshot) {
    memset(snapshot, 0, sizeof(RoomControlsSnapshot));
    xSemaphoreTake(store->mutex, portMAX_DELAY);

    if (room_idx < 0 || room_idx >= static_cast<int8_t>(store->room_count)) {
        xSemaphoreGive(store->mutex);
        return false;
    }

    Room& room = store->rooms[room_idx];
    copy_string(snapshot->room_name, sizeof(snapshot->room_name), room.name);
    for (uint8_t i = 0; i < room.entity_count; i++) {
        uint8_t entity_idx = room.entity_ids[i];
        if (entity_visible_in_room_controls_locked(store, entity_idx)) {
            snapshot->entity_count++;
        }
    }
    if (snapshot->entity_count > MAX_ENTITIES) {
        snapshot->entity_count = MAX_ENTITIES;
        snapshot->truncated = true;
    }

    uint8_t snapshot_idx = 0;
    auto append_entity_to_snapshot = [&](uint8_t entity_idx) {
        if (snapshot_idx >= snapshot->entity_count) {
            return;
        }

        snapshot->entity_ids[snapshot_idx] = entity_idx;
        snapshot->entity_types[snapshot_idx] = store->entities[entity_idx].command_type;
        snapshot->entity_climate_mode_masks[snapshot_idx] = store->entities[entity_idx].climate_mode_mask;
        copy_string(snapshot->entity_names[snapshot_idx], MAX_ENTITY_NAME_LEN, store->entities[entity_idx].display_name);
        snapshot_idx++;
    };

    // Always place climate widgets first, then covers, then lights.
    for (uint8_t i = 0; i < room.entity_count && snapshot_idx < snapshot->entity_count; i++) {
        uint8_t entity_idx = room.entity_ids[i];
        if (!entity_visible_in_room_controls_locked(store, entity_idx)) {
            continue;
        }
        if (store->entities[entity_idx].command_type == CommandType::SetClimateModeAndTemperature) {
            append_entity_to_snapshot(entity_idx);
        }
    }
    for (uint8_t i = 0; i < room.entity_count && snapshot_idx < snapshot->entity_count; i++) {
        uint8_t entity_idx = room.entity_ids[i];
        if (!entity_visible_in_room_controls_locked(store, entity_idx)) {
            continue;
        }
        if (store->entities[entity_idx].command_type == CommandType::SetCoverOpenClose) {
            append_entity_to_snapshot(entity_idx);
        }
    }
    for (uint8_t i = 0; i < room.entity_count && snapshot_idx < snapshot->entity_count; i++) {
        uint8_t entity_idx = room.entity_ids[i];
        if (!entity_visible_in_room_controls_locked(store, entity_idx)) {
            continue;
        }
        if (store->entities[entity_idx].command_type != CommandType::SetClimateModeAndTemperature &&
            store->entities[entity_idx].command_type != CommandType::SetCoverOpenClose) {
            append_entity_to_snapshot(entity_idx);
        }
    }

    xSemaphoreGive(store->mutex);
    return true;
}

bool store_open_settings(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    const bool changed = store->settings_mode != SettingsMode::Menu;
    store->settings_mode = SettingsMode::Menu;
    if (changed) {
        store->settings_revision++;
    }
    xSemaphoreGive(store->mutex);
    if (changed) {
        notify_ui(store);
    }
    return true;
}

bool store_open_wifi_settings(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    const bool changed = store->settings_mode != SettingsMode::Wifi;
    store->settings_mode = SettingsMode::Wifi;
    uint8_t page_count = wifi_list_page_count(store->wifi_network_count);
    if (store->wifi_list_page >= page_count) {
        store->wifi_list_page = static_cast<uint8_t>(page_count - 1);
    }
    if (changed) {
        store->settings_revision++;
    }
    xSemaphoreGive(store->mutex);
    if (changed) {
        notify_ui(store);
    }
    return true;
}

bool store_open_wifi_password(EntityStore* store, const char* ssid) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    const bool mode_changed = store->settings_mode != SettingsMode::WifiPassword;
    store->settings_mode = SettingsMode::WifiPassword;
    copy_string(store->wifi_target_ssid, sizeof(store->wifi_target_ssid), ssid);
    store->wifi_password_input[0] = '\0';
    store->wifi_password_symbols = false;
    store->wifi_password_shift = false;
    store->wifi_connect_error[0] = '\0';
    store->settings_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
    (void)mode_changed;
    return true;
}

bool store_open_standby(EntityStore* store, uint32_t now_ms) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    const bool can_activate = store->wifi == ConnState::Up && store->home_assistant == ConnState::Up && store->rooms_loaded;
    const bool changed = can_activate && (!store->standby_active || store->settings_mode != SettingsMode::None);
    if (changed) {
        store->settings_mode = SettingsMode::None;
        store->standby_active = true;
        store->standby_last_refresh_ms = now_ms;
        store->standby_data_dirty = false;
        store->standby_revision++;
        store->settings_revision++;
    }
    xSemaphoreGive(store->mutex);
    if (changed) {
        notify_ui(store);
    }
    return changed;
}

bool store_settings_back(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    SettingsMode next_mode = store->settings_mode;
    switch (store->settings_mode) {
    case SettingsMode::WifiPassword:
        next_mode = SettingsMode::Wifi;
        break;
    case SettingsMode::Wifi:
        next_mode = SettingsMode::Menu;
        break;
    case SettingsMode::Menu:
        next_mode = SettingsMode::None;
        break;
    default:
        next_mode = SettingsMode::None;
        break;
    }
    const bool changed = next_mode != store->settings_mode;
    store->settings_mode = next_mode;
    if (changed) {
        store->settings_revision++;
    }
    xSemaphoreGive(store->mutex);
    if (changed) {
        notify_ui(store);
    }
    return changed;
}

bool store_close_settings(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    const bool changed = store->settings_mode != SettingsMode::None;
    store->settings_mode = SettingsMode::None;
    if (changed) {
        store->settings_revision++;
    }
    xSemaphoreGive(store->mutex);
    if (changed) {
        notify_ui(store);
    }
    return changed;
}

bool store_shift_wifi_list_page(EntityStore* store, int8_t delta) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    int16_t page = static_cast<int16_t>(store->wifi_list_page) + delta;
    uint8_t pages = wifi_list_page_count(store->wifi_network_count);
    if (page < 0) {
        page = 0;
    } else if (page >= pages) {
        page = pages - 1;
    }
    if (store->wifi_list_page == static_cast<uint8_t>(page)) {
        xSemaphoreGive(store->mutex);
        return false;
    }
    store->wifi_list_page = static_cast<uint8_t>(page);
    store->settings_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
    return true;
}

bool store_set_wifi_password_symbols(EntityStore* store, bool symbols) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    if (store->wifi_password_symbols == symbols) {
        xSemaphoreGive(store->mutex);
        return false;
    }
    store->wifi_password_symbols = symbols;
    store->wifi_password_shift = false;
    store->settings_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
    return true;
}

bool store_toggle_wifi_password_shift(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    store->wifi_password_shift = !store->wifi_password_shift;
    store->settings_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
    return true;
}

bool store_append_wifi_password_char(EntityStore* store, char ch) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    size_t len = strlen(store->wifi_password_input);
    if (len >= MAX_WIFI_PASSWORD_LEN) {
        xSemaphoreGive(store->mutex);
        return false;
    }
    store->wifi_password_input[len] = ch;
    store->wifi_password_input[len + 1] = '\0';
    store->settings_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
    return true;
}

bool store_backspace_wifi_password(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    size_t len = strlen(store->wifi_password_input);
    if (len == 0) {
        xSemaphoreGive(store->mutex);
        return false;
    }
    store->wifi_password_input[len - 1] = '\0';
    store->settings_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
    return true;
}

bool store_clear_wifi_password(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    if (store->wifi_password_input[0] == '\0') {
        xSemaphoreGive(store->mutex);
        return false;
    }
    store->wifi_password_input[0] = '\0';
    store->settings_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
    return true;
}

void store_set_wifi_connection_info(EntityStore* store, bool connected, const char* ssid, const char* ip_address, int16_t rssi) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    bool changed = false;
    if (store->wifi_connected != connected) {
        store->wifi_connected = connected;
        changed = true;
    }
    int16_t rssi_delta = static_cast<int16_t>(store->wifi_rssi - rssi);
    if (rssi_delta < 0) {
        rssi_delta = static_cast<int16_t>(-rssi_delta);
    }
    // Avoid frequent full-screen redraws for minor RSSI jitter.
    if (!connected || !store->wifi_connected || rssi_delta >= 4) {
        if (store->wifi_rssi != rssi) {
            store->wifi_rssi = rssi;
            changed = true;
        }
    }
    if (strcmp(store->wifi_connected_ssid, ssid ? ssid : "") != 0) {
        copy_string(store->wifi_connected_ssid, sizeof(store->wifi_connected_ssid), ssid);
        changed = true;
    }
    if (strcmp(store->wifi_ip_address, ip_address ? ip_address : "") != 0) {
        copy_string(store->wifi_ip_address, sizeof(store->wifi_ip_address), ip_address);
        changed = true;
    }
    if (changed) {
        store->settings_revision++;
    }
    xSemaphoreGive(store->mutex);
    if (changed) {
        notify_ui(store);
    }
}

void store_set_wifi_scan_state(EntityStore* store, bool in_progress) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    if (store->wifi_scan_in_progress == in_progress) {
        xSemaphoreGive(store->mutex);
        return;
    }
    store->wifi_scan_in_progress = in_progress;
    store->settings_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
}

void store_set_wifi_scan_results(EntityStore* store, const WifiNetwork* networks, uint8_t count) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    if (count > MAX_WIFI_NETWORKS) {
        count = MAX_WIFI_NETWORKS;
    }
    memset(store->wifi_networks, 0, sizeof(store->wifi_networks));
    for (uint8_t i = 0; i < count; i++) {
        copy_string(store->wifi_networks[i].ssid, sizeof(store->wifi_networks[i].ssid), networks[i].ssid);
        store->wifi_networks[i].rssi = networks[i].rssi;
        store->wifi_networks[i].secure = networks[i].secure;
    }
    store->wifi_network_count = count;
    uint8_t pages = wifi_list_page_count(count);
    if (store->wifi_list_page >= pages) {
        store->wifi_list_page = static_cast<uint8_t>(pages - 1);
    }
    store->settings_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
}

void store_set_wifi_connecting(EntityStore* store, bool connecting) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    if (store->wifi_connecting == connecting) {
        xSemaphoreGive(store->mutex);
        return;
    }
    store->wifi_connecting = connecting;
    if (connecting) {
        store->wifi_connect_error[0] = '\0';
    }
    store->settings_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
}

void store_set_wifi_connect_error(EntityStore* store, const char* error) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    const char* safe_error = error ? error : "";
    if (strcmp(store->wifi_connect_error, safe_error) == 0) {
        xSemaphoreGive(store->mutex);
        return;
    }
    copy_string(store->wifi_connect_error, sizeof(store->wifi_connect_error), safe_error);
    store->settings_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
}

void store_set_wifi_profile(EntityStore* store, const char* ssid, bool custom_profile_active) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    bool changed = false;
    const char* safe_ssid = ssid ? ssid : "";
    if (store->wifi_custom_profile_active != custom_profile_active) {
        store->wifi_custom_profile_active = custom_profile_active;
        changed = true;
    }
    if (strcmp(store->wifi_profile_ssid, safe_ssid) != 0) {
        copy_string(store->wifi_profile_ssid, sizeof(store->wifi_profile_ssid), safe_ssid);
        changed = true;
    }
    if (changed) {
        store->settings_revision++;
    }
    xSemaphoreGive(store->mutex);
    if (changed) {
        notify_ui(store);
    }
}

void store_get_wifi_settings_snapshot(EntityStore* store, WifiSettingsSnapshot* snapshot) {
    memset(snapshot, 0, sizeof(WifiSettingsSnapshot));
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    snapshot->wifi_state = store->wifi;
    snapshot->connected = store->wifi_connected;
    snapshot->scan_in_progress = store->wifi_scan_in_progress;
    snapshot->connecting = store->wifi_connecting;
    snapshot->custom_profile_active = store->wifi_custom_profile_active;
    snapshot->rssi = store->wifi_rssi;
    snapshot->page = store->wifi_list_page;
    snapshot->network_count = store->wifi_network_count;
    copy_string(snapshot->connect_error, sizeof(snapshot->connect_error), store->wifi_connect_error);
    copy_string(snapshot->connected_ssid, sizeof(snapshot->connected_ssid), store->wifi_connected_ssid);
    copy_string(snapshot->profile_ssid, sizeof(snapshot->profile_ssid), store->wifi_profile_ssid);
    copy_string(snapshot->ip_address, sizeof(snapshot->ip_address), store->wifi_ip_address);
    for (uint8_t i = 0; i < store->wifi_network_count && i < MAX_WIFI_NETWORKS; i++) {
        copy_string(snapshot->networks[i].ssid, sizeof(snapshot->networks[i].ssid), store->wifi_networks[i].ssid);
        snapshot->networks[i].rssi = store->wifi_networks[i].rssi;
        snapshot->networks[i].secure = store->wifi_networks[i].secure;
    }
    xSemaphoreGive(store->mutex);
}

bool store_get_wifi_password_snapshot(EntityStore* store, WifiPasswordSnapshot* snapshot) {
    memset(snapshot, 0, sizeof(WifiPasswordSnapshot));
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    if (store->settings_mode != SettingsMode::WifiPassword) {
        xSemaphoreGive(store->mutex);
        return false;
    }
    snapshot->symbols = store->wifi_password_symbols;
    snapshot->shift = store->wifi_password_shift;
    snapshot->connecting = store->wifi_connecting;
    copy_string(snapshot->target_ssid, sizeof(snapshot->target_ssid), store->wifi_target_ssid);
    copy_string(snapshot->password, sizeof(snapshot->password), store->wifi_password_input);
    copy_string(snapshot->connect_error, sizeof(snapshot->connect_error), store->wifi_connect_error);
    xSemaphoreGive(store->mutex);
    return true;
}

void store_note_interaction(EntityStore* store, uint32_t now_ms) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    store->last_interaction_ms = now_ms;
    xSemaphoreGive(store->mutex);
}

void store_poll_standby_timeout(EntityStore* store, uint32_t now_ms) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);

    const bool can_activate = store->settings_mode == SettingsMode::None && store->wifi == ConnState::Up &&
                              store->home_assistant == ConnState::Up && store->rooms_loaded;
    const bool idle_timed_out = static_cast<uint32_t>(now_ms - store->last_interaction_ms) >= STANDBY_IDLE_TIMEOUT_MS;
    bool changed = false;

    if (store->standby_active) {
        if (!can_activate) {
            store->standby_active = false;
            store->standby_data_dirty = false;
            store->standby_revision++;
            changed = true;
        } else {
            const uint32_t elapsed = static_cast<uint32_t>(now_ms - store->standby_last_refresh_ms);
            if (elapsed >= STANDBY_REFRESH_INTERVAL_MS) {
                store->standby_last_refresh_ms = now_ms;
                store->standby_data_dirty = false;
                store->standby_revision++;
                changed = true;
            }
        }
    } else if (can_activate && idle_timed_out) {
        store->standby_active = true;
        store->standby_last_refresh_ms = now_ms;
        store->standby_data_dirty = false;
        store->standby_revision++;
        changed = true;
    }

    xSemaphoreGive(store->mutex);
    if (changed) {
        notify_ui(store);
    }
}

void store_set_standby_weather(EntityStore* store, const char* condition, bool has_temperature, float temperature_c) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    bool changed = false;
    bool should_notify = false;

    char normalized_condition[MAX_STANDBY_CONDITION_LEN];
    copy_string(normalized_condition, sizeof(normalized_condition), condition);
    if (strcmp(store->standby.weather_condition, normalized_condition) != 0) {
        copy_string(store->standby.weather_condition, sizeof(store->standby.weather_condition), normalized_condition);
        changed = true;
    }

    if (store->standby.weather_temperature_valid != has_temperature) {
        store->standby.weather_temperature_valid = has_temperature;
        changed = true;
    }
    if (has_temperature && !float_approx_equal(store->standby.weather_temperature_c, temperature_c)) {
        store->standby.weather_temperature_c = temperature_c;
        changed = true;
    }

    if (changed) {
        if (store->standby_active) {
            store->standby_data_dirty = true;
        } else {
            store->standby_revision++;
            should_notify = true;
        }
    }
    xSemaphoreGive(store->mutex);
    if (should_notify) {
        notify_ui(store);
    }
}

void store_set_standby_forecast(EntityStore* store, const StandbyForecastDay* days, uint8_t day_count) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    bool changed = false;
    bool should_notify = false;
    if (day_count > MAX_STANDBY_FORECAST_DAYS) {
        day_count = MAX_STANDBY_FORECAST_DAYS;
    }

    bool has_high = false;
    float high_c = 0.0f;
    bool has_low = false;
    float low_c = 0.0f;
    if (days && day_count > 0) {
        has_high = days[0].high_valid;
        high_c = days[0].high_c;
        has_low = days[0].low_valid;
        low_c = days[0].low_c;
    }

    if (store->standby.weather_high_valid != has_high) {
        store->standby.weather_high_valid = has_high;
        changed = true;
    }
    if (has_high && !float_approx_equal(store->standby.weather_high_c, high_c)) {
        store->standby.weather_high_c = high_c;
        changed = true;
    }
    if (store->standby.weather_low_valid != has_low) {
        store->standby.weather_low_valid = has_low;
        changed = true;
    }
    if (has_low && !float_approx_equal(store->standby.weather_low_c, low_c)) {
        store->standby.weather_low_c = low_c;
        changed = true;
    }

    if (store->standby.forecast_day_count != day_count) {
        store->standby.forecast_day_count = day_count;
        changed = true;
    }

    for (uint8_t idx = 0; idx < MAX_STANDBY_FORECAST_DAYS; idx++) {
        StandbyForecastDay next_day = {};
        if (days && idx < day_count) {
            next_day = days[idx];
        }
        if (!standby_forecast_day_equal(store->standby.forecast_days[idx], next_day)) {
            store->standby.forecast_days[idx] = next_day;
            changed = true;
        }
    }

    if (changed) {
        if (store->standby_active) {
            store->standby_data_dirty = true;
        } else {
            store->standby_revision++;
            should_notify = true;
        }
    }
    xSemaphoreGive(store->mutex);
    if (should_notify) {
        notify_ui(store);
    }
}

void store_set_standby_energy_metric(EntityStore* store, StandbyEnergyMetric metric, bool valid, float value) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    bool changed = false;
    bool should_notify = false;

    bool* valid_ptr = nullptr;
    float* value_ptr = nullptr;
    float epsilon = 0.05f;
    switch (metric) {
    case StandbyEnergyMetric::SolarGeneration:
        valid_ptr = &store->standby.solar_generation_valid;
        value_ptr = &store->standby.solar_generation_kwh;
        break;
    case StandbyEnergyMetric::GridInput:
        valid_ptr = &store->standby.grid_input_valid;
        value_ptr = &store->standby.grid_input_kwh;
        break;
    case StandbyEnergyMetric::GridExport:
        valid_ptr = &store->standby.grid_export_valid;
        value_ptr = &store->standby.grid_export_kwh;
        break;
    case StandbyEnergyMetric::BatteryUsage:
        valid_ptr = &store->standby.battery_usage_valid;
        value_ptr = &store->standby.battery_usage_kwh;
        break;
    case StandbyEnergyMetric::BatteryChargeEnergy:
        valid_ptr = &store->standby.battery_charge_energy_valid;
        value_ptr = &store->standby.battery_charge_energy_kwh;
        break;
    case StandbyEnergyMetric::BatteryCharge:
        valid_ptr = &store->standby.battery_charge_valid;
        value_ptr = &store->standby.battery_charge_pct;
        epsilon = 0.5f;
        break;
    case StandbyEnergyMetric::HouseUsage:
        valid_ptr = &store->standby.house_usage_valid;
        value_ptr = &store->standby.house_usage_kwh;
        break;
    default:
        xSemaphoreGive(store->mutex);
        return;
    }

    if (*valid_ptr != valid) {
        *valid_ptr = valid;
        changed = true;
    }
    if (valid && !float_approx_equal(*value_ptr, value, epsilon)) {
        *value_ptr = value;
        changed = true;
    }

    if (changed) {
        if (store->standby_active) {
            store->standby_data_dirty = true;
        } else {
            store->standby_revision++;
            should_notify = true;
        }
    }
    xSemaphoreGive(store->mutex);
    if (should_notify) {
        notify_ui(store);
    }
}

void store_get_standby_snapshot(EntityStore* store, StandbySnapshot* snapshot) {
    memset(snapshot, 0, sizeof(StandbySnapshot));
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    *snapshot = store->standby;
    xSemaphoreGive(store->mutex);
}

bool store_is_standby_active(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    const bool active = store->standby_active;
    xSemaphoreGive(store->mutex);
    return active;
}

void store_update_ui_state(EntityStore* store, const Screen* screen, UIState* ui_state) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);

    ui_state->selected_floor = store->selected_floor;
    ui_state->selected_room = store->selected_room;
    ui_state->floor_list_page = store->floor_list_page;
    ui_state->room_list_page = store->room_list_page;
    ui_state->room_controls_page = store->room_controls_page;
    ui_state->rooms_revision = store->rooms_revision;
    ui_state->wifi_list_page = store->wifi_list_page;
    ui_state->settings_revision = store->settings_revision;
    ui_state->standby_revision = store->standby_revision;

    if (store->settings_mode != SettingsMode::None) {
        switch (store->settings_mode) {
        case SettingsMode::Menu:
            ui_state->mode = UiMode::SettingsMenu;
            break;
        case SettingsMode::Wifi:
            ui_state->mode = UiMode::WifiSettings;
            break;
        case SettingsMode::WifiPassword:
            ui_state->mode = UiMode::WifiPassword;
            break;
        default:
            ui_state->mode = UiMode::SettingsMenu;
            break;
        }
    } else {
        // Handle wifi and home assistant state first
        if (store->wifi == ConnState::Up && store->home_assistant == ConnState::Up) {
            if (!store->rooms_loaded) {
                ui_state->mode = UiMode::Boot;
            } else if (store->standby_active) {
                ui_state->mode = UiMode::Standby;
            } else if (store->selected_floor < 0) {
                ui_state->mode = UiMode::FloorList;
            } else if (store->selected_room < 0) {
                ui_state->mode = UiMode::RoomList;
            } else {
                ui_state->mode = UiMode::RoomControls;
            }
        } else if (store->wifi == ConnState::Initializing) {
            ui_state->mode = UiMode::Boot;
        } else if (store->wifi == ConnState::InvalidCredentials) {
            ui_state->mode = UiMode::WifiDisconnected;
        } else if (store->wifi == ConnState::ConnectionError) {
            ui_state->mode = UiMode::WifiDisconnected;
        } else if (store->home_assistant == ConnState::Initializing) {
            ui_state->mode = UiMode::Boot;
        } else if (store->home_assistant == ConnState::InvalidCredentials) {
            ui_state->mode = UiMode::HassInvalidKey;
        } else if (store->home_assistant == ConnState::ConnectionError) {
            ui_state->mode = UiMode::HassDisconnected;
        } else {
            ui_state->mode = UiMode::GenericError;
        }
    }

    memset(ui_state->widget_values, 0, sizeof(ui_state->widget_values));
    for (uint8_t widget_idx = 0; widget_idx < screen->widget_count; widget_idx++) {
        uint8_t entity_id = screen->entity_ids[widget_idx];
        ui_state->widget_values[widget_idx] = store->entities[entity_id].current_value;
    }

    xSemaphoreGive(store->mutex);
}

void store_bump_rooms_revision(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    store->rooms_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
}

void store_wait_for_wifi_up(EntityStore* store) {
    xEventGroupWaitBits(store->event_group, BIT_WIFI_UP, pdFALSE, pdTRUE, portMAX_DELAY);
}

void store_flush_pending_commands(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    for (uint8_t entity_idx = 0; entity_idx < store->entity_count; ++entity_idx) {
        store->entities[entity_idx].command_pending = false;
    }
    xSemaphoreGive(store->mutex);
}

EntityRef store_add_entity(EntityStore* store, EntityConfig entity) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);

    if (store->entity_count >= MAX_ENTITIES) {
        xSemaphoreGive(store->mutex);
        esp_system_abort("too many entities declared !");
    }

    uint8_t entity_id = store->entity_count++;
    HomeAssistantEntity& new_entity = store->entities[entity_id];
    memset(&new_entity, 0, sizeof(HomeAssistantEntity));
    copy_string(new_entity.entity_id, sizeof(new_entity.entity_id), entity.entity_id);
    fallback_entity_name(entity.entity_id, new_entity.display_name, sizeof(new_entity.display_name));
    new_entity.command_type = entity.command_type;
    if (new_entity.command_type == CommandType::SetClimateModeAndTemperature) {
        new_entity.climate_mode_mask = CLIMATE_MODE_MASK_DEFAULT;
        new_entity.climate_hvac_modes_known = false;
        new_entity.climate_is_ac = false;
        new_entity.current_value = climate_pack_value(ClimateMode::Off, climate_celsius_to_steps(20.0f));
    }

    xSemaphoreGive(store->mutex);
    return EntityRef{.index = entity_id};
}
