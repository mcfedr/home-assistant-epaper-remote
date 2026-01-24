#pragma once

#include <FastEPD.h>
#include <cstdint>

struct Rect {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
};

struct TouchEvent {
    uint16_t x;
    uint16_t y;
};

enum class BitDepth : uint8_t {
    BD_1BPP,
    BD_4BPP,
};

class Widget {
public:
    virtual ~Widget() = default;
    virtual void fullDraw(FASTEPD* display, BitDepth depth, uint8_t value) = 0;
    virtual Rect partialDraw(FASTEPD* display, BitDepth depth, uint8_t from, uint8_t to) = 0;
    virtual bool isTouching(const TouchEvent* touch_event) const = 0;
    virtual uint8_t getValueFromTouch(const TouchEvent* touch_event, uint8_t original_value) const = 0;
};