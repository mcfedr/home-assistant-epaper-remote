#pragma once

#include "boards.h"
#include <cstddef>
#include <cstdint>

// Buttons configuration
constexpr uint8_t BUTTON_BORDER_SIZE = 4;
constexpr uint8_t BUTTON_SIZE = 100;
constexpr uint8_t BUTTON_ICON_SIZE = 64;
constexpr uint8_t SLIDER_OFFSET = 100;    // The zero is a bit on the right
constexpr uint8_t TOUCH_AREA_MARGIN = 20; // Larger touch target for easier tapping

// Home assistant configuration
constexpr uint32_t HASS_MAX_JSON_BUFFER = 1024 * 256; // 256k, area/entity registries can be large
constexpr uint32_t HASS_RECONNECT_DELAY_MS = 10000;

// When sending commands too fast (on a slider), this can flood
// the zigbee network and make the commands fail. Increase this delay
// if you see errors when using sliders.
constexpr uint32_t HASS_TASK_SEND_DELAY_MS = 500;

// When sending commands, we'll receive the updates from the server
// with a delay. This causes jittering in the slider and unnecessary
// commands sent to the server. We ignore updates from the server
// during this delay after a command was sent on an entity.
// FIXME: We can lose updates, we should have an authoritative value
// and a target value in the store at some point.
constexpr uint32_t HASS_IGNORE_UPDATE_DELAY_MS = 1000;

// Other constants
constexpr size_t MAX_ENTITIES = 128;
constexpr size_t MAX_DEVICE_MAPPINGS = 512;
constexpr size_t MAX_WIDGETS_PER_SCREEN = 16;
constexpr size_t MAX_FLOORS = 16;
constexpr size_t MAX_ROOMS = 32;
constexpr size_t MAX_ENTITY_ID_LEN = 96;
constexpr size_t MAX_ENTITY_NAME_LEN = 40;
constexpr size_t MAX_ICON_NAME_LEN = 64;
constexpr size_t MAX_FLOOR_NAME_LEN = 40;
constexpr size_t MAX_ROOM_NAME_LEN = 40;
constexpr size_t MAX_WIFI_NETWORKS = 24;
constexpr size_t MAX_WIFI_SSID_LEN = 33;
constexpr size_t MAX_WIFI_PASSWORD_LEN = 64;
constexpr size_t MAX_WIFI_IP_LEN = 20;
constexpr size_t MAX_WIFI_ERROR_LEN = 64;
constexpr size_t MAX_STANDBY_CONDITION_LEN = 32;
constexpr size_t MAX_STANDBY_FORECAST_DAYS = 5;
constexpr size_t MAX_STANDBY_DAY_LABEL_LEN = 8;
constexpr uint32_t TOUCH_RELEASE_TIMEOUT_MS = 25;
constexpr uint32_t DISPLAY_FULL_REDRAW_TIMEOUT_MS = 15000;
constexpr uint8_t DISPLAY_PARTIAL_UPDATE_PASSES = 2;
constexpr uint8_t DISPLAY_FULL_UPDATE_PASSES = 4;
constexpr uint32_t STANDBY_IDLE_TIMEOUT_MS = 120000;
constexpr uint32_t STANDBY_REFRESH_INTERVAL_MS = 3600000; // 1 hour

// Floor/room list UI geometry
constexpr uint16_t ROOM_LIST_TITLE_Y = 40;
constexpr uint8_t ROOM_LIST_COLUMNS = 2;
constexpr uint8_t ROOM_LIST_ROWS = 4;
constexpr uint8_t ROOM_LIST_ROOMS_PER_PAGE = ROOM_LIST_COLUMNS * ROOM_LIST_ROWS;
constexpr uint16_t FLOOR_LIST_GRID_START_Y = 120;
constexpr uint16_t ROOM_LIST_HEADER_HEIGHT = 100;
constexpr uint16_t ROOM_LIST_GRID_START_Y = ROOM_LIST_HEADER_HEIGHT + 12;
constexpr uint16_t ROOM_LIST_GRID_BOTTOM_Y = 860;
constexpr uint16_t ROOM_LIST_GRID_MARGIN_X = 20;
constexpr uint16_t ROOM_LIST_GRID_GAP_X = 16;
constexpr uint16_t ROOM_LIST_GRID_GAP_Y = 16;
constexpr uint16_t ROOM_LIST_FOOTER_Y = 920;
constexpr uint16_t ROOM_LIST_SWIPE_THRESHOLD_X = 80;
constexpr uint16_t ROOM_LIST_TILE_ICON_SIZE = 64;
constexpr uint16_t ROOM_LIST_TILE_ICON_TOP_PADDING = 10;
constexpr uint16_t ROOM_LIST_TILE_ICON_LABEL_GAP = 10;
constexpr uint16_t ROOM_LIST_TILE_LABEL_BOTTOM_PADDING = 8;
constexpr uint16_t ROOM_LIST_TILE_RADIUS = 18;
constexpr uint16_t HOME_SETTINGS_BUTTON_X = DISPLAY_WIDTH - 92;
constexpr uint16_t HOME_SETTINGS_BUTTON_Y = 31;
constexpr uint16_t HOME_SETTINGS_BUTTON_W = 64;
constexpr uint16_t HOME_SETTINGS_BUTTON_H = 64;
constexpr uint16_t HOME_SETTINGS_ICON_SIZE = 30;

