#pragma once

#include <cstddef>
#include <cstdint>

// Buttons configuration
constexpr uint8_t BUTTON_BORDER_SIZE = 4;
constexpr uint8_t BUTTON_SIZE = 100;
constexpr uint8_t BUTTON_ICON_SIZE = 64;
constexpr uint8_t SLIDER_OFFSET = 100;    // The zero is a bit on the right
constexpr uint8_t TOUCH_AREA_MARGIN = 15; // A touch within 15px of the target is OK

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
constexpr size_t MAX_FLOOR_NAME_LEN = 40;
constexpr size_t MAX_ROOM_NAME_LEN = 40;
constexpr uint32_t TOUCH_RELEASE_TIMEOUT_MS = 50;
constexpr uint32_t DISPLAY_FULL_REDRAW_TIMEOUT_MS = 5000;

// Floor/room list UI geometry
constexpr uint16_t ROOM_LIST_TITLE_Y = 40;
constexpr uint8_t ROOM_LIST_COLUMNS = 2;
constexpr uint8_t ROOM_LIST_ROWS = 4;
constexpr uint8_t ROOM_LIST_ROOMS_PER_PAGE = ROOM_LIST_COLUMNS * ROOM_LIST_ROWS;
constexpr uint16_t FLOOR_LIST_GRID_START_Y = 48;
constexpr uint16_t ROOM_LIST_HEADER_HEIGHT = 100;
constexpr uint16_t ROOM_LIST_GRID_START_Y = ROOM_LIST_HEADER_HEIGHT + 12;
constexpr uint16_t ROOM_LIST_GRID_BOTTOM_Y = 860;
constexpr uint16_t ROOM_LIST_GRID_MARGIN_X = 20;
constexpr uint16_t ROOM_LIST_GRID_GAP_X = 16;
constexpr uint16_t ROOM_LIST_GRID_GAP_Y = 16;
constexpr uint16_t ROOM_LIST_FOOTER_Y = 920;
constexpr uint16_t ROOM_LIST_SWIPE_THRESHOLD_X = 80;

// Room controls UI geometry
constexpr uint16_t ROOM_CONTROLS_HEADER_HEIGHT = 110;
constexpr uint16_t ROOM_CONTROLS_ITEM_START_Y = 130;
constexpr uint16_t ROOM_CONTROLS_ITEM_SPACING = 120;
constexpr uint16_t ROOM_CONTROLS_ITEM_X = 30;
constexpr uint16_t ROOM_CONTROLS_BACK_X = 20;
constexpr uint16_t ROOM_CONTROLS_BACK_Y = 25;
constexpr uint16_t ROOM_CONTROLS_BACK_W = 120;
constexpr uint16_t ROOM_CONTROLS_BACK_H = 60;
