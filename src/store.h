#pragma once
#include "constants.h"
#include "climate_value.h"
#include "entity_ref.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "screen.h"
#include "ui_state.h"
#include <cstdint>

enum class CommandType : uint8_t {
    SetLightBrightnessPercentage,
    SetClimateModeAndTemperature,
    SetCoverOpenClose,
    SetFanSpeedPercentage,
    SwitchOnOff,
    AutomationOnOff,
};

struct HomeAssistantEntity {
    char entity_id[MAX_ENTITY_ID_LEN];
    char display_name[MAX_ENTITY_NAME_LEN];
    CommandType command_type;
    uint8_t climate_mode_mask;
    bool climate_hvac_modes_known;
    bool climate_is_ac;
    uint8_t current_value;
    uint8_t command_value;
    bool command_pending;
};

struct EntityConfig {
    const char* entity_id;
    CommandType command_type;
};

enum class ConnState : uint8_t {
    Initializing,
    InvalidCredentials,
    ConnectionError,
    Up,
};

struct Room {
    char name[MAX_ROOM_NAME_LEN];
    char icon[MAX_ICON_NAME_LEN];
    int8_t floor_idx;
    uint8_t entity_ids[MAX_ENTITIES];
    uint8_t entity_count;
};

struct Floor {
    char name[MAX_FLOOR_NAME_LEN];
    char icon[MAX_ICON_NAME_LEN];
};

struct FloorListSnapshot {
    uint8_t floor_count;
    char floor_names[MAX_FLOORS][MAX_FLOOR_NAME_LEN];
    char floor_icons[MAX_FLOORS][MAX_ICON_NAME_LEN];
};

struct RoomListSnapshot {
    uint8_t room_count;
    int8_t room_indices[MAX_ROOMS];
    char floor_name[MAX_FLOOR_NAME_LEN];
    char room_names[MAX_ROOMS][MAX_ROOM_NAME_LEN];
    char room_icons[MAX_ROOMS][MAX_ICON_NAME_LEN];
};

struct RoomControlsSnapshot {
    char room_name[MAX_ROOM_NAME_LEN];
    uint8_t entity_count;
    uint8_t entity_ids[MAX_ENTITIES];
    CommandType entity_types[MAX_ENTITIES];
    uint8_t entity_climate_mode_masks[MAX_ENTITIES];
    char entity_names[MAX_ENTITIES][MAX_ENTITY_NAME_LEN];
    bool truncated;
};

struct EntityStore {
    ConnState wifi = ConnState::Initializing;
    ConnState home_assistant = ConnState::Initializing;

    Floor floors[MAX_FLOORS];
    uint8_t floor_count;
    int8_t selected_floor = -1;
    uint8_t floor_list_page = 0;

    Room rooms[MAX_ROOMS];
    uint8_t room_count;
    int8_t selected_room = -1;
    uint8_t room_list_page = 0;
    uint8_t room_controls_page = 0;
    bool rooms_loaded = false;
    uint32_t rooms_revision = 0;

    HomeAssistantEntity entities[MAX_ENTITIES];
    uint8_t entity_count;

    SemaphoreHandle_t mutex;
    TaskHandle_t home_assistant_task;
    TaskHandle_t ui_task;
    EventGroupHandle_t event_group = nullptr;
};

struct Command {
    CommandType type;
    const char* entity_id;
    uint8_t entity_idx;
    uint8_t value;
};

constexpr EventBits_t BIT_WIFI_UP = (1 << 0);

void store_init(EntityStore* store);
void store_set_wifi_state(EntityStore* store, ConnState state);
void store_set_hass_state(EntityStore* store, ConnState state);
void store_update_value(EntityStore* store, uint8_t entity_idx, uint8_t value);
void store_send_command(EntityStore* store, uint8_t entity_idx, uint8_t value);
bool store_get_pending_command(EntityStore* store, Command* command);
void store_ack_pending_command(EntityStore* store, const Command* command);
void store_begin_room_sync(EntityStore* store);
void store_finish_room_sync(EntityStore* store);
int8_t store_add_floor(EntityStore* store, const char* floor_name, const char* icon_name);
int8_t store_add_room(EntityStore* store, const char* room_name, const char* icon_name, int8_t floor_idx);
int16_t store_find_room(EntityStore* store, const char* room_name);
int8_t store_add_entity_to_room(EntityStore* store, uint8_t room_idx, EntityConfig entity, const char* display_name);
bool store_select_floor(EntityStore* store, int8_t floor_idx);
bool store_select_room(EntityStore* store, int8_t room_idx);
bool store_go_home(EntityStore* store);
bool store_shift_floor_list_page(EntityStore* store, int8_t delta);
bool store_shift_room_list_page(EntityStore* store, int8_t delta);
bool store_shift_room_controls_page(EntityStore* store, int8_t delta);
uint8_t store_get_room_count(EntityStore* store);
void store_get_floor_list_snapshot(EntityStore* store, FloorListSnapshot* snapshot);
bool store_get_room_list_snapshot(EntityStore* store, int8_t floor_idx, RoomListSnapshot* snapshot);
bool store_get_room_controls_snapshot(EntityStore* store, int8_t room_idx, RoomControlsSnapshot* snapshot);
void store_update_ui_state(EntityStore* store, const Screen* screen, UIState* ui_state);
void store_bump_rooms_revision(EntityStore* store);
void store_wait_for_wifi_up(EntityStore* store);
void store_flush_pending_commands(EntityStore* store);
EntityRef store_add_entity(EntityStore* store, EntityConfig entity);