// Room controls UI geometry
constexpr uint16_t ROOM_CONTROLS_HEADER_HEIGHT = 110;
constexpr uint16_t ROOM_CONTROLS_ITEM_START_Y = 130;
constexpr uint16_t ROOM_CONTROLS_LIGHT_HEIGHT = (BUTTON_SIZE + 50) * 13 / 10;      // +30%
constexpr uint16_t ROOM_CONTROLS_LIGHT_MIN_HEIGHT = 96;                              // Keep enough room for icon + label
constexpr uint16_t ROOM_CONTROLS_CLIMATE_HEIGHT = (BUTTON_SIZE * 2 + 40) * 13 / 10; // +30%
constexpr uint16_t ROOM_CONTROLS_COVER_HEIGHT = (BUTTON_SIZE + 40) * 13 / 10;       // +30%
constexpr uint16_t ROOM_CONTROLS_ITEM_GAP = 20;
constexpr uint16_t ROOM_CONTROLS_LIGHT_COLUMN_GAP = 20;
constexpr uint16_t ROOM_CONTROLS_ITEM_X = 30;
constexpr uint16_t ROOM_CONTROLS_BOTTOM_PADDING = 12;
constexpr uint16_t ROOM_CONTROLS_BACK_X = 20;
constexpr uint16_t ROOM_CONTROLS_BACK_Y = 25;
constexpr uint16_t ROOM_CONTROLS_BACK_W = 120;
constexpr uint16_t ROOM_CONTROLS_BACK_H = 60;

// Settings / Wi-Fi UI geometry
constexpr uint16_t SETTINGS_HEADER_HEIGHT = 100;
constexpr uint16_t SETTINGS_TILE_X = 30;
constexpr uint16_t SETTINGS_TILE_Y = 190;
constexpr uint16_t SETTINGS_TILE_W = DISPLAY_WIDTH - 2 * SETTINGS_TILE_X;
constexpr uint16_t SETTINGS_TILE_H = 170;
constexpr uint16_t SETTINGS_TILE_GAP = 24;
constexpr uint16_t SETTINGS_STANDBY_TILE_X = SETTINGS_TILE_X;
constexpr uint16_t SETTINGS_STANDBY_TILE_Y = SETTINGS_TILE_Y + SETTINGS_TILE_H + SETTINGS_TILE_GAP;
constexpr uint16_t SETTINGS_STANDBY_TILE_W = SETTINGS_TILE_W;
constexpr uint16_t SETTINGS_STANDBY_TILE_H = SETTINGS_TILE_H;

constexpr uint16_t WIFI_INFO_X = 24;
constexpr uint16_t WIFI_INFO_Y = SETTINGS_HEADER_HEIGHT + 18;
constexpr uint16_t WIFI_INFO_W = DISPLAY_WIDTH - 2 * WIFI_INFO_X;
constexpr uint16_t WIFI_INFO_H = 190;
constexpr uint16_t WIFI_ACTIONS_Y = WIFI_INFO_Y + WIFI_INFO_H + 12;
constexpr uint16_t WIFI_SCAN_BUTTON_X = WIFI_INFO_X;
constexpr uint16_t WIFI_SCAN_BUTTON_Y = WIFI_ACTIONS_Y;
constexpr uint16_t WIFI_SCAN_BUTTON_W = 170;
constexpr uint16_t WIFI_SCAN_BUTTON_H = 46;
constexpr uint16_t WIFI_DEFAULT_BUTTON_X = WIFI_SCAN_BUTTON_X + WIFI_SCAN_BUTTON_W + 12;
constexpr uint16_t WIFI_DEFAULT_BUTTON_Y = WIFI_ACTIONS_Y;
constexpr uint16_t WIFI_DEFAULT_BUTTON_W = WIFI_INFO_X + WIFI_INFO_W - WIFI_DEFAULT_BUTTON_X;
constexpr uint16_t WIFI_DEFAULT_BUTTON_H = WIFI_SCAN_BUTTON_H;
constexpr uint16_t WIFI_NETWORK_LIST_X = WIFI_INFO_X;
constexpr uint16_t WIFI_NETWORK_LIST_Y = WIFI_ACTIONS_Y + WIFI_SCAN_BUTTON_H + 14;
constexpr uint16_t WIFI_NETWORK_LIST_W = WIFI_INFO_W;
constexpr uint16_t WIFI_NETWORK_ROW_H = 64;
constexpr uint16_t WIFI_NETWORK_ROW_GAP = 10;
constexpr uint8_t WIFI_NETWORKS_PER_PAGE = 7;
constexpr uint16_t WIFI_NETWORK_PAGE_BADGE_Y = DISPLAY_HEIGHT - 54;

constexpr uint16_t WIFI_PASSWORD_BOX_X = WIFI_INFO_X;
constexpr uint16_t WIFI_PASSWORD_BOX_Y = SETTINGS_HEADER_HEIGHT + 20;
constexpr uint16_t WIFI_PASSWORD_BOX_W = WIFI_INFO_W;
constexpr uint16_t WIFI_PASSWORD_BOX_H = 140;
constexpr uint16_t WIFI_KEYBOARD_X = 18;
constexpr uint16_t WIFI_KEYBOARD_Y = WIFI_PASSWORD_BOX_Y + WIFI_PASSWORD_BOX_H + 16;
constexpr uint16_t WIFI_KEYBOARD_W = DISPLAY_WIDTH - 2 * WIFI_KEYBOARD_X;
constexpr uint16_t WIFI_KEY_H = 56;
constexpr uint16_t WIFI_KEY_GAP = 8;

// Standby UI geometry
constexpr uint16_t STANDBY_MARGIN = 18;
constexpr uint16_t STANDBY_HEADER_Y = 20;
constexpr uint16_t STANDBY_WEATHER_Y = 20;
constexpr uint16_t STANDBY_WEATHER_H = 360;
constexpr uint16_t STANDBY_ENERGY_Y = STANDBY_WEATHER_Y + STANDBY_WEATHER_H + 8;
constexpr uint16_t STANDBY_ENERGY_H = DISPLAY_HEIGHT - STANDBY_ENERGY_Y - 6;
