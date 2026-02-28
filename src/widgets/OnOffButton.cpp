#include "widgets/OnOffButton.h"
#include "assets/Montserrat_Regular_16.h"
#include "assets/Montserrat_Regular_20.h"
#include "assets/Montserrat_Regular_26.h"
#include "assets/icons.h"
#include "constants.h"
#include <FastEPD.h>
#include <algorithm>
#include <cstring>

static void set_label_font(FASTEPD* display, uint8_t font_idx) {
    switch (font_idx) {
    case 0:
        display->setFont(Montserrat_Regular_26);
        break;
    case 1:
        display->setFont(Montserrat_Regular_20);
        break;
    case 2:
        display->setFont(Montserrat_Regular_16);
        break;
    default:
        display->setFont(Montserrat_Regular_16);
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

    if (rect_.w == 0) {
        rect_.w = BUTTON_SIZE;
    }
    if (rect_.h == 0) {
        rect_.h = BUTTON_SIZE;
    }

    constexpr int16_t pad_x = 8;
    int16_t pad_top = rect_.h >= 120 ? 6 : 3;
    int16_t bottom_pad = rect_.h >= 120 ? 6 : 3;
    int16_t label_gap = rect_.h >= 120 ? 8 : 4;
    int16_t label_h = rect_.h >= 150 ? 30 : (rect_.h >= 120 ? 24 : 20);

    int16_t max_icon_h = static_cast<int16_t>(rect_.h) - (pad_top + label_gap + label_h + bottom_pad);
    if (max_icon_h < BUTTON_ICON_SIZE) {
        while (max_icon_h < BUTTON_ICON_SIZE && label_gap > 2) {
            label_gap--;
            max_icon_h++;
        }
        while (max_icon_h < BUTTON_ICON_SIZE && pad_top > 1) {
            pad_top--;
            max_icon_h++;
        }
        while (max_icon_h < BUTTON_ICON_SIZE && bottom_pad > 1) {
            bottom_pad--;
            max_icon_h++;
        }
    }

    const int16_t max_icon_w = std::max<int16_t>(BUTTON_ICON_SIZE, static_cast<int16_t>(rect_.w) - 2 * pad_x);
    max_icon_h = std::max<int16_t>(BUTTON_ICON_SIZE, max_icon_h);
    const int16_t desired_sprite = std::min(max_icon_w, max_icon_h);
    // Keep sprite RAM roughly stable even when widget cards get taller.
    const int16_t sprite_cap = BUTTON_SIZE;
    const int16_t min_sprite = BUTTON_ICON_SIZE;
    const int16_t clamped_sprite = std::max<int16_t>(min_sprite, std::min<int16_t>(desired_sprite, sprite_cap));
    sprite_size_ = static_cast<uint16_t>(clamped_sprite);

    icon_rect_ = Rect{
        .x = static_cast<uint16_t>(rect_.x + (rect_.w - sprite_size_) / 2),
        .y = static_cast<uint16_t>(rect_.y + pad_top + (max_icon_h - sprite_size_) / 2),
        .w = sprite_size_,
        .h = sprite_size_,
    };

    const uint16_t label_y = static_cast<uint16_t>(rect_.y + rect_.h - bottom_pad - label_h);
    label_rect_ = Rect{
        .x = static_cast<uint16_t>(rect_.x + 4),
        .y = label_y,
        .w = static_cast<uint16_t>(rect_.w > 8 ? rect_.w - 8 : rect_.w),
        .h = static_cast<uint16_t>(std::max<int16_t>(16, label_h)),
    };

    const uint16_t icon_center = sprite_size_ / 2;
    const uint16_t icon_radius = sprite_size_ / 2;
    const uint16_t icon_pos = static_cast<uint16_t>((sprite_size_ - BUTTON_ICON_SIZE) / 2);

    on_sprite_4bpp.initSprite(sprite_size_, sprite_size_);
    on_sprite_4bpp.setMode(BB_MODE_4BPP);
    on_sprite_4bpp.fillScreen(0xf);
    on_sprite_4bpp.fillCircle(icon_center, icon_center, icon_radius, BBEP_BLACK);
    on_sprite_4bpp.loadBMP(on_icon, icon_pos, icon_pos, BBEP_BLACK, 0xf);

    off_sprite_4bpp.initSprite(sprite_size_, sprite_size_);
    off_sprite_4bpp.setMode(BB_MODE_4BPP);
    off_sprite_4bpp.fillScreen(0xf);
    off_sprite_4bpp.fillCircle(icon_center, icon_center, icon_radius, BBEP_BLACK);
    off_sprite_4bpp.fillCircle(icon_center, icon_center, icon_radius - BUTTON_BORDER_SIZE, 0xf);
    off_sprite_4bpp.loadBMP(off_icon, icon_pos, icon_pos, 0xf, BBEP_BLACK);

    on_sprite_1bpp.initSprite(sprite_size_, sprite_size_);
    on_sprite_1bpp.setMode(BB_MODE_1BPP);
    on_sprite_1bpp.fillScreen(BBEP_WHITE);
    on_sprite_1bpp.fillCircle(icon_center, icon_center, icon_radius, BBEP_BLACK);
    on_sprite_1bpp.loadBMP(on_icon, icon_pos, icon_pos, BBEP_BLACK, BBEP_WHITE);

    off_sprite_1bpp.initSprite(sprite_size_, sprite_size_);
    off_sprite_1bpp.setMode(BB_MODE_1BPP);
    off_sprite_1bpp.fillScreen(BBEP_WHITE);
    off_sprite_1bpp.fillCircle(icon_center, icon_center, icon_radius, BBEP_BLACK);
    off_sprite_1bpp.fillCircle(icon_center, icon_center, icon_radius - BUTTON_BORDER_SIZE, BBEP_WHITE);
    off_sprite_1bpp.loadBMP(off_icon, icon_pos, icon_pos, BBEP_WHITE, BBEP_BLACK);

    // Compute the hit box
    const int x_min = static_cast<int>(rect_.x) - TOUCH_AREA_MARGIN;
    const int y_min = static_cast<int>(rect_.y) - TOUCH_AREA_MARGIN;
    hit_rect_ = Rect{
        static_cast<uint16_t>(x_min < 0 ? 0 : x_min),
        static_cast<uint16_t>(y_min < 0 ? 0 : y_min),
        static_cast<uint16_t>(rect_.w + 2 * TOUCH_AREA_MARGIN),
        static_cast<uint16_t>(rect_.h + 2 * TOUCH_AREA_MARGIN),
    };
}

Rect OnOffButton::partialDraw(FASTEPD* display, BitDepth depth, uint8_t from, uint8_t to) {
    if (to) {
        if (depth == BitDepth::BD_4BPP) {
            display->drawSprite(&on_sprite_4bpp, icon_rect_.x, icon_rect_.y);
        } else {
            display->drawSprite(&on_sprite_1bpp, icon_rect_.x, icon_rect_.y);
        }
    } else {
        if (depth == BitDepth::BD_4BPP) {
            display->drawSprite(&off_sprite_4bpp, icon_rect_.x, icon_rect_.y);
        } else {
            display->drawSprite(&off_sprite_1bpp, icon_rect_.x, icon_rect_.y);
        }
    }

    return Rect{icon_rect_.x, icon_rect_.y, icon_rect_.w, icon_rect_.h};
}

void OnOffButton::fullDraw(FASTEPD* display, BitDepth depth, uint8_t value) {
    const uint8_t white = depth == BitDepth::BD_4BPP ? 0xf : BBEP_WHITE;
    display->fillRoundRect(rect_.x, rect_.y, rect_.w, rect_.h, 18, white);
    display->drawRoundRect(rect_.x, rect_.y, rect_.w, rect_.h, 18, BBEP_BLACK);
    if (label_rect_.y > rect_.y + 6) {
        const int16_t divider_y = static_cast<int16_t>(label_rect_.y) - 4;
        display->drawLine(rect_.x + 12, divider_y, rect_.x + rect_.w - 12, divider_y, BBEP_BLACK);
    }

    partialDraw(display, depth, 0, value);

    // Fit the label in one centered line below the icon.
    const int16_t max_w = static_cast<int16_t>(label_rect_.w);
    char draw_label[MAX_ENTITY_NAME_LEN];
    copy_text(draw_label, sizeof(draw_label), label_);

    uint8_t font_idx = 3;
    for (uint8_t idx = 0; idx <= 3; idx++) {
        set_label_font(display, idx);
        BB_RECT text_rect = get_text_box(display, draw_label);
        if (text_rect.w <= max_w && text_rect.h <= label_rect_.h) {
            font_idx = idx;
            break;
        }
    }

    set_label_font(display, font_idx);
    truncate_with_ellipsis(display, draw_label, sizeof(draw_label), max_w);
    display->setTextColor(BBEP_BLACK);
    BB_RECT text_rect = get_text_box(display, draw_label);
    int16_t text_x = static_cast<int16_t>(label_rect_.x) + static_cast<int16_t>(label_rect_.w - text_rect.w) / 2;
    int16_t text_y = static_cast<int16_t>(label_rect_.y) + static_cast<int16_t>(label_rect_.h + text_rect.h) / 2 - 2;
    display->setCursor(text_x, text_y);
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
