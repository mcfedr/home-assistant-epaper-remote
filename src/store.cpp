#include "store.h"
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

void store_init(EntityStore* store) {
    store->mutex = xSemaphoreCreateMutex();
    store->event_group = xEventGroupCreate();
}

void store_set_wifi_state(EntityStore* store, ConnState state) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    ConnState previous_state = store->wifi;
    store->wifi = state;
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
        }
    }

    store->rooms_loaded = true;
    store->rooms_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
}

int8_t store_add_floor(EntityStore* store, const char* floor_name) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);

    if (store->floor_count >= MAX_FLOORS) {
        xSemaphoreGive(store->mutex);
        return -1;
    }

    uint8_t idx = store->floor_count++;
    Floor& floor = store->floors[idx];
    memset(&floor, 0, sizeof(Floor));
    copy_string(floor.name, sizeof(floor.name), floor_name);

    xSemaphoreGive(store->mutex);
    return static_cast<int8_t>(idx);
}

int8_t store_add_room(EntityStore* store, const char* room_name, int8_t floor_idx) {
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
    store->rooms_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
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
    snapshot->entity_count = room.entity_count;
    if (room.entity_count > MAX_WIDGETS_PER_SCREEN) {
        snapshot->entity_count = MAX_WIDGETS_PER_SCREEN;
        snapshot->truncated = true;
    }

    for (uint8_t i = 0; i < snapshot->entity_count; i++) {
        uint8_t entity_idx = room.entity_ids[i];
        snapshot->entity_ids[i] = entity_idx;
        copy_string(snapshot->entity_names[i], MAX_ENTITY_NAME_LEN, store->entities[entity_idx].display_name);
    }

    xSemaphoreGive(store->mutex);
    return true;
}

void store_update_ui_state(EntityStore* store, const Screen* screen, UIState* ui_state) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);

    ui_state->selected_floor = store->selected_floor;
    ui_state->selected_room = store->selected_room;
    ui_state->floor_list_page = store->floor_list_page;
    ui_state->room_list_page = store->room_list_page;
    ui_state->rooms_revision = store->rooms_revision;

    // Handle wifi and home assistant state first
    if (store->wifi == ConnState::Up && store->home_assistant == ConnState::Up) {
        if (!store->rooms_loaded) {
            ui_state->mode = UiMode::Boot;
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

    memset(ui_state->widget_values, 0, sizeof(ui_state->widget_values));
    for (uint8_t widget_idx = 0; widget_idx < screen->widget_count; widget_idx++) {
        uint8_t entity_id = screen->entity_ids[widget_idx];
        ui_state->widget_values[widget_idx] = store->entities[entity_id].current_value;
    }

    xSemaphoreGive(store->mutex);
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

    xSemaphoreGive(store->mutex);
    return EntityRef{.index = entity_id};
}
