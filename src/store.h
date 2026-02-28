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

enum class SettingsMode : uint8_t {
    None,
    Menu,
    Wifi,
    WifiPassword,
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

struct WifiNetwork {
    char ssid[MAX_WIFI_SSID_LEN];
    int16_t rssi;
    bool secure;
};

struct WifiSettingsSnapshot {
    ConnState wifi_state;
    bool connected;
    bool scan_in_progress;
    bool connecting;
    bool custom_profile_active;
    char connect_error[MAX_WIFI_ERROR_LEN];
    char connected_ssid[MAX_WIFI_SSID_LEN];
    char profile_ssid[MAX_WIFI_SSID_LEN];
    char ip_address[MAX_WIFI_IP_LEN];
    int16_t rssi;
    uint8_t page;
    uint8_t network_count;
    WifiNetwork networks[MAX_WIFI_NETWORKS];
};

struct WifiPasswordSnapshot {
    bool symbols;
    bool shift;
    bool connecting;
    char target_ssid[MAX_WIFI_SSID_LEN];
    char password[MAX_WIFI_PASSWORD_LEN + 1];
    char connect_error[MAX_WIFI_ERROR_LEN];
};

enum class StandbyEnergyMetric : uint8_t {
    SolarGeneration,
    GridInput,
    GridExport,
    BatteryUsage,
    BatteryChargeEnergy,
    BatteryCharge,
    HouseUsage,
};

struct StandbyForecastDay {
    char day_label[MAX_STANDBY_DAY_LABEL_LEN];
    char condition[MAX_STANDBY_CONDITION_LEN];
    bool high_valid;
    float high_c;
    bool low_valid;
    float low_c;
};

struct StandbySnapshot {
    char weather_condition[MAX_STANDBY_CONDITION_LEN];
    bool weather_temperature_valid;
    float weather_temperature_c;
    bool weather_high_valid;
    float weather_high_c;
    bool weather_low_valid;
    float weather_low_c;
    uint8_t forecast_day_count;
    StandbyForecastDay forecast_days[MAX_STANDBY_FORECAST_DAYS];

    bool solar_generation_valid;
    float solar_generation_kwh;
    bool grid_input_valid;
    float grid_input_kwh;
    bool grid_export_valid;
    float grid_export_kwh;
    bool battery_usage_valid;
    float battery_usage_kwh;
    bool battery_charge_energy_valid;
    float battery_charge_energy_kwh;
    bool battery_charge_valid;
    float battery_charge_pct;
    bool house_usage_valid;
    float house_usage_kwh;
};

struct EntityStore {
    ConnState wifi = ConnState::Initializing;
    ConnState home_assistant = ConnState::Initializing;
    SettingsMode settings_mode = SettingsMode::None;

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
    uint32_t settings_revision = 0;

    HomeAssistantEntity entities[MAX_ENTITIES];
    uint8_t entity_count;

    char wifi_connected_ssid[MAX_WIFI_SSID_LEN];
    char wifi_ip_address[MAX_WIFI_IP_LEN];
    int16_t wifi_rssi = -127;
    bool wifi_connected = false;
    bool wifi_scan_in_progress = false;
    bool wifi_connecting = false;
    char wifi_connect_error[MAX_WIFI_ERROR_LEN];
    bool wifi_custom_profile_active = false;
    char wifi_profile_ssid[MAX_WIFI_SSID_LEN];
    WifiNetwork wifi_networks[MAX_WIFI_NETWORKS];
    uint8_t wifi_network_count = 0;
    uint8_t wifi_list_page = 0;
    char wifi_target_ssid[MAX_WIFI_SSID_LEN];
    char wifi_password_input[MAX_WIFI_PASSWORD_LEN + 1];
    bool wifi_password_symbols = false;
    bool wifi_password_shift = false;

    uint32_t last_interaction_ms = 0;
    bool standby_active = false;
    uint32_t standby_last_refresh_ms = 0;
    bool standby_data_dirty = false;
    uint32_t standby_revision = 0;
    StandbySnapshot standby = {};

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
bool store_open_settings(EntityStore* store);
bool store_open_wifi_settings(EntityStore* store);
bool store_open_wifi_password(EntityStore* store, const char* ssid);
bool store_open_standby(EntityStore* store, uint32_t now_ms);
bool store_settings_back(EntityStore* store);
bool store_close_settings(EntityStore* store);
bool store_shift_wifi_list_page(EntityStore* store, int8_t delta);
bool store_set_wifi_password_symbols(EntityStore* store, bool symbols);
bool store_toggle_wifi_password_shift(EntityStore* store);
bool store_append_wifi_password_char(EntityStore* store, char ch);
bool store_backspace_wifi_password(EntityStore* store);
bool store_clear_wifi_password(EntityStore* store);
void store_set_wifi_connection_info(EntityStore* store, bool connected, const char* ssid, const char* ip_address, int16_t rssi);
void store_set_wifi_scan_state(EntityStore* store, bool in_progress);
void store_set_wifi_scan_results(EntityStore* store, const WifiNetwork* networks, uint8_t count);
void store_set_wifi_connecting(EntityStore* store, bool connecting);
void store_set_wifi_connect_error(EntityStore* store, const char* error);
void store_set_wifi_profile(EntityStore* store, const char* ssid, bool custom_profile_active);
void store_get_wifi_settings_snapshot(EntityStore* store, WifiSettingsSnapshot* snapshot);
bool store_get_wifi_password_snapshot(EntityStore* store, WifiPasswordSnapshot* snapshot);
void store_note_interaction(EntityStore* store, uint32_t now_ms);
void store_poll_standby_timeout(EntityStore* store, uint32_t now_ms);
void store_set_standby_weather(EntityStore* store, const char* condition, bool has_temperature, float temperature_c);
void store_set_standby_forecast(EntityStore* store, const StandbyForecastDay* days, uint8_t day_count);
void store_set_standby_energy_metric(EntityStore* store, StandbyEnergyMetric metric, bool valid, float value);
void store_get_standby_snapshot(EntityStore* store, StandbySnapshot* snapshot);
bool store_is_standby_active(EntityStore* store);
void store_update_ui_state(EntityStore* store, const Screen* screen, UIState* ui_state);
void store_bump_rooms_revision(EntityStore* store);
void store_wait_for_wifi_up(EntityStore* store);
void store_flush_pending_commands(EntityStore* store);
EntityRef store_add_entity(EntityStore* store, EntityConfig entity);
