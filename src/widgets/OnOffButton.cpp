#include "widgets/OnOffButton.h"
#include "assets/Montserrat_Regular_26.h"
#include "assets/icons.h"
#include "constants.h"
#include <FastEPD.h>

OnOffButton::OnOffButton(const char* label, const uint8_t* on_icon, const uint8_t* off_icon, Rect rect)
    : label_(label)
    , rect_(rect) {
    uint8_t icon_pos = (BUTTON_SIZE - BUTTON_ICON_SIZE) / 2;
    on_sprite_4bpp.initSprite(BUTTON_SIZE + 2, BUTTON_SIZE + 2);
    on_sprite_4bpp.setMode(BB_MODE_4BPP);
    on_sprite_4bpp.fillScreen(0xf);
    on_sprite_4bpp.fillCircle(BUTTON_SIZE / 2, BUTTON_SIZE / 2, BUTTON_SIZE / 2, BBEP_BLACK);
    on_sprite_4bpp.loadBMP(on_icon, icon_pos, icon_pos, BBEP_BLACK, 0xf);

    off_sprite_4bpp.initSprite(BUTTON_SIZE + 2, BUTTON_SIZE + 2);
    off_sprite_4bpp.setMode(BB_MODE_4BPP);
    off_sprite_4bpp.fillScreen(0xf);
    off_sprite_4bpp.fillCircle(BUTTON_SIZE / 2, BUTTON_SIZE / 2, BUTTON_SIZE / 2, BBEP_BLACK);
    off_sprite_4bpp.fillCircle(BUTTON_SIZE / 2, BUTTON_SIZE / 2, BUTTON_SIZE / 2 - BUTTON_BORDER_SIZE, 0xf);
    off_sprite_4bpp.loadBMP(off_icon, icon_pos, icon_pos, 0xf, BBEP_BLACK);

    on_sprite_1bpp.initSprite(BUTTON_SIZE + 2, BUTTON_SIZE + 2);
    on_sprite_1bpp.setMode(BB_MODE_1BPP);
    on_sprite_1bpp.fillScreen(BBEP_WHITE);
    on_sprite_1bpp.fillCircle(BUTTON_SIZE / 2, BUTTON_SIZE / 2, BUTTON_SIZE / 2, BBEP_BLACK);
    on_sprite_1bpp.loadBMP(on_icon, icon_pos, icon_pos, BBEP_BLACK, BBEP_WHITE);

    off_sprite_1bpp.initSprite(BUTTON_SIZE + 2, BUTTON_SIZE + 2);
    off_sprite_1bpp.setMode(BB_MODE_1BPP);
    off_sprite_1bpp.fillScreen(BBEP_WHITE);
    off_sprite_1bpp.fillCircle(BUTTON_SIZE / 2, BUTTON_SIZE / 2, BUTTON_SIZE / 2, BBEP_BLACK);
    off_sprite_1bpp.fillCircle(BUTTON_SIZE / 2, BUTTON_SIZE / 2, BUTTON_SIZE / 2 - BUTTON_BORDER_SIZE, BBEP_WHITE);
    off_sprite_1bpp.loadBMP(off_icon, icon_pos, icon_pos, BBEP_WHITE, BBEP_BLACK);

    // Compute the hit box
    const int x_min = static_cast<int>(rect_.x) - TOUCH_AREA_MARGIN;
    const int y_min = static_cast<int>(rect_.y) - TOUCH_AREA_MARGIN;
    hit_rect_ =
        Rect{static_cast<uint16_t>(x_min < 0 ? 0 : x_min), static_cast<uint16_t>(y_min < 0 ? 0 : y_min),
             static_cast<uint16_t>(BUTTON_SIZE + 2 * TOUCH_AREA_MARGIN), static_cast<uint16_t>(BUTTON_SIZE + 2 * TOUCH_AREA_MARGIN)};
}

Rect OnOffButton::partialDraw(FASTEPD* display, BitDepth depth, uint8_t from, uint8_t to) {
    if (to) {
        if (depth == BitDepth::BD_4BPP) {
            display->drawSprite(&on_sprite_4bpp, rect_.x, rect_.y);
        } else {
            display->drawSprite(&on_sprite_1bpp, rect_.x, rect_.y);
        }
    } else {
        if (depth == BitDepth::BD_4BPP) {
            display->drawSprite(&off_sprite_4bpp, rect_.x, rect_.y);
        } else {
            display->drawSprite(&off_sprite_1bpp, rect_.x, rect_.y);
        }
    }

    return Rect{rect_.x, rect_.y, BUTTON_SIZE + 1, BUTTON_SIZE + 1};
}

void OnOffButton::fullDraw(FASTEPD* display, BitDepth depth, uint8_t value) {
    partialDraw(display, depth, 0, value);

    // Add the title
    BB_RECT rect;
    display->setFont(Montserrat_Regular_26);
    display->setTextColor(BBEP_BLACK);
    display->getStringBox("pI", &rect); // FIXME How to get actual font height ?
    // display->getStringBox(label_, &rect);
    display->setCursor(rect_.x + BUTTON_SIZE + 30, rect_.y + (BUTTON_SIZE + rect.h) / 2 - 5);
    display->write(label_);
}

bool OnOffButton::isTouching(const TouchEvent* touch_event) const {
    return touch_event->x >= hit_rect_.x && touch_event->x < hit_rect_.x + hit_rect_.w && touch_event->y >= hit_rect_.y &&
           touch_event->y < hit_rect_.y + hit_rect_.h;
}

uint8_t OnOffButton::getValueFromTouch(const TouchEvent* touch_event, uint8_t original_value) const {
    if (!isTouching(touch_event)) {
        return original_value;
    }

    return original_value ? 0 : 1;
}