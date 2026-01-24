#include "widgets/Slider.h"
#include "assets/Montserrat_Regular_26.h"
#include "assets/icons.h"
#include "constants.h"
#include <FastEPD.h>

FASTEPD
sprite_left_empty_4bpp, sprite_left_full_4bpp, sprite_right_empty_4bpp, sprite_right_full_4bpp, sprite_left_empty_1bpp,
    sprite_left_full_1bpp, sprite_right_empty_1bpp, sprite_right_full_1bpp;

void initialize_slider_sprites() {
    sprite_right_empty_4bpp.initSprite(BUTTON_SIZE / 2, BUTTON_SIZE);
    sprite_right_empty_4bpp.setMode(BB_MODE_4BPP);
    sprite_right_empty_4bpp.fillRect(0, 0, BUTTON_SIZE / 2, BUTTON_SIZE, 0xf);
    sprite_right_empty_4bpp.fillCircle(-1, BUTTON_SIZE / 2, BUTTON_SIZE / 2, BBEP_BLACK);
    sprite_right_empty_4bpp.fillCircle(-1, BUTTON_SIZE / 2, BUTTON_SIZE / 2 - BUTTON_BORDER_SIZE, 0xf);

    sprite_right_full_4bpp.initSprite(BUTTON_SIZE / 2, BUTTON_SIZE);
    sprite_right_full_4bpp.setMode(BB_MODE_4BPP);
    sprite_right_full_4bpp.fillRect(0, 0, BUTTON_SIZE / 2, BUTTON_SIZE, 0xf);
    sprite_right_full_4bpp.fillCircle(-1, BUTTON_SIZE / 2, BUTTON_SIZE / 2, BBEP_BLACK);

    sprite_left_empty_4bpp.initSprite(BUTTON_SIZE / 2, BUTTON_SIZE);
    sprite_left_empty_4bpp.setMode(BB_MODE_4BPP);
    sprite_left_empty_4bpp.fillRect(0, 0, BUTTON_SIZE / 2, BUTTON_SIZE, 0xf);
    sprite_left_empty_4bpp.fillCircle(BUTTON_SIZE / 2, BUTTON_SIZE / 2, BUTTON_SIZE / 2, BBEP_BLACK);
    sprite_left_empty_4bpp.fillCircle(BUTTON_SIZE / 2, BUTTON_SIZE / 2, BUTTON_SIZE / 2 - BUTTON_BORDER_SIZE, 0xf);

    sprite_left_full_4bpp.initSprite(BUTTON_SIZE / 2, BUTTON_SIZE);
    sprite_left_full_4bpp.setMode(BB_MODE_4BPP);
    sprite_left_full_4bpp.fillRect(0, 0, BUTTON_SIZE / 2, BUTTON_SIZE, 0xf);
    sprite_left_full_4bpp.fillCircle(BUTTON_SIZE / 2, BUTTON_SIZE / 2, BUTTON_SIZE / 2, BBEP_BLACK);

    sprite_right_empty_1bpp.initSprite(BUTTON_SIZE / 2, BUTTON_SIZE);
    sprite_right_empty_1bpp.setMode(BB_MODE_1BPP);
    sprite_right_empty_1bpp.fillRect(0, 0, BUTTON_SIZE / 2, BUTTON_SIZE, BBEP_WHITE);
    sprite_right_empty_1bpp.fillCircle(-1, BUTTON_SIZE / 2, BUTTON_SIZE / 2, BBEP_BLACK);
    sprite_right_empty_1bpp.fillCircle(-1, BUTTON_SIZE / 2, BUTTON_SIZE / 2 - BUTTON_BORDER_SIZE, BBEP_WHITE);

    sprite_right_full_1bpp.initSprite(BUTTON_SIZE / 2, BUTTON_SIZE);
    sprite_right_full_1bpp.setMode(BB_MODE_1BPP);
    sprite_right_full_1bpp.fillRect(0, 0, BUTTON_SIZE / 2, BUTTON_SIZE, BBEP_WHITE);
    sprite_right_full_1bpp.fillCircle(-1, BUTTON_SIZE / 2, BUTTON_SIZE / 2, BBEP_BLACK);

    sprite_left_empty_1bpp.initSprite(BUTTON_SIZE / 2, BUTTON_SIZE);
    sprite_left_empty_1bpp.setMode(BB_MODE_1BPP);
    sprite_left_empty_1bpp.fillRect(0, 0, BUTTON_SIZE / 2, BUTTON_SIZE, BBEP_WHITE);
    sprite_left_empty_1bpp.fillCircle(BUTTON_SIZE / 2, BUTTON_SIZE / 2, BUTTON_SIZE / 2, BBEP_BLACK);
    sprite_left_empty_1bpp.fillCircle(BUTTON_SIZE / 2, BUTTON_SIZE / 2, BUTTON_SIZE / 2 - BUTTON_BORDER_SIZE, BBEP_WHITE);

    sprite_left_full_1bpp.initSprite(BUTTON_SIZE / 2, BUTTON_SIZE);
    sprite_left_full_1bpp.setMode(BB_MODE_1BPP);
    sprite_left_full_1bpp.fillRect(0, 0, BUTTON_SIZE / 2, BUTTON_SIZE, BBEP_WHITE);
    sprite_left_full_1bpp.fillCircle(BUTTON_SIZE / 2, BUTTON_SIZE / 2, BUTTON_SIZE / 2, BBEP_BLACK);
}

