#include "widgets/ClimateWidget.h"
#include "assets/Montserrat_Regular_16.h"
#include "assets/Montserrat_Regular_20.h"
#include "assets/Montserrat_Regular_26.h"
#include "climate_value.h"
#include "constants.h"
#include <FastEPD.h>
#include <cstdio>
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

static void draw_centered_text(FASTEPD* display, const char* text, const Rect* rect, int16_t y_offset = 0) {
    BB_RECT text_box = get_text_box(display, text);
    const int16_t x = static_cast<int16_t>(rect->x) + static_cast<int16_t>(rect->w - text_box.w) / 2;
    const int16_t y = static_cast<int16_t>(rect->y) + static_cast<int16_t>(rect->h + text_box.h) / 2 - 2 + y_offset;
    display->setCursor(x, y);
    display->write(text);
}

static const char* climate_mode_label(ClimateMode mode) {
    switch (mode) {
    case ClimateMode::Off:
        return "OFF";
    case ClimateMode::Heat:
        return "HEAT";
    case ClimateMode::Cool:
        return "COOL";
    default:
        return "OFF";
    }
}

static void draw_mode_button(FASTEPD* display, const Rect* rect, const char* label, bool active, uint8_t white) {
    const uint8_t fill = active ? BBEP_BLACK : white;
    if (rect->w > 2 && rect->h > 2) {
        display->fillRect(rect->x + 1, rect->y + 1, rect->w - 2, rect->h - 2, fill);
    }
    display->drawRect(rect->x, rect->y, rect->w, rect->h, BBEP_BLACK);

    display->setTextColor(active ? white : BBEP_BLACK);
    display->setFont(Montserrat_Regular_16);
    draw_centered_text(display, label, rect);
    display->setTextColor(BBEP_BLACK);
}

ClimateWidget::ClimateWidget(const char* label, Rect rect, uint8_t climate_mode_mask)
    : rect_(rect) {
    strncpy(label_, label ? label : "", sizeof(label_) - 1);
    label_[sizeof(label_) - 1] = '\0';
    mode_button_count_ = 0;

    climate_mode_mask = climate_normalize_mode_mask(climate_mode_mask);
    mode_buttons_[mode_button_count_++] = ClimateMode::Off;
    if (climate_is_mode_supported(climate_mode_mask, ClimateMode::Heat)) {
        mode_buttons_[mode_button_count_++] = ClimateMode::Heat;
    }
    if (climate_is_mode_supported(climate_mode_mask, ClimateMode::Cool)) {
        mode_buttons_[mode_button_count_++] = ClimateMode::Cool;
    }

    const int16_t rect_x = static_cast<int16_t>(rect_.x);
    const int16_t rect_y = static_cast<int16_t>(rect_.y);
    const int16_t rect_w = static_cast<int16_t>(rect_.w);
    const int16_t rect_h = static_cast<int16_t>(rect_.h);

    hit_rect_ = make_rect(rect_x - TOUCH_AREA_MARGIN, rect_y - TOUCH_AREA_MARGIN, rect_w + 2 * TOUCH_AREA_MARGIN,
                          rect_h + 2 * TOUCH_AREA_MARGIN);

    const int16_t pad = 14;
    const int16_t row_gap = 10;

    const int16_t label_h = 32;
    const int16_t label_y = rect_y + 10;
    label_rect_ = make_rect(rect_x + pad, label_y, rect_w - 2 * pad, label_h);

    const int16_t mode_y = label_y + label_h + row_gap;
    int16_t controls_h = 72;
    int16_t controls_y = rect_y + rect_h - 14 - controls_h;
    int16_t mode_h = controls_y - row_gap - mode_y;
    if (mode_h < 54) {
        mode_h = 54;
        controls_y = mode_y + mode_h + row_gap;
        controls_h = rect_y + rect_h - 14 - controls_y;
        if (controls_h < 44) {
            controls_h = 44;
        }
    }

    const int16_t mode_gap = 8;
    const int16_t mode_row_x = rect_x + pad;
    const int16_t mode_row_w = rect_w - 2 * pad;
    const int16_t mode_w = (mode_row_w - (mode_button_count_ - 1) * mode_gap) / mode_button_count_;
    const int16_t mode_row_right = mode_row_x + mode_row_w;
    for (uint8_t i = 0; i < mode_button_count_; i++) {
        const int16_t mode_x = mode_row_x + i * (mode_w + mode_gap);
        int16_t mode_right = i == mode_button_count_ - 1 ? mode_row_right : mode_x + mode_w;
        if (mode_right > mode_row_right) {
            mode_right = mode_row_right;
        }
        mode_rects_[i] = make_rect(mode_x, mode_y, mode_right - mode_x, mode_h);
    }

    const int16_t controls_x = rect_x + pad;
    const int16_t controls_w = rect_w - 2 * pad;
    int16_t button_w = 80;
    const int16_t control_gap = 12;
    const int16_t min_temp_w = 100;
    if (button_w * 2 + control_gap * 2 + min_temp_w > controls_w) {
        button_w = (controls_w - control_gap * 2 - min_temp_w) / 2;
        if (button_w < 32) {
            button_w = 32;
        }
    }

    const int16_t minus_x = controls_x;
    const int16_t plus_x = controls_x + controls_w - button_w;
    const int16_t temp_x = minus_x + button_w + control_gap;
    const int16_t temp_w = plus_x - control_gap - temp_x;

    minus_rect_ = make_rect(minus_x, controls_y, button_w, controls_h);
    plus_rect_ = make_rect(plus_x, controls_y, button_w, controls_h);
    temp_adjust_value_rect_ = make_rect(temp_x, controls_y, temp_w, controls_h);
}

