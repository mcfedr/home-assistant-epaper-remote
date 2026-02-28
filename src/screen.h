#pragma once

#include "constants.h"
#include "entity_ref.h"
#include "widgets/Widget.h"
#include <cstddef>

struct Screen {
    size_t widget_count;
    Widget* widgets[MAX_WIDGETS_PER_SCREEN];
    uint8_t entity_ids[MAX_WIDGETS_PER_SCREEN];
};

struct SliderConfig {
    EntityRef entity_ref;
    const char* label;
    const uint8_t* icon_on;
    const uint8_t* icon_off;
    uint16_t pos_x;
    uint16_t pos_y;
    uint16_t width;
    uint16_t height;
};

struct ButtonConfig {
    EntityRef entity_ref;
    const char* label;
    const uint8_t* icon_on;
    const uint8_t* icon_off;
    uint16_t pos_x;
    uint16_t pos_y;
    uint16_t width;
    uint16_t height;
};

struct ClimateConfig {
    EntityRef entity_ref;
    const char* label;
    uint8_t climate_mode_mask;
    uint16_t pos_x;
    uint16_t pos_y;
    uint16_t width;
    uint16_t height;
};

struct CoverConfig {
    EntityRef entity_ref;
    const char* label;
    uint16_t pos_x;
    uint16_t pos_y;
    uint16_t width;
    uint16_t height;
};

void screen_add_slider(SliderConfig config, Screen* screen);
void screen_add_button(ButtonConfig config, Screen* screen);
void screen_add_climate(ClimateConfig config, Screen* screen);
void screen_add_cover(CoverConfig config, Screen* screen);
void screen_clear(Screen* screen);
