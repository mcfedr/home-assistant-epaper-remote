#include "screen.h"
#include "esp_system.h"
#include "widgets/ClimateWidget.h"
#include "widgets/OnOffButton.h"
#include "widgets/Slider.h"

void screen_add_slider(SliderConfig config, Screen* screen) {
    if (screen->widget_count >= MAX_WIDGETS_PER_SCREEN) {
        esp_system_abort("too many widgets configured");
    }

    Rect rect{
        .x = (int16_t)config.pos_x,
        .y = (int16_t)config.pos_y,
        .w = (int16_t)config.width,
        .h = (int16_t)config.height,
    };

    Slider* widget = new (std::nothrow) Slider(config.label, config.icon_on, config.icon_off, rect);
    if (!widget) {
        esp_system_abort("out of memory");
    }

    const uint16_t widget_idx = screen->widget_count++;
    screen->widgets[widget_idx] = widget;
    screen->entity_ids[widget_idx] = config.entity_ref.index;
}

void screen_add_button(ButtonConfig config, Screen* screen) {
    if (screen->widget_count >= MAX_WIDGETS_PER_SCREEN) {
        esp_system_abort("too many widgets configured");
    }

    Rect rect{
        .x = (int16_t)config.pos_x,
        .y = (int16_t)config.pos_y,
        .w = (int16_t)config.width,
        .h = (int16_t)config.height,
    };

    OnOffButton* widget = new (std::nothrow) OnOffButton(config.label, config.icon_on, config.icon_off, rect);
    if (!widget) {
        esp_system_abort("out of memory");
    }

    const uint16_t widget_idx = screen->widget_count++;
    screen->widgets[widget_idx] = widget;
    screen->entity_ids[widget_idx] = config.entity_ref.index;
}

void screen_add_climate(ClimateConfig config, Screen* screen) {
    if (screen->widget_count >= MAX_WIDGETS_PER_SCREEN) {
        esp_system_abort("too many widgets configured");
    }

    Rect rect{
        .x = (int16_t)config.pos_x,
        .y = (int16_t)config.pos_y,
        .w = (int16_t)config.width,
        .h = (int16_t)config.height,
    };

    ClimateWidget* widget = new (std::nothrow) ClimateWidget(config.label, rect, config.climate_mode_mask);
    if (!widget) {
        esp_system_abort("out of memory");
    }

    const uint16_t widget_idx = screen->widget_count++;
    screen->widgets[widget_idx] = widget;
    screen->entity_ids[widget_idx] = config.entity_ref.index;
}

void screen_clear(Screen* screen) {
    for (size_t idx = 0; idx < screen->widget_count; idx++) {
        delete screen->widgets[idx];
        screen->widgets[idx] = nullptr;
        screen->entity_ids[idx] = 0;
    }
    screen->widget_count = 0;
}