Slider::Slider(const char* label, const uint8_t* on_icon, const uint8_t* off_icon, Rect rect)
    : label_(label)
    , rect_(rect) {
    on_sprite_4bpp.initSprite(BUTTON_ICON_SIZE, BUTTON_ICON_SIZE);
    on_sprite_4bpp.setMode(BB_MODE_4BPP);
    on_sprite_4bpp.loadBMP(on_icon, 0, 0, BBEP_BLACK, 0xf);
    off_sprite_4bpp.initSprite(BUTTON_ICON_SIZE, BUTTON_ICON_SIZE);
    off_sprite_4bpp.setMode(BB_MODE_4BPP);
    off_sprite_4bpp.loadBMP(off_icon, 0, 0, 0xf, BBEP_BLACK);

    on_sprite_1bpp.initSprite(BUTTON_ICON_SIZE, BUTTON_ICON_SIZE);
    on_sprite_1bpp.setMode(BB_MODE_1BPP);
    on_sprite_1bpp.loadBMP(on_icon, 0, 0, BBEP_BLACK, BBEP_WHITE);
    off_sprite_1bpp.initSprite(BUTTON_ICON_SIZE, BUTTON_ICON_SIZE);
    off_sprite_1bpp.setMode(BB_MODE_1BPP);
    off_sprite_1bpp.loadBMP(off_icon, 0, 0, BBEP_WHITE, BBEP_BLACK);

    // Compute the hitbox
    const int button_y = static_cast<int>(rect_.y) + static_cast<int>(rect_.h) - BUTTON_SIZE;
    const int x_min = static_cast<int>(rect_.x) - TOUCH_AREA_MARGIN;
    const int y_min = button_y - TOUCH_AREA_MARGIN;
    const uint16_t hitbox_width = rect_.w + 2 * TOUCH_AREA_MARGIN;
    const uint16_t hitbox_height = BUTTON_SIZE + 2 * TOUCH_AREA_MARGIN;

    hit_rect_ =
        Rect{static_cast<uint16_t>(x_min < 0 ? 0 : x_min), static_cast<uint16_t>(y_min < 0 ? 0 : y_min), hitbox_width, hitbox_height};
}

