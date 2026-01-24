#pragma once

#include "widgets/Widget.h"
#include <FastEPD.h>

class OnOffButton : public Widget {
public:
    OnOffButton(const char* label, const uint8_t* on_icon, const uint8_t* off_icon, Rect rect);

    void fullDraw(FASTEPD* display, BitDepth depth, uint8_t value) override;
    Rect partialDraw(FASTEPD* display, BitDepth depth, uint8_t from, uint8_t to) override;
    bool isTouching(const TouchEvent* touch_event) const override;
    uint8_t getValueFromTouch(const TouchEvent* touch_event, uint8_t original_value) const override;

private:
    const char* label_;
    FASTEPD off_sprite_4bpp;
    FASTEPD on_sprite_4bpp;
    FASTEPD off_sprite_1bpp;
    FASTEPD on_sprite_1bpp;
    Rect rect_;
    Rect hit_rect_;
};