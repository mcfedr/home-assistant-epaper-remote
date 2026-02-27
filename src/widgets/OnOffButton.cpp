#include "widgets/OnOffButton.h"
#include "assets/Montserrat_Regular_26.h"
#include "assets/icons.h"
#include "constants.h"
#include <FastEPD.h>
#include <cstring>

static void set_label_font(FASTEPD* display, uint8_t font_idx) {
    switch (font_idx) {
    case 0:
        display->setFont(Montserrat_Regular_26);
        break;
    case 1:
        display->setFont(FONT_16x16);
        break;
    case 2:
        display->setFont(FONT_12x16);
        break;
    default:
        display->setFont(FONT_8x8);
        break;
    }
}

static BB_RECT get_text_box(FASTEPD* display, const char* text) {
    BB_RECT rect = {};
    display->getStringBox(text, &rect);
    return rect;
}

static void copy_text(char* dst, size_t dst_len, const char* src) {
    if (dst_len == 0) {
        return;
    }
    strncpy(dst, src ? src : "", dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static void truncate_with_ellipsis(FASTEPD* display, char* text, size_t text_len, int16_t max_w) {
    if (text[0] == '\0' || max_w <= 0) {
        text[0] = '\0';
        return;
    }

    BB_RECT rect = get_text_box(display, text);
    if (rect.w <= max_w) {
        return;
    }

    char candidate[MAX_ENTITY_NAME_LEN];
    size_t len = strlen(text);
    while (len > 0) {
        len--;
        size_t keep = len;
        if (keep > sizeof(candidate) - 4) {
            keep = sizeof(candidate) - 4;
        }
        memcpy(candidate, text, keep);
        candidate[keep] = '.';
        candidate[keep + 1] = '.';
        candidate[keep + 2] = '.';
        candidate[keep + 3] = '\0';

        rect = get_text_box(display, candidate);
        if (rect.w <= max_w) {
            copy_text(text, text_len, candidate);
            return;
        }
    }

    copy_text(text, text_len, "...");
    rect = get_text_box(display, text);
    if (rect.w > max_w) {
        text[0] = '\0';
    }
}

OnOffButton::OnOffButton(const char* label, const uint8_t* on_icon, const uint8_t* off_icon, Rect rect)
    : rect_(rect) {
    strncpy(label_, label ? label : "", sizeof(label_) - 1);
    label_[sizeof(label_) - 1] = '\0';

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

    // Fit the light label in the remaining row width.
    const int16_t text_x = rect_.x + BUTTON_SIZE + 30;
    const int16_t max_w = static_cast<int16_t>(display->width()) - text_x - 8;
    char draw_label[MAX_ENTITY_NAME_LEN];
    copy_text(draw_label, sizeof(draw_label), label_);

    uint8_t font_idx = 3;
    for (uint8_t idx = 0; idx <= 3; idx++) {
        set_label_font(display, idx);
        BB_RECT text_rect = get_text_box(display, draw_label);
        if (text_rect.w <= max_w) {
            font_idx = idx;
            break;
        }
    }

    set_label_font(display, font_idx);
    truncate_with_ellipsis(display, draw_label, sizeof(draw_label), max_w);
    display->setTextColor(BBEP_BLACK);
    BB_RECT line_rect = get_text_box(display, "pI");
    display->setCursor(text_x, rect_.y + (BUTTON_SIZE + line_rect.h) / 2 - 5);
    display->write(draw_label);
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
