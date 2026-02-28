#pragma once

#include "widgets/Widget.h"
#include "constants.h"

class CoverWidget : public Widget {
public:
    CoverWidget(const char* label, Rect rect);
    Rect partialDraw(FASTEPD* display, BitDepth depth, uint8_t from, uint8_t to) override;
    void fullDraw(FASTEPD* display, BitDepth depth, uint8_t value) override;
    bool isTouching(const TouchEvent* touch_event) const override;
    uint8_t getValueFromTouch(const TouchEvent* touch_event, uint8_t original_value) const override;

private:
    char label_[MAX_ENTITY_NAME_LEN];
    Rect rect_;
    Rect hit_rect_;
    Rect label_rect_;
    Rect up_rect_;
    Rect down_rect_;
};
