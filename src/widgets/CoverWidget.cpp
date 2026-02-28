#include "widgets/CoverWidget.h"
#include "assets/Montserrat_Regular_16.h"
#include "assets/Montserrat_Regular_20.h"
#include "assets/icons.h"
#include "constants.h"
#include <FastEPD.h>
#include <algorithm>
#include <cstring>

static bool point_in_rect(const TouchEvent* touch_event, const Rect* rect) {
    return touch_event->x >= rect->x && touch_event->x < rect->x + rect->w && touch_event->y >= rect->y &&
           touch_event->y < rect->y + rect->h;
}

static BB_RECT get_text_box(FASTEPD* display, const char* text) {
    BB_RECT rect = {};
    display->getStringBox(text, &rect);
    return rect;
}

static Rect make_rect(int16_t x, int16_t y, int16_t w, int16_t h) {
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (w < 1) {
        w = 1;
    }
    if (h < 1) {
        h = 1;
    }
    return Rect{
        .x = static_cast<uint16_t>(x),
        .y = static_cast<uint16_t>(y),
        .w = static_cast<uint16_t>(w),
        .h = static_cast<uint16_t>(h),
    };
}

static void draw_text_at(FASTEPD* display, int16_t x, int16_t y, const char* text, bool reinforce = false) {
    display->setCursor(x, y);
    display->write(text);
    if (reinforce) {
        display->setCursor(x + 1, y);
        display->write(text);
    }
}

static void draw_centered_text(FASTEPD* display, const char* text, const Rect* rect, bool reinforce = false, int16_t y_offset = 0) {
    BB_RECT text_rect = get_text_box(display, text);
    const int16_t x = static_cast<int16_t>(rect->x) + static_cast<int16_t>(rect->w - text_rect.w) / 2;
    const int16_t y = static_cast<int16_t>(rect->y) + static_cast<int16_t>(rect->h + text_rect.h) / 2 - 2 + y_offset;
    draw_text_at(display, x, y, text, reinforce);
}

static void truncate_with_ellipsis(FASTEPD* display, char* text, size_t text_len, int16_t max_w) {
    if (!text || text[0] == '\0' || max_w <= 0) {
        if (text && text_len > 0) {
            text[0] = '\0';
        }
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
            strncpy(text, candidate, text_len - 1);
            text[text_len - 1] = '\0';
            return;
        }
    }

    strncpy(text, "...", text_len - 1);
    text[text_len - 1] = '\0';
}

static void draw_cover_action_button(FASTEPD* display, const Rect* rect, bool up, bool active, uint8_t white) {
    const uint8_t fill = active ? BBEP_BLACK : white;
    display->fillRoundRect(rect->x, rect->y, rect->w, rect->h, 12, fill);
    display->drawRoundRect(rect->x, rect->y, rect->w, rect->h, 12, BBEP_BLACK);

    const uint8_t* icon = up ? cover_up : cover_down;
    const int16_t icon_x = static_cast<int16_t>(rect->x) + std::max<int16_t>(0, (static_cast<int16_t>(rect->w) - BUTTON_ICON_SIZE) / 2);
    const int16_t icon_y = static_cast<int16_t>(rect->y) + std::max<int16_t>(0, (static_cast<int16_t>(rect->h) - BUTTON_ICON_SIZE) / 2);
    const uint8_t fg = active ? white : BBEP_BLACK;
    display->loadBMP(icon, icon_x, icon_y, fill, fg);
}

CoverWidget::CoverWidget(const char* label, Rect rect)
    : rect_(rect) {
    strncpy(label_, label ? label : "", sizeof(label_) - 1);
    label_[sizeof(label_) - 1] = '\0';

    const int16_t rect_x = static_cast<int16_t>(rect_.x);
    const int16_t rect_y = static_cast<int16_t>(rect_.y);
    const int16_t rect_w = static_cast<int16_t>(rect_.w);
    const int16_t rect_h = static_cast<int16_t>(rect_.h);

    hit_rect_ = make_rect(rect_x - TOUCH_AREA_MARGIN, rect_y - TOUCH_AREA_MARGIN, rect_w + 2 * TOUCH_AREA_MARGIN,
                          rect_h + 2 * TOUCH_AREA_MARGIN);

    const int16_t pad = 14;
    const int16_t gap = 12;
    const int16_t label_h = 34;
    const int16_t button_h = std::max<int16_t>(60, rect_h - (label_h + 2 * pad + gap));
    const int16_t button_y = rect_y + rect_h - pad - button_h;

    label_rect_ = make_rect(rect_x + pad, rect_y + 10, rect_w - 2 * pad, label_h);

    const int16_t buttons_x = rect_x + pad;
    const int16_t buttons_w = rect_w - 2 * pad;
    const int16_t button_w = (buttons_w - gap) / 2;
    up_rect_ = make_rect(buttons_x, button_y, button_w, button_h);
    down_rect_ = make_rect(buttons_x + button_w + gap, button_y, buttons_w - button_w - gap, button_h);
}

Rect CoverWidget::partialDraw(FASTEPD* display, BitDepth depth, uint8_t from, uint8_t to) {
    (void)from;
    fullDraw(display, depth, to);
    return rect_;
}

void CoverWidget::fullDraw(FASTEPD* display, BitDepth depth, uint8_t value) {
    const uint8_t white = depth == BitDepth::BD_4BPP ? 0xf : BBEP_WHITE;
    display->fillRoundRect(rect_.x, rect_.y, rect_.w, rect_.h, 18, white);
    display->drawRoundRect(rect_.x, rect_.y, rect_.w, rect_.h, 18, BBEP_BLACK);
    display->setTextColor(BBEP_BLACK);

    char draw_label[MAX_ENTITY_NAME_LEN];
    strncpy(draw_label, label_, sizeof(draw_label) - 1);
    draw_label[sizeof(draw_label) - 1] = '\0';
    display->setFont(Montserrat_Regular_20);
    truncate_with_ellipsis(display, draw_label, sizeof(draw_label), static_cast<int16_t>(label_rect_.w));
    draw_centered_text(display, draw_label, &label_rect_, true);

    const bool up_active = value != 0;
    draw_cover_action_button(display, &up_rect_, true, up_active, white);
    draw_cover_action_button(display, &down_rect_, false, !up_active, white);
}

bool CoverWidget::isTouching(const TouchEvent* touch_event) const {
    return point_in_rect(touch_event, &hit_rect_);
}

uint8_t CoverWidget::getValueFromTouch(const TouchEvent* touch_event, uint8_t original_value) const {
    if (!isTouching(touch_event)) {
        return original_value;
    }
    if (point_in_rect(touch_event, &up_rect_)) {
        return 1;
    }
    if (point_in_rect(touch_event, &down_rect_)) {
        return 0;
    }
    return original_value;
}