Rect ClimateWidget::partialDraw(FASTEPD* display, BitDepth depth, uint8_t from, uint8_t to) {
    (void)from;
    fullDraw(display, depth, to);
    return rect_;
}

void ClimateWidget::fullDraw(FASTEPD* display, BitDepth depth, uint8_t value) {
    const uint8_t white = depth == BitDepth::BD_4BPP ? 0xf : BBEP_WHITE;
    display->fillRect(rect_.x, rect_.y, rect_.w, rect_.h, white);
    display->drawRect(rect_.x, rect_.y, rect_.w, rect_.h, BBEP_BLACK);
    display->setTextColor(BBEP_BLACK);

    char draw_label[MAX_ENTITY_NAME_LEN];
    strncpy(draw_label, label_, sizeof(draw_label) - 1);
    draw_label[sizeof(draw_label) - 1] = '\0';

    display->setFont(Montserrat_Regular_20);
    truncate_with_ellipsis(display, draw_label, sizeof(draw_label), static_cast<int16_t>(label_rect_.w));
    BB_RECT label_box = get_text_box(display, draw_label);
    const int16_t label_y = static_cast<int16_t>(label_rect_.y) + static_cast<int16_t>(label_rect_.h + label_box.h) / 2 - 2;
    display->setCursor(label_rect_.x, label_y);
    display->write(draw_label);

    ClimateMode mode = climate_unpack_mode(value);
    bool mode_visible = false;
    for (uint8_t i = 0; i < mode_button_count_; i++) {
        if (mode_buttons_[i] == mode) {
            mode_visible = true;
            break;
        }
    }
    if (!mode_visible) {
        mode = ClimateMode::Off;
    }
    uint8_t temp_steps = climate_unpack_temp_steps(value);
    float temp_c = climate_steps_to_celsius(temp_steps);

    for (uint8_t i = 0; i < mode_button_count_; i++) {
        ClimateMode button_mode = mode_buttons_[i];
        draw_mode_button(display, &mode_rects_[i], climate_mode_label(button_mode), button_mode == mode, white);
    }

    display->drawRect(minus_rect_.x, minus_rect_.y, minus_rect_.w, minus_rect_.h, BBEP_BLACK);
    display->drawRect(plus_rect_.x, plus_rect_.y, plus_rect_.w, plus_rect_.h, BBEP_BLACK);
    display->drawRect(temp_adjust_value_rect_.x, temp_adjust_value_rect_.y, temp_adjust_value_rect_.w, temp_adjust_value_rect_.h, BBEP_BLACK);

    display->setFont(Montserrat_Regular_26);
    draw_centered_text(display, "-", &minus_rect_, -2);
    draw_centered_text(display, "+", &plus_rect_, -2);

    char temp_text[16];
    snprintf(temp_text, sizeof(temp_text), "%.1fC", temp_c);
    display->setFont(Montserrat_Regular_20);
    draw_centered_text(display, temp_text, &temp_adjust_value_rect_);
}

bool ClimateWidget::isTouching(const TouchEvent* touch_event) const {
    return point_in_rect(touch_event, &hit_rect_);
}

uint8_t ClimateWidget::getValueFromTouch(const TouchEvent* touch_event, uint8_t original_value) const {
    if (!isTouching(touch_event)) {
        return original_value;
    }

    ClimateMode mode = climate_unpack_mode(original_value);
    uint8_t temp_steps = climate_unpack_temp_steps(original_value);

    for (uint8_t i = 0; i < mode_button_count_; i++) {
        if (point_in_rect(touch_event, &mode_rects_[i])) {
            return climate_pack_value(mode_buttons_[i], temp_steps);
        }
    }

    if (point_in_rect(touch_event, &minus_rect_)) {
        if (temp_steps > 0) {
            temp_steps--;
        }
        return climate_pack_value(mode, temp_steps);
    }

    if (point_in_rect(touch_event, &plus_rect_)) {
        if (temp_steps < CLIMATE_TEMP_MAX_STEPS) {
            temp_steps++;
        }
        return climate_pack_value(mode, temp_steps);
    }

    return original_value;
}
