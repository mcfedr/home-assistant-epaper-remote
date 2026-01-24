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
constexpr uint16_t HASS_MAX_JSON_BUFFER = 1024 * 20; // 20k, home assistant talks a lot
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
constexpr size_t MAX_ENTITIES = 8;
constexpr size_t MAX_WIDGETS_PER_SCREEN = 8;
constexpr uint32_t TOUCH_RELEASE_TIMEOUT_MS = 50;
constexpr uint32_t DISPLAY_FULL_REDRAW_TIMEOUT_MS = 5000;
