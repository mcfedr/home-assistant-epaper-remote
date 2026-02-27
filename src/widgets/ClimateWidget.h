#pragma once

#include "climate_value.h"
#include "constants.h"
#include "widgets/Widget.h"
#include <FastEPD.h>

class ClimateWidget : public Widget {
public:
    ClimateWidget(const char* label, Rect rect, uint8_t climate_mode_mask);

    void fullDraw(FASTEPD* display, BitDepth depth, uint8_t value) override;
    Rect partialDraw(FASTEPD* display, BitDepth depth, uint8_t from, uint8_t to) override;
    bool isTouching(const TouchEvent* touch_event) const override;
    uint8_t getValueFromTouch(const TouchEvent* touch_event, uint8_t original_value) const override;

private:
    char label_[MAX_ENTITY_NAME_LEN];
    Rect rect_;
    Rect hit_rect_;
    Rect label_rect_;
    ClimateMode mode_buttons_[3];
    uint8_t mode_button_count_;
    Rect mode_rects_[3];
    Rect minus_rect_;
    Rect plus_rect_;
    Rect temp_adjust_value_rect_;
};