Rect Slider::partialDraw(FASTEPD* display, BitDepth depth, uint8_t from, uint8_t to) {
    uint8_t white;
    FASTEPD* sprite_left_full;
    FASTEPD* sprite_left_empty;
    FASTEPD* sprite_right_full;
    FASTEPD* sprite_right_empty;
    FASTEPD* on_sprite;
    FASTEPD* off_sprite;
    if (depth == BitDepth::BD_4BPP) {
        white = 0xf;
        sprite_left_full = &sprite_left_full_4bpp;
        sprite_left_empty = &sprite_left_empty_4bpp;
        sprite_right_full = &sprite_right_full_4bpp;
        sprite_right_empty = &sprite_right_empty_4bpp;
        on_sprite = &on_sprite_4bpp;
        off_sprite = &off_sprite_4bpp;
    } else {
        white = BBEP_WHITE;
        sprite_left_full = &sprite_left_full_1bpp;
        sprite_left_empty = &sprite_left_empty_1bpp;
        sprite_right_full = &sprite_right_full_1bpp;
        sprite_right_empty = &sprite_right_empty_1bpp;
        on_sprite = &on_sprite_1bpp;
        off_sprite = &off_sprite_1bpp;
    }

    // Normalize display values between 0 and width - BUTTON_SIZE / 2
    uint16_t previous_value_x = 0;
    uint16_t value_x = 0;
    uint16_t slider_width = rect_.w - SLIDER_OFFSET - BUTTON_SIZE / 2;
    if (from > 0) {
        previous_value_x = SLIDER_OFFSET + (from * slider_width) / 100;
    }
    if (to > 0) {
        value_x = SLIDER_OFFSET + (to * slider_width) / 100;
    }

    uint16_t y = rect_.y + rect_.h - BUTTON_SIZE;
    uint32_t t1 = micros();

    // Reset if we're going down
    // Note: this can erase the right edge, this must be done before drawing it
    if (value_x < previous_value_x) {
        display->fillRect(rect_.x + value_x, y + BUTTON_BORDER_SIZE, previous_value_x - value_x + BUTTON_SIZE / 2,
                          BUTTON_SIZE - 2 * BUTTON_BORDER_SIZE, white);
    }

    // Reset the left edge if needed
    if (previous_value_x == 0 && value_x > 0) {
        display->drawSprite(sprite_left_full, rect_.x, y);
    } else if (value_x == 0 && previous_value_x > 0) {
        display->drawSprite(sprite_left_empty, rect_.x, y);
    }

    // Reset the right edge if needed
    if (value_x >= rect_.w - BUTTON_SIZE || previous_value_x >= rect_.w - BUTTON_SIZE) {
        display->drawSprite(sprite_right_empty, rect_.x + rect_.w - BUTTON_SIZE / 2, y);
    }

    // Fill if we're going up
    if (value_x > previous_value_x) {
        if (previous_value_x == 0) {
            display->fillRect(rect_.x + BUTTON_SIZE / 2, y, value_x - BUTTON_SIZE / 2, BUTTON_SIZE, BBEP_BLACK);
        } else {
            display->fillRect(rect_.x + previous_value_x, y, value_x - previous_value_x, BUTTON_SIZE, BBEP_BLACK);
        }
    }

    // Draw the position circle
    if (value_x > 0) {
        display->fillCircle( // transparent sprite not implemented ?
            rect_.x + value_x, y + BUTTON_SIZE / 2, BUTTON_SIZE / 2 - 1, BBEP_BLACK);
    }

    // Re-draw the image if needed
    if (value_x < (BUTTON_SIZE / 2 + BUTTON_SIZE) || previous_value_x < BUTTON_SIZE) {
        if (value_x > 0) {
            display->drawSprite(on_sprite, rect_.x + (BUTTON_SIZE - BUTTON_ICON_SIZE) / 2, y + (BUTTON_SIZE - BUTTON_ICON_SIZE) / 2);
        } else {
            display->drawSprite(off_sprite, rect_.x + (BUTTON_SIZE - BUTTON_ICON_SIZE) / 2, y + (BUTTON_SIZE - BUTTON_ICON_SIZE) / 2);
        }
    }

    // Return the calculated damage
    const uint16_t x0 = std::min(previous_value_x, value_x);
    const uint16_t x1 = std::max(previous_value_x, value_x);

    return Rect{static_cast<uint16_t>(rect_.x + x0), y, static_cast<uint16_t>(x1 - x0 + BUTTON_SIZE / 2), BUTTON_SIZE};
}

void Slider::fullDraw(FASTEPD* display, BitDepth depth, uint8_t value) {
    // We need a black background to handle the borders of the slider
    display->fillRect(rect_.x, rect_.y + rect_.h - BUTTON_SIZE, rect_.w, BUTTON_SIZE, BBEP_BLACK);

    // We don't really care about performance here
    partialDraw(display, depth, 100, 0);
    if (value > 0) {
        partialDraw(display, depth, 0, value);
    }

    // Add the title
    BB_RECT rect;
    display->setFont(Montserrat_Regular_26);
    display->setTextColor(BBEP_BLACK);
    display->getStringBox("pI", &rect); // FIXME How to get actual font height ?
    // display->getStringBox(label_, &rect);
    display->setCursor(rect_.x, rect_.y + rect.h);
    display->write(label_);
}

bool Slider::isTouching(const TouchEvent* touch_event) const {
    return touch_event->x >= hit_rect_.x && touch_event->x < hit_rect_.x + hit_rect_.w && touch_event->y >= hit_rect_.y &&
           touch_event->y < hit_rect_.y + hit_rect_.h;
}

uint8_t Slider::getValueFromTouch(const TouchEvent* touch_event, uint8_t original_value) const {
    const int touch_x = static_cast<int>(touch_event->x);

    // Slider geometry
    const int slider_start = static_cast<int>(rect_.x) + SLIDER_OFFSET;
    const int slider_end = static_cast<int>(rect_.x) + static_cast<int>(rect_.w) - BUTTON_SIZE / 2;

    // Clamp touch to slider range
    const int clamped_x = std::min(slider_end, std::max(slider_start, touch_x));

    // Normalize to 0–100
    const int value = (100 * (clamped_x - slider_start)) / (slider_end - slider_start);

    return static_cast<uint8_t>(value);
}