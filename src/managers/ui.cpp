#include "ui.h"
#include "assets/Montserrat_Regular_16.h"
#include "assets/Montserrat_Regular_20.h"
#include "assets/Montserrat_Regular_26.h"
#include "assets/icons.h"
#include "boards.h"
#include "constants.h"
#include "draw.h"
#include "screen.h"
#include "store.h"
#include "widgets/Widget.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

static const char* TAG = "ui";
static const char* const TEXT_BOOT[] = {"Home Assistant", "e-paper remote", nullptr};
static const char* const TEXT_WIFI_DISCONNECTED[] = {"Not connected", "to Wifi", "Tap to open Wi-Fi settings", nullptr};
static const char* const TEXT_HASS_DISCONNECTED[] = {"Not connected", "to Home Assistant", nullptr};
static const char* const TEXT_HASS_INVALID_KEY[] = {"Cannot connect", "to Home Assistant:", "invalid token", nullptr};
static const char* const TEXT_GENERIC_ERROR[] = {"Unknown error", nullptr};

static const char* strip_mdi_prefix(const char* icon_name) {
    if (!icon_name) {
        return nullptr;
    }
    if (strncmp(icon_name, "mdi:", 4) == 0) {
        return icon_name + 4;
    }
    return icon_name;
}

static const uint8_t* ui_icon_for_ha_icon(const char* icon_name) {
    const char* mdi_name = strip_mdi_prefix(icon_name);
    if (!mdi_name || mdi_name[0] == '\0') {
        return nullptr;
    }

    if (strcmp(mdi_name, "account-cowboy-hat") == 0) {
        return account_cowboy_hat;
    }
    if (strcmp(mdi_name, "bathtub-outline") == 0 || strcmp(mdi_name, "bathtub") == 0) {
        return bathtub_outline;
    }
    if (strcmp(mdi_name, "bed") == 0 || strcmp(mdi_name, "bed-outline") == 0) {
        return bed;
    }
    if (strcmp(mdi_name, "countertop") == 0) {
        return countertop;
    }
    if (strcmp(mdi_name, "cradle") == 0) {
        return cradle;
    }
    if (strcmp(mdi_name, "door") == 0 || strcmp(mdi_name, "door-open") == 0) {
        return door;
    }
    if (strcmp(mdi_name, "garage") == 0) {
        return garage;
    }
    if (strcmp(mdi_name, "office-building") == 0 || strcmp(mdi_name, "office-building-outline") == 0) {
        return office_building;
    }
    if (strcmp(mdi_name, "shower-head") == 0 || strcmp(mdi_name, "shower") == 0) {
        return shower_head;
    }
    if (strcmp(mdi_name, "sofa") == 0) {
        return sofa;
    }
    if (strcmp(mdi_name, "stairs-up") == 0 || strcmp(mdi_name, "stairs") == 0) {
        return stairs_up;
    }
    if (strcmp(mdi_name, "walk") == 0 || strcmp(mdi_name, "walking") == 0) {
        return walk;
    }

    return home_outline;
}

static bool ui_draw_room_tile_icon(FASTEPD* epaper, int16_t tile_x, int16_t tile_y, int16_t tile_w, int16_t tile_h, const char* icon_name) {
    const uint8_t* icon = ui_icon_for_ha_icon(icon_name);
    if (!icon) {
        return false;
    }

    const int16_t reserved_height = ROOM_LIST_TILE_ICON_TOP_PADDING + ROOM_LIST_TILE_ICON_SIZE + ROOM_LIST_TILE_ICON_LABEL_GAP;
    if (reserved_height >= tile_h) {
        return false;
    }

    const int16_t icon_x = tile_x + (tile_w - ROOM_LIST_TILE_ICON_SIZE) / 2;
    const int16_t icon_y = tile_y + ROOM_LIST_TILE_ICON_TOP_PADDING;
    epaper->loadBMP(icon, icon_x, icon_y, 0xf, BBEP_BLACK);
    return true;
}

void accumulate_damage(Rect& acc, const Rect& r) {
    if (r.w <= 0 || r.h <= 0) {
        return;
    }

    if (acc.w <= 0 || acc.h <= 0) {
        acc = r;
        return;
    }

    const int16_t x1 = std::min(acc.x, r.x);
    const int16_t y1 = std::min(acc.y, r.y);
    const int16_t x2 = std::max(acc.x + acc.w, r.x + r.w);
    const int16_t y2 = std::max(acc.y + acc.h, r.y + r.h);

    acc.x = x1;
    acc.y = y1;
    acc.w = x2 - x1;
    acc.h = y2 - y1;
}

void ui_room_controls_draw_widgets(UIState* state, BitDepth depth, Screen* screen, FASTEPD* epaper) {
    for (uint8_t widget_idx = 0; widget_idx < screen->widget_count; widget_idx++) {
        screen->widgets[widget_idx]->fullDraw(epaper, depth, state->widget_values[widget_idx]);
    }
}

void ui_show_message(UiMode mode, FASTEPD* epaper) {
    const uint8_t* icon = alert_circle;
    const char* const* text_lines = TEXT_GENERIC_ERROR;

    switch (mode) {
    case UiMode::Boot:
        icon = home_assistant;
        text_lines = TEXT_BOOT;
        break;
    case UiMode::WifiDisconnected:
        icon = wifi_off;
        text_lines = TEXT_WIFI_DISCONNECTED;
        break;
    case UiMode::HassDisconnected:
        icon = server_network_off;
        text_lines = TEXT_HASS_DISCONNECTED;
        break;
    case UiMode::HassInvalidKey:
        icon = lock_alert_outline;
        text_lines = TEXT_HASS_INVALID_KEY;
        break;
    default:
        break;
    }

    drawCenteredIconWithText(epaper, icon, text_lines, 30, 100);
}

static void set_room_list_font(FASTEPD* epaper, uint8_t font_idx) {
    switch (font_idx) {
    case 0:
        epaper->setFont(Montserrat_Regular_26);
        break;
    case 1:
        epaper->setFont(Montserrat_Regular_20);
        break;
    default:
        epaper->setFont(Montserrat_Regular_16);
        break;
    }
}

static BB_RECT get_text_box(FASTEPD* epaper, const char* text) {
    BB_RECT rect = {};
    epaper->getStringBox(text, &rect);
    return rect;
}

static void draw_text_at(FASTEPD* epaper, int16_t x, int16_t y, const char* text, bool reinforce = false) {
    epaper->setCursor(x, y);
    epaper->write(text);
    if (reinforce) {
        epaper->setCursor(x + 1, y);
        epaper->write(text);
    }
}

static void ui_copy_string(char* dst, size_t dst_len, const char* src) {
    if (dst_len == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static bool contains_case_insensitive(const char* haystack, const char* needle) {
    if (!haystack || !needle || needle[0] == '\0') {
        return false;
    }

    const size_t needle_len = strlen(needle);
    const size_t haystack_len = strlen(haystack);
    if (needle_len > haystack_len) {
        return false;
    }

    for (size_t i = 0; i + needle_len <= haystack_len; i++) {
        bool match = true;
        for (size_t j = 0; j < needle_len; j++) {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') {
                a = static_cast<char>(a + ('a' - 'A'));
            }
            if (b >= 'A' && b <= 'Z') {
                b = static_cast<char>(b + ('a' - 'A'));
            }
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

static void trim_copy(char* dst, size_t dst_len, const char* src, size_t len) {
    size_t start = 0;
    while (start < len && src[start] == ' ') {
        start++;
    }

    size_t end = len;
    while (end > start && src[end - 1] == ' ') {
        end--;
    }

    size_t out_len = end - start;
    if (out_len >= dst_len) {
        out_len = dst_len - 1;
    }
    memcpy(dst, src + start, out_len);
    dst[out_len] = '\0';
}

static bool split_room_name(const char* name, char* line1, size_t line1_len, char* line2, size_t line2_len) {
    const size_t len = strlen(name);
    if (len == 0) {
        line1[0] = '\0';
        line2[0] = '\0';
        return false;
    }

    int best_dist = 32767;
    size_t best_pos = len;
    for (size_t i = 1; i + 1 < len; i++) {
        if (name[i] == ' ') {
            int dist = static_cast<int>(i > len / 2 ? i - len / 2 : len / 2 - i);
            if (dist < best_dist) {
                best_dist = dist;
                best_pos = i;
            }
        }
    }

    if (best_pos == len) {
        line1[0] = '\0';
        line2[0] = '\0';
        return false;
    }

    trim_copy(line1, line1_len, name, best_pos);
    trim_copy(line2, line2_len, name + best_pos + 1, len - best_pos - 1);
    return line1[0] != '\0' && line2[0] != '\0';
}

static uint8_t fit_font_for_lines(FASTEPD* epaper, const char* line1, const char* line2, int16_t max_w, int16_t max_h) {
    const bool two_lines = line2 && line2[0] != '\0';
    for (uint8_t font_idx = 0; font_idx <= 2; font_idx++) {
        set_room_list_font(epaper, font_idx);
        BB_RECT rect1 = get_text_box(epaper, line1);
        BB_RECT rect2 = two_lines ? get_text_box(epaper, line2) : BB_RECT{};

        const int16_t line_h = rect1.h;
        const int16_t gap = font_idx == 0 ? 10 : 4;
        const int16_t h = two_lines ? static_cast<int16_t>(line_h + rect2.h + gap) : line_h;
        const int16_t w1 = rect1.w;
        const int16_t w2 = two_lines ? rect2.w : 0;
        const int16_t w = std::max(w1, w2);
        if (w <= max_w && h <= max_h) {
            return font_idx;
        }
    }
    return 255;
}

static void truncate_with_ellipsis(FASTEPD* epaper, char* line, size_t line_len, int16_t max_w) {
    if (line[0] == '\0') {
        return;
    }

    BB_RECT rect = get_text_box(epaper, line);
    if (rect.w <= max_w) {
        return;
    }

    if (line_len < 4) {
        line[0] = '\0';
        return;
    }

    char candidate[64];
    size_t current_len = strlen(line);
    while (current_len > 0) {
        current_len--;
        size_t keep = current_len;
        if (keep > sizeof(candidate) - 4) {
            keep = sizeof(candidate) - 4;
        }

        memcpy(candidate, line, keep);
        candidate[keep] = '.';
        candidate[keep + 1] = '.';
        candidate[keep + 2] = '.';
        candidate[keep + 3] = '\0';

        rect = get_text_box(epaper, candidate);
        if (rect.w <= max_w) {
            strncpy(line, candidate, line_len - 1);
            line[line_len - 1] = '\0';
            return;
        }
    }

    strncpy(line, "...", line_len - 1);
    line[line_len - 1] = '\0';
}

static void ui_draw_room_tile_label(FASTEPD* epaper, int16_t label_x, int16_t label_y, int16_t label_w, int16_t label_h, const char* name) {
    constexpr int16_t pad_x = 12;
    constexpr int16_t pad_y = 6;
    const int16_t max_w = label_w - pad_x * 2;
    const int16_t max_h = label_h - pad_y * 2;
    if (max_w <= 0 || max_h <= 0) {
        return;
    }

    char line1[64];
    char line2[64];
    char split1[64];
    char split2[64];
    strncpy(line1, name, sizeof(line1) - 1);
    line1[sizeof(line1) - 1] = '\0';
    line2[0] = '\0';

    bool has_split = split_room_name(name, split1, sizeof(split1), split2, sizeof(split2));
    uint8_t one_line_font = fit_font_for_lines(epaper, line1, "", max_w, max_h);
    uint8_t split_font = has_split ? fit_font_for_lines(epaper, split1, split2, max_w, max_h) : 255;

    uint8_t font_idx = one_line_font;
    if (split_font < font_idx) {
        strncpy(line1, split1, sizeof(line1) - 1);
        line1[sizeof(line1) - 1] = '\0';
        strncpy(line2, split2, sizeof(line2) - 1);
        line2[sizeof(line2) - 1] = '\0';
        font_idx = split_font;
    }

    if (font_idx == 255) {
        font_idx = 2;
        line2[0] = '\0';
    }

    set_room_list_font(epaper, font_idx);
    truncate_with_ellipsis(epaper, line1, sizeof(line1), max_w);
    if (line2[0] != '\0') {
        truncate_with_ellipsis(epaper, line2, sizeof(line2), max_w);
    }

    BB_RECT rect1 = get_text_box(epaper, line1);
    BB_RECT rect2 = line2[0] != '\0' ? get_text_box(epaper, line2) : BB_RECT{};
    const bool two_lines = line2[0] != '\0';
    const int16_t gap = font_idx == 0 ? 10 : 4;
    const int16_t total_h = two_lines ? static_cast<int16_t>(rect1.h + rect2.h + gap) : rect1.h;
    const int16_t top = label_y + (label_h - total_h) / 2;
    const bool reinforce = font_idx != 0;
    draw_text_at(epaper, label_x + (label_w - rect1.w) / 2, top, line1, reinforce);

    if (two_lines) {
        draw_text_at(epaper, label_x + (label_w - rect2.w) / 2, top + rect1.h + gap, line2, reinforce);
    }
}

static void ui_draw_back_icon(FASTEPD* epaper) {
    const int16_t center_x = ROOM_CONTROLS_BACK_X + ROOM_CONTROLS_BACK_W / 2;
    const int16_t center_y = ROOM_CONTROLS_BACK_Y + ROOM_CONTROLS_BACK_H / 2;
    const int16_t tip_x = center_x - 20;
    const int16_t shaft_end_x = center_x + 14;
    const int16_t wing_dx = 12;
    const int16_t wing_dy = 12;

    for (int8_t t = -1; t <= 1; t++) {
        epaper->drawLine(tip_x, center_y + t, tip_x + wing_dx, center_y - wing_dy + t, BBEP_BLACK);
        epaper->drawLine(tip_x, center_y + t, tip_x + wing_dx, center_y + wing_dy + t, BBEP_BLACK);
        epaper->drawLine(tip_x, center_y + t, shaft_end_x, center_y + t, BBEP_BLACK);
    }
}

static void ui_draw_settings_icon(FASTEPD* epaper, int16_t center_x, int16_t center_y, int16_t icon_size) {
    const int16_t half_w = icon_size / 2;
    const int16_t x_left = center_x - half_w;
    const int16_t x_right = center_x + half_w;
    const int16_t y1 = center_y - icon_size / 3;
    const int16_t y2 = center_y;
    const int16_t y3 = center_y + icon_size / 3;
    const int16_t knob_r = std::max<int16_t>(3, icon_size / 8);

    for (int8_t t = -1; t <= 1; t++) {
        epaper->drawLine(x_left, y1 + t, x_right, y1 + t, BBEP_BLACK);
        epaper->drawLine(x_left, y2 + t, x_right, y2 + t, BBEP_BLACK);
        epaper->drawLine(x_left, y3 + t, x_right, y3 + t, BBEP_BLACK);
    }

    epaper->fillCircle(center_x - icon_size / 5, y1, knob_r, BBEP_BLACK);
    epaper->fillCircle(center_x + icon_size / 6, y2, knob_r, BBEP_BLACK);
    epaper->fillCircle(center_x - icon_size / 10, y3, knob_r, BBEP_BLACK);
}

static void ui_draw_back_button(FASTEPD* epaper) {
    epaper->fillRoundRect(ROOM_CONTROLS_BACK_X, ROOM_CONTROLS_BACK_Y, ROOM_CONTROLS_BACK_W, ROOM_CONTROLS_BACK_H, 14, BBEP_WHITE);
    epaper->drawRoundRect(ROOM_CONTROLS_BACK_X, ROOM_CONTROLS_BACK_Y, ROOM_CONTROLS_BACK_W, ROOM_CONTROLS_BACK_H, 14, BBEP_BLACK);
    ui_draw_back_icon(epaper);
}

static void ui_draw_floor_list_header(FASTEPD* epaper) {
    const int16_t header_x = ROOM_LIST_GRID_MARGIN_X;
    const int16_t header_y = 18;
    const int16_t header_w = DISPLAY_WIDTH - 2 * ROOM_LIST_GRID_MARGIN_X;
    const int16_t header_h = FLOOR_LIST_GRID_START_Y - header_y - 12;
    const int16_t icon_size = 64;
    const int16_t icon_x = header_x + 16;
    const int16_t icon_y = header_y + (header_h - icon_size) / 2;
    const int16_t text_x = icon_x + icon_size + 20;

    epaper->fillRoundRect(header_x, header_y, header_w, header_h, 20, 0xe);
    epaper->drawRoundRect(header_x, header_y, header_w, header_h, 20, BBEP_BLACK);
    epaper->loadBMP(home_outline, icon_x, icon_y, 0xe, BBEP_BLACK);

    epaper->setFont(Montserrat_Regular_26);
    draw_text_at(epaper, text_x, header_y + 42, "Home");

    epaper->setFont(Montserrat_Regular_16);
    draw_text_at(epaper, text_x, header_y + 68, "Choose a floor", true);

    epaper->fillRoundRect(HOME_SETTINGS_BUTTON_X, HOME_SETTINGS_BUTTON_Y, HOME_SETTINGS_BUTTON_W, HOME_SETTINGS_BUTTON_H, 14, 0xf);
    epaper->drawRoundRect(HOME_SETTINGS_BUTTON_X, HOME_SETTINGS_BUTTON_Y, HOME_SETTINGS_BUTTON_W, HOME_SETTINGS_BUTTON_H, 14, BBEP_BLACK);
    ui_draw_settings_icon(epaper, HOME_SETTINGS_BUTTON_X + HOME_SETTINGS_BUTTON_W / 2, HOME_SETTINGS_BUTTON_Y + HOME_SETTINGS_BUTTON_H / 2,
                          HOME_SETTINGS_ICON_SIZE);
}

static uint8_t list_page_count(uint8_t item_count) {
    if (item_count == 0) {
        return 1;
    }
    return static_cast<uint8_t>((item_count + ROOM_LIST_ROOMS_PER_PAGE - 1) / ROOM_LIST_ROOMS_PER_PAGE);
}

struct ListGridLayout {
    uint8_t columns;
    uint8_t rows;
    uint8_t items_per_page;
};

static ListGridLayout list_grid_layout(uint8_t item_count, uint8_t page_count, bool expand_single_page_layout) {
    ListGridLayout layout = {
        .columns = ROOM_LIST_COLUMNS,
        .rows = ROOM_LIST_ROWS,
        .items_per_page = ROOM_LIST_ROOMS_PER_PAGE,
    };

    if (!expand_single_page_layout || page_count != 1 || item_count == 0 || item_count > ROOM_LIST_ROOMS_PER_PAGE) {
        return layout;
    }

    if (item_count <= 3) {
        layout.columns = 1;
        layout.rows = item_count;
    } else {
        layout.columns = 2;
        layout.rows = static_cast<uint8_t>((item_count + 1) / 2);
    }
    layout.items_per_page = static_cast<uint8_t>(layout.columns * layout.rows);
    return layout;
}

static void ui_draw_name_grid(FASTEPD* epaper, const char names[][MAX_ROOM_NAME_LEN], const char icons[][MAX_ICON_NAME_LEN], uint8_t item_count,
                              uint8_t list_page, uint16_t grid_start_y, bool expand_single_page_layout = false) {
    const uint8_t total_pages = list_page_count(item_count);
    const uint8_t page = std::min(list_page, static_cast<uint8_t>(total_pages - 1));
    const ListGridLayout layout = list_grid_layout(item_count, total_pages, expand_single_page_layout);
    const uint8_t first_idx = static_cast<uint8_t>(page * layout.items_per_page);
    const uint8_t last_idx = std::min<uint8_t>(item_count, static_cast<uint8_t>(first_idx + layout.items_per_page));

    const int16_t grid_w = DISPLAY_WIDTH - 2 * ROOM_LIST_GRID_MARGIN_X;
    const int16_t grid_h = ROOM_LIST_GRID_BOTTOM_Y - grid_start_y;
    const int16_t tile_w = (grid_w - (layout.columns - 1) * ROOM_LIST_GRID_GAP_X) / layout.columns;
    const int16_t tile_h = (grid_h - (layout.rows - 1) * ROOM_LIST_GRID_GAP_Y) / layout.rows;

    for (uint8_t idx = first_idx; idx < last_idx; idx++) {
        const uint8_t slot = idx - first_idx;
        const uint8_t row = slot / layout.columns;
        const uint8_t col = slot % layout.columns;
        const int16_t tile_x = ROOM_LIST_GRID_MARGIN_X + col * (tile_w + ROOM_LIST_GRID_GAP_X);
        const int16_t tile_y = grid_start_y + row * (tile_h + ROOM_LIST_GRID_GAP_Y);

        epaper->fillRoundRect(tile_x, tile_y, tile_w, tile_h, ROOM_LIST_TILE_RADIUS, 0xf);
        epaper->drawRoundRect(tile_x, tile_y, tile_w, tile_h, ROOM_LIST_TILE_RADIUS, BBEP_BLACK);
        if (tile_w > 10 && tile_h > 10) {
            epaper->drawRoundRect(tile_x + 3, tile_y + 3, tile_w - 6, tile_h - 6, ROOM_LIST_TILE_RADIUS - 4, 0xd);
        }
        const char* icon_name = icons ? icons[idx] : nullptr;
        const bool has_icon = ui_draw_room_tile_icon(epaper, tile_x, tile_y, tile_w, tile_h, icon_name);

        int16_t label_y = tile_y + 4;
        int16_t label_h = tile_h - 8;
        if (has_icon) {
            label_y = tile_y + ROOM_LIST_TILE_ICON_TOP_PADDING + ROOM_LIST_TILE_ICON_SIZE + ROOM_LIST_TILE_ICON_LABEL_GAP;
            label_h = tile_h - (label_y - tile_y) - ROOM_LIST_TILE_LABEL_BOTTOM_PADDING;
        }

        ui_draw_room_tile_label(epaper, tile_x, label_y, tile_w, label_h, names[idx]);
    }

    if (total_pages > 1) {
        char page_text[20];
        snprintf(page_text, sizeof(page_text), "Page %u/%u", page + 1, total_pages);

        epaper->setFont(Montserrat_Regular_16);
        BB_RECT label_rect = get_text_box(epaper, page_text);
        const int16_t label_width = label_rect.w + 24;
        const int16_t label_x = DISPLAY_WIDTH - ROOM_LIST_GRID_MARGIN_X - label_width;
        const int16_t label_y = ROOM_LIST_FOOTER_Y - 22;

        epaper->fillRoundRect(label_x, label_y, label_width, 32, 12, 0xe);
        epaper->drawRoundRect(label_x, label_y, label_width, 32, 12, BBEP_BLACK);
        draw_text_at(epaper, label_x + 12, ROOM_LIST_FOOTER_Y, page_text, true);
    }
}

void ui_draw_floor_list(FASTEPD* epaper, const FloorListSnapshot* snapshot, uint8_t floor_list_page) {
    epaper->setTextColor(BBEP_BLACK);
    ui_draw_floor_list_header(epaper);

    if (snapshot->floor_count == 0) {
        epaper->setFont(Montserrat_Regular_26);
        draw_text_at(epaper, ROOM_LIST_GRID_MARGIN_X, FLOOR_LIST_GRID_START_Y + 40, "No floors found");
        return;
    }

    ui_draw_name_grid(epaper, snapshot->floor_names, snapshot->floor_icons, snapshot->floor_count, floor_list_page, FLOOR_LIST_GRID_START_Y, true);
}

void ui_draw_room_list_header(FASTEPD* epaper, const char* floor_name) {
    epaper->fillRect(0, 0, DISPLAY_WIDTH, ROOM_LIST_HEADER_HEIGHT, 0xe);
    ui_draw_back_button(epaper);

    epaper->setFont(Montserrat_Regular_20);
    char floor_label[MAX_FLOOR_NAME_LEN];
    strncpy(floor_label, floor_name ? floor_name : "", sizeof(floor_label) - 1);
    floor_label[sizeof(floor_label) - 1] = '\0';
    truncate_with_ellipsis(epaper, floor_label, sizeof(floor_label), DISPLAY_WIDTH - (ROOM_CONTROLS_BACK_X + ROOM_CONTROLS_BACK_W + 32) - 8);
    draw_text_at(epaper, ROOM_CONTROLS_BACK_X + ROOM_CONTROLS_BACK_W + 32, ROOM_CONTROLS_BACK_Y + 30, floor_label, true);

    epaper->setFont(Montserrat_Regular_16);
    draw_text_at(epaper, ROOM_CONTROLS_BACK_X + ROOM_CONTROLS_BACK_W + 32, ROOM_CONTROLS_BACK_Y + 56, "Choose a room", true);

    epaper->drawLine(0, ROOM_LIST_HEADER_HEIGHT, DISPLAY_WIDTH, ROOM_LIST_HEADER_HEIGHT, BBEP_BLACK);
}

void ui_draw_room_list(FASTEPD* epaper, const RoomListSnapshot* snapshot, uint8_t room_list_page) {
    epaper->setTextColor(BBEP_BLACK);
    ui_draw_room_list_header(epaper, snapshot->floor_name);

    if (snapshot->room_count == 0) {
        epaper->setFont(Montserrat_Regular_26);
        draw_text_at(epaper, ROOM_LIST_GRID_MARGIN_X, ROOM_LIST_GRID_START_Y + 40, "No rooms found");
        return;
    }

    ui_draw_name_grid(epaper, snapshot->room_names, snapshot->room_icons, snapshot->room_count, room_list_page, ROOM_LIST_GRID_START_Y, false);
}

static const char* ui_wifi_state_label(ConnState state, bool connecting) {
    if (connecting) {
        return "Connecting...";
    }
    switch (state) {
    case ConnState::Up:
        return "Connected";
    case ConnState::Initializing:
        return "Connecting...";
    case ConnState::InvalidCredentials:
        return "Auth failed";
    case ConnState::ConnectionError:
    default:
        return "Disconnected";
    }
}

static uint8_t ui_rssi_quality(int16_t rssi) {
    if (rssi <= -95) {
        return 0;
    }
    if (rssi >= -45) {
        return 100;
    }
    return static_cast<uint8_t>((rssi + 95) * 2);
}

static void ui_draw_settings_header(FASTEPD* epaper, const char* title) {
    epaper->fillRect(0, 0, DISPLAY_WIDTH, SETTINGS_HEADER_HEIGHT, 0xe);
    ui_draw_back_button(epaper);
    epaper->setFont(Montserrat_Regular_20);
    draw_text_at(epaper, ROOM_CONTROLS_BACK_X + ROOM_CONTROLS_BACK_W + 32, ROOM_CONTROLS_BACK_Y + 36, title);
    epaper->drawLine(0, SETTINGS_HEADER_HEIGHT, DISPLAY_WIDTH, SETTINGS_HEADER_HEIGHT, BBEP_BLACK);
}

void ui_draw_settings_menu(FASTEPD* epaper) {
    epaper->setTextColor(BBEP_BLACK);
    ui_draw_settings_header(epaper, "Settings");

    epaper->fillRoundRect(SETTINGS_TILE_X, SETTINGS_TILE_Y, SETTINGS_TILE_W, SETTINGS_TILE_H, 20, 0xf);
    epaper->drawRoundRect(SETTINGS_TILE_X, SETTINGS_TILE_Y, SETTINGS_TILE_W, SETTINGS_TILE_H, 20, BBEP_BLACK);

    epaper->setFont(Montserrat_Regular_20);
    draw_text_at(epaper, SETTINGS_TILE_X + 24, SETTINGS_TILE_Y + 68, "Wi-Fi");
    epaper->setFont(Montserrat_Regular_16);
    draw_text_at(epaper, SETTINGS_TILE_X + 24, SETTINGS_TILE_Y + 102, "Network settings and diagnostics");

    epaper->fillRoundRect(SETTINGS_STANDBY_TILE_X, SETTINGS_STANDBY_TILE_Y, SETTINGS_STANDBY_TILE_W, SETTINGS_STANDBY_TILE_H, 20, 0xf);
    epaper->drawRoundRect(SETTINGS_STANDBY_TILE_X, SETTINGS_STANDBY_TILE_Y, SETTINGS_STANDBY_TILE_W, SETTINGS_STANDBY_TILE_H, 20, BBEP_BLACK);

    epaper->setFont(Montserrat_Regular_20);
    draw_text_at(epaper, SETTINGS_STANDBY_TILE_X + 24, SETTINGS_STANDBY_TILE_Y + 68, "Standby Screen");
    epaper->setFont(Montserrat_Regular_16);
    draw_text_at(epaper, SETTINGS_STANDBY_TILE_X + 24, SETTINGS_STANDBY_TILE_Y + 102, "Open now for debug");
}

static void ui_draw_wifi_network_row(FASTEPD* epaper, int16_t x, int16_t y, int16_t w, const WifiNetwork& network, bool connected) {
    epaper->fillRoundRect(x, y, w, WIFI_NETWORK_ROW_H, 12, connected ? 0xe : 0xf);
    epaper->drawRoundRect(x, y, w, WIFI_NETWORK_ROW_H, 12, BBEP_BLACK);

    epaper->setFont(Montserrat_Regular_16);
    char ssid[MAX_WIFI_SSID_LEN];
    strncpy(ssid, network.ssid, sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = '\0';
    truncate_with_ellipsis(epaper, ssid, sizeof(ssid), static_cast<int16_t>(w - 190));
    draw_text_at(epaper, x + 16, y + 25, ssid);

    char right_text[40];
    snprintf(right_text, sizeof(right_text), "%s  %ddBm", network.secure ? "LOCK" : "OPEN", static_cast<int>(network.rssi));
    BB_RECT right_rect = get_text_box(epaper, right_text);
    draw_text_at(epaper, x + w - right_rect.w - 14, y + 25, right_text);
}

void ui_draw_wifi_settings(FASTEPD* epaper, const WifiSettingsSnapshot* snapshot) {
    epaper->setTextColor(BBEP_BLACK);
    ui_draw_settings_header(epaper, "Wi-Fi");

    epaper->fillRoundRect(WIFI_INFO_X, WIFI_INFO_Y, WIFI_INFO_W, WIFI_INFO_H, 14, 0xf);
    epaper->drawRoundRect(WIFI_INFO_X, WIFI_INFO_Y, WIFI_INFO_W, WIFI_INFO_H, 14, BBEP_BLACK);

    epaper->setFont(Montserrat_Regular_20);
    draw_text_at(epaper, WIFI_INFO_X + 14, WIFI_INFO_Y + 32, ui_wifi_state_label(snapshot->wifi_state, snapshot->connecting));

    epaper->setFont(Montserrat_Regular_16);
    char profile_line[96];
    if (snapshot->custom_profile_active && snapshot->profile_ssid[0] != '\0') {
        snprintf(profile_line, sizeof(profile_line), "Profile: Custom (%s)", snapshot->profile_ssid);
    } else {
        snprintf(profile_line, sizeof(profile_line), "Profile: Home default");
    }
    truncate_with_ellipsis(epaper, profile_line, sizeof(profile_line), static_cast<int16_t>(WIFI_INFO_W - 24));
    draw_text_at(epaper, WIFI_INFO_X + 14, WIFI_INFO_Y + 58, profile_line);

    char ssid_line[96];
    snprintf(ssid_line, sizeof(ssid_line), "Network: %s", snapshot->connected && snapshot->connected_ssid[0] ? snapshot->connected_ssid : "(none)");
    draw_text_at(epaper, WIFI_INFO_X + 14, WIFI_INFO_Y + 84, ssid_line);

    char ip_line[80];
    snprintf(ip_line, sizeof(ip_line), "IP: %s", snapshot->connected && snapshot->ip_address[0] ? snapshot->ip_address : "-");
    draw_text_at(epaper, WIFI_INFO_X + 14, WIFI_INFO_Y + 110, ip_line);

    char signal_line[80];
    snprintf(signal_line, sizeof(signal_line), "Signal: %d dBm (%u%%)", static_cast<int>(snapshot->rssi),
             static_cast<unsigned>(ui_rssi_quality(snapshot->rssi)));
    draw_text_at(epaper, WIFI_INFO_X + 14, WIFI_INFO_Y + 136, signal_line);

    if (snapshot->scan_in_progress) {
        draw_text_at(epaper, WIFI_INFO_X + 14, WIFI_INFO_Y + 164, "Scanning nearby networks...");
    } else if (snapshot->connect_error[0] != '\0') {
        draw_text_at(epaper, WIFI_INFO_X + 14, WIFI_INFO_Y + 164, snapshot->connect_error);
    }

    epaper->fillRoundRect(WIFI_SCAN_BUTTON_X, WIFI_SCAN_BUTTON_Y, WIFI_SCAN_BUTTON_W, WIFI_SCAN_BUTTON_H, 10, 0xf);
    epaper->drawRoundRect(WIFI_SCAN_BUTTON_X, WIFI_SCAN_BUTTON_Y, WIFI_SCAN_BUTTON_W, WIFI_SCAN_BUTTON_H, 10, BBEP_BLACK);
    epaper->setFont(Montserrat_Regular_16);
    BB_RECT scan_rect = get_text_box(epaper, "Scan");
    draw_text_at(epaper, WIFI_SCAN_BUTTON_X + (WIFI_SCAN_BUTTON_W - scan_rect.w) / 2,
                 WIFI_SCAN_BUTTON_Y + (WIFI_SCAN_BUTTON_H + scan_rect.h) / 2 - 2, "Scan");

    const char* default_label = snapshot->custom_profile_active ? "Use Default" : "On Default";
    epaper->fillRoundRect(WIFI_DEFAULT_BUTTON_X, WIFI_DEFAULT_BUTTON_Y, WIFI_DEFAULT_BUTTON_W, WIFI_DEFAULT_BUTTON_H, 10,
                          snapshot->custom_profile_active ? 0xf : 0xe);
    epaper->drawRoundRect(WIFI_DEFAULT_BUTTON_X, WIFI_DEFAULT_BUTTON_Y, WIFI_DEFAULT_BUTTON_W, WIFI_DEFAULT_BUTTON_H, 10, BBEP_BLACK);
    BB_RECT default_rect = get_text_box(epaper, default_label);
    draw_text_at(epaper, WIFI_DEFAULT_BUTTON_X + (WIFI_DEFAULT_BUTTON_W - default_rect.w) / 2,
                 WIFI_DEFAULT_BUTTON_Y + (WIFI_DEFAULT_BUTTON_H + default_rect.h) / 2 - 2, default_label);

    const uint8_t page_count = snapshot->network_count == 0
                                   ? 1
                                   : static_cast<uint8_t>((snapshot->network_count + WIFI_NETWORKS_PER_PAGE - 1) / WIFI_NETWORKS_PER_PAGE);
    const uint8_t page = std::min(snapshot->page, static_cast<uint8_t>(page_count - 1));
    const uint8_t first_idx = static_cast<uint8_t>(page * WIFI_NETWORKS_PER_PAGE);
    const uint8_t last_idx = std::min<uint8_t>(snapshot->network_count, static_cast<uint8_t>(first_idx + WIFI_NETWORKS_PER_PAGE));

    if (snapshot->network_count == 0) {
        epaper->setFont(Montserrat_Regular_16);
        draw_text_at(epaper, WIFI_NETWORK_LIST_X + 4, WIFI_NETWORK_LIST_Y + 30, "No networks found. Tap Scan.");
    } else {
        for (uint8_t idx = first_idx; idx < last_idx; idx++) {
            int16_t row_y = WIFI_NETWORK_LIST_Y + static_cast<int16_t>((idx - first_idx) * (WIFI_NETWORK_ROW_H + WIFI_NETWORK_ROW_GAP));
            bool connected = snapshot->connected && strcmp(snapshot->connected_ssid, snapshot->networks[idx].ssid) == 0;
            ui_draw_wifi_network_row(epaper, WIFI_NETWORK_LIST_X, row_y, WIFI_NETWORK_LIST_W, snapshot->networks[idx], connected);
        }
    }

    if (page_count > 1) {
        char page_text[24];
        snprintf(page_text, sizeof(page_text), "Page %u/%u", static_cast<unsigned>(page + 1), static_cast<unsigned>(page_count));
        BB_RECT page_rect = get_text_box(epaper, page_text);
        const int16_t badge_w = page_rect.w + 22;
        const int16_t badge_x = DISPLAY_WIDTH - WIFI_INFO_X - badge_w;
        epaper->fillRoundRect(badge_x, WIFI_NETWORK_PAGE_BADGE_Y - 24, badge_w, 34, 10, 0xf);
        epaper->drawRoundRect(badge_x, WIFI_NETWORK_PAGE_BADGE_Y - 24, badge_w, 34, 10, BBEP_BLACK);
        draw_text_at(epaper, badge_x + 11, WIFI_NETWORK_PAGE_BADGE_Y, page_text);
    }
}

static void ui_draw_key(FASTEPD* epaper, int16_t x, int16_t y, int16_t w, int16_t h, const char* label, bool active = false) {
    epaper->fillRoundRect(x, y, w, h, 8, active ? 0xe : 0xf);
    epaper->drawRoundRect(x, y, w, h, 8, BBEP_BLACK);
    BB_RECT text_rect = get_text_box(epaper, label);
    draw_text_at(epaper, x + (w - text_rect.w) / 2, y + (h + text_rect.h) / 2 - 1, label);
}

void ui_draw_wifi_password(FASTEPD* epaper, const WifiPasswordSnapshot* snapshot) {
    epaper->setTextColor(BBEP_BLACK);
    ui_draw_settings_header(epaper, "Wi-Fi Password");
    epaper->setFont(Montserrat_Regular_16);

    epaper->fillRoundRect(WIFI_PASSWORD_BOX_X, WIFI_PASSWORD_BOX_Y, WIFI_PASSWORD_BOX_W, WIFI_PASSWORD_BOX_H, 14, 0xf);
    epaper->drawRoundRect(WIFI_PASSWORD_BOX_X, WIFI_PASSWORD_BOX_Y, WIFI_PASSWORD_BOX_W, WIFI_PASSWORD_BOX_H, 14, BBEP_BLACK);

    char ssid_line[96];
    snprintf(ssid_line, sizeof(ssid_line), "Network: %s", snapshot->target_ssid);
    draw_text_at(epaper, WIFI_PASSWORD_BOX_X + 14, WIFI_PASSWORD_BOX_Y + 28, ssid_line);

    char masked[MAX_WIFI_PASSWORD_LEN + 1];
    size_t pass_len = strlen(snapshot->password);
    if (pass_len > MAX_WIFI_PASSWORD_LEN) {
        pass_len = MAX_WIFI_PASSWORD_LEN;
    }
    for (size_t i = 0; i < pass_len; i++) {
        masked[i] = '*';
    }
    masked[pass_len] = '\0';

    char pass_line[MAX_WIFI_PASSWORD_LEN + 20];
    snprintf(pass_line, sizeof(pass_line), "Password: %s", masked);
    truncate_with_ellipsis(epaper, pass_line, sizeof(pass_line), static_cast<int16_t>(WIFI_PASSWORD_BOX_W - 24));
    draw_text_at(epaper, WIFI_PASSWORD_BOX_X + 14, WIFI_PASSWORD_BOX_Y + 58, pass_line);
    draw_text_at(epaper, WIFI_PASSWORD_BOX_X + 14, WIFI_PASSWORD_BOX_Y + 88, snapshot->connecting ? "Connecting..." : "Tap Connect when ready");
    if (snapshot->connect_error[0] != '\0') {
        draw_text_at(epaper, WIFI_PASSWORD_BOX_X + 14, WIFI_PASSWORD_BOX_Y + 116, snapshot->connect_error);
    }

    const int16_t key_w = static_cast<int16_t>((WIFI_KEYBOARD_W - 9 * WIFI_KEY_GAP) / 10);
    const int16_t row1_y = WIFI_KEYBOARD_Y;
    const int16_t row2_y = row1_y + WIFI_KEY_H + WIFI_KEY_GAP;
    const int16_t row3_y = row2_y + WIFI_KEY_H + WIFI_KEY_GAP;
    const int16_t row4_y = row3_y + WIFI_KEY_H + WIFI_KEY_GAP;
    const int16_t row5_y = row4_y + WIFI_KEY_H + WIFI_KEY_GAP;

    auto draw_row = [&](int16_t row_x, int16_t row_y, const char* keys, uint8_t count) {
        for (uint8_t i = 0; i < count; i++) {
            int16_t x = static_cast<int16_t>(row_x + i * (key_w + WIFI_KEY_GAP));
            char label[2] = {keys[i], '\0'};
            if (!snapshot->symbols && snapshot->shift && label[0] >= 'a' && label[0] <= 'z') {
                label[0] = static_cast<char>(label[0] - ('a' - 'A'));
            }
            ui_draw_key(epaper, x, row_y, key_w, WIFI_KEY_H, label);
        }
    };

    const char* row1 = snapshot->symbols ? "1234567890" : "qwertyuiop";
    const char* row2 = snapshot->symbols ? "!@#$%^&*()" : "asdfghjkl";
    const char* row3 = snapshot->symbols ? "-_=+.,:/?" : "zxcvbnm";

    draw_row(WIFI_KEYBOARD_X, row1_y, row1, 10);
    draw_row(static_cast<int16_t>(WIFI_KEYBOARD_X + (key_w + WIFI_KEY_GAP) / 2), row2_y, row2, 9);
    draw_row(static_cast<int16_t>(WIFI_KEYBOARD_X + 2 * (key_w + WIFI_KEY_GAP)), row3_y, row3, 7);

    const int16_t shift_w = 88;
    const int16_t symbols_w = 92;
    const int16_t space_w = 180;
    const int16_t del_w = 88;
    const int16_t clear_w = 88;
    int16_t x = WIFI_KEYBOARD_X;
    ui_draw_key(epaper, x, row4_y, shift_w, WIFI_KEY_H, "Shift", snapshot->shift);
    x += shift_w + WIFI_KEY_GAP;
    ui_draw_key(epaper, x, row4_y, symbols_w, WIFI_KEY_H, snapshot->symbols ? "ABC" : "123", snapshot->symbols);
    x += symbols_w + WIFI_KEY_GAP;
    ui_draw_key(epaper, x, row4_y, space_w, WIFI_KEY_H, "Space");
    x += space_w + WIFI_KEY_GAP;
    ui_draw_key(epaper, x, row4_y, del_w, WIFI_KEY_H, "Del");
    x += del_w + WIFI_KEY_GAP;
    ui_draw_key(epaper, x, row4_y, clear_w, WIFI_KEY_H, "Clear");

    ui_draw_key(epaper, WIFI_KEYBOARD_X, row5_y, WIFI_KEYBOARD_W, WIFI_KEY_H, "Connect", snapshot->connecting);
}

static void format_weather_condition(const char* raw, char* out, size_t out_len) {
    if (out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!raw || raw[0] == '\0') {
        ui_copy_string(out, out_len, "No forecast");
        return;
    }

    size_t out_idx = 0;
    bool capitalize_next = true;
    for (size_t i = 0; raw[i] != '\0' && out_idx + 1 < out_len; i++) {
        char ch = raw[i];
        if (ch == '_' || ch == '-') {
            ch = ' ';
        }
        if (ch == ' ') {
            if (out_idx > 0 && out[out_idx - 1] != ' ') {
                out[out_idx++] = ' ';
            }
            capitalize_next = true;
            continue;
        }
        if (capitalize_next && ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - ('a' - 'A'));
        }
        out[out_idx++] = ch;
        capitalize_next = false;
    }
    out[out_idx] = '\0';
}

static const uint8_t* ui_weather_icon_for_condition(const char* condition) {
    if (!condition || condition[0] == '\0') {
        return home_outline;
    }
    if (contains_case_insensitive(condition, "sun") || contains_case_insensitive(condition, "clear")) {
        return climate_mode_heat;
    }
    if (contains_case_insensitive(condition, "rain") || contains_case_insensitive(condition, "storm") ||
        contains_case_insensitive(condition, "snow") || contains_case_insensitive(condition, "cloud") ||
        contains_case_insensitive(condition, "fog")) {
        return climate_mode_cool;
    }
    if (contains_case_insensitive(condition, "wind")) {
        return fan;
    }
    return home_outline;
}

static void ui_draw_centered_text(FASTEPD* epaper, int16_t center_x, int16_t baseline_y, const char* text, bool reinforce = false) {
    BB_RECT rect = get_text_box(epaper, text);
    draw_text_at(epaper, center_x - rect.w / 2, baseline_y, text, reinforce);
}

static void format_temperature_text(char* out, size_t out_len, bool valid, float value, bool include_unit = false) {
    if (!valid) {
        snprintf(out, out_len, "--");
        return;
    }
    const float rounded = std::round(value * 10.0f) / 10.0f;
    const bool whole = std::fabs(rounded - std::round(rounded)) < 0.05f;
    if (whole) {
        snprintf(out, out_len, include_unit ? "%.0fC" : "%.0f", rounded);
    } else {
        snprintf(out, out_len, include_unit ? "%.1fC" : "%.1f", rounded);
    }
}

static void format_energy_text(char* out, size_t out_len, bool valid, float value, bool include_unit = true) {
    if (!valid) {
        snprintf(out, out_len, "--");
        return;
    }
    const float abs_value = std::fabs(value);
    if (abs_value >= 100.0f) {
        snprintf(out, out_len, include_unit ? "%.0fkWh" : "%.0f", value);
    } else if (abs_value >= 10.0f) {
        snprintf(out, out_len, include_unit ? "%.1fkWh" : "%.1f", value);
    } else {
        snprintf(out, out_len, include_unit ? "%.1fkWh" : "%.1f", value);
    }
}

static void format_percent_text(char* out, size_t out_len, bool valid, float value) {
    if (!valid) {
        snprintf(out, out_len, "--");
        return;
    }
    const float rounded = std::round(value);
    snprintf(out, out_len, "%.0f", rounded);
}

static void ui_draw_energy_node(FASTEPD* epaper,
                                int16_t center_x,
                                int16_t center_y,
                                int16_t radius,
                                const uint8_t* icon,
                                const char* label) {
    epaper->fillCircle(center_x, center_y, radius, BBEP_BLACK);
    epaper->fillCircle(center_x, center_y, radius - 3, 0xf);

    if (icon) {
        epaper->loadBMP(icon, center_x - 28, center_y - radius + 10, 0xf, BBEP_BLACK);
    }

    epaper->setFont(Montserrat_Regular_16);
    ui_draw_centered_text(epaper, center_x, center_y - 2, label, true);
}

void ui_draw_standby(FASTEPD* epaper, const StandbySnapshot* snapshot) {
    epaper->setTextColor(BBEP_BLACK);
    epaper->fillScreen(0xf);

    const int16_t card_x = STANDBY_MARGIN;
    const int16_t card_w = DISPLAY_WIDTH - 2 * STANDBY_MARGIN;

    // Weather card
    epaper->fillRoundRect(card_x, STANDBY_WEATHER_Y, card_w, STANDBY_WEATHER_H, 20, 0xf);
    epaper->drawRoundRect(card_x, STANDBY_WEATHER_Y, card_w, STANDBY_WEATHER_H, 20, BBEP_BLACK);

    const uint8_t* weather_icon = ui_weather_icon_for_condition(snapshot->weather_condition);
    epaper->loadBMP(weather_icon, card_x + 20, STANDBY_WEATHER_Y + 48, 0xf, BBEP_BLACK);

    char condition_line[MAX_STANDBY_CONDITION_LEN];
    format_weather_condition(snapshot->weather_condition, condition_line, sizeof(condition_line));
    epaper->setFont(Montserrat_Regular_26);
    truncate_with_ellipsis(epaper, condition_line, sizeof(condition_line), card_w - 240);
    draw_text_at(epaper, card_x + 106, STANDBY_WEATHER_Y + 92, condition_line, true);

    epaper->setFont(Montserrat_Regular_20);
    draw_text_at(epaper, card_x + 106, STANDBY_WEATHER_Y + 126, "Forecast Home");

    char now_temp[20];
    char hi_temp[20];
    char low_temp[20];
    format_temperature_text(now_temp, sizeof(now_temp), snapshot->weather_temperature_valid, snapshot->weather_temperature_c, true);
    format_temperature_text(hi_temp, sizeof(hi_temp), snapshot->weather_high_valid, snapshot->weather_high_c, true);
    format_temperature_text(low_temp, sizeof(low_temp), snapshot->weather_low_valid, snapshot->weather_low_c, true);

    epaper->setFont(Montserrat_Regular_26);
    BB_RECT now_rect = get_text_box(epaper, now_temp);
    draw_text_at(epaper, card_x + card_w - now_rect.w - 18, STANDBY_WEATHER_Y + 92, now_temp, true);

    char high_low[48];
    snprintf(high_low, sizeof(high_low), "%s / %s", hi_temp, low_temp);
    epaper->setFont(Montserrat_Regular_20);
    BB_RECT hl_rect = get_text_box(epaper, high_low);
    draw_text_at(epaper, card_x + card_w - hl_rect.w - 18, STANDBY_WEATHER_Y + 126, high_low);

    const uint8_t forecast_slots = MAX_STANDBY_FORECAST_DAYS;
    const int16_t forecast_row_y = STANDBY_WEATHER_Y + 170;
    const int16_t forecast_inner_w = card_w - 24;
    const int16_t slot_w = forecast_inner_w / forecast_slots;
    for (uint8_t idx = 0; idx < forecast_slots; idx++) {
        const int16_t slot_x = card_x + 12 + idx * slot_w;
        const int16_t slot_center_x = slot_x + slot_w / 2;
        const bool valid_day = idx < snapshot->forecast_day_count;
        const StandbyForecastDay* day = valid_day ? &snapshot->forecast_days[idx] : nullptr;

        char day_label[MAX_STANDBY_DAY_LABEL_LEN] = "--";
        if (day && day->day_label[0] != '\0') {
            ui_copy_string(day_label, sizeof(day_label), day->day_label);
        }

        epaper->setFont(Montserrat_Regular_20);
        ui_draw_centered_text(epaper, slot_center_x, forecast_row_y + 26, day_label, true);

        const uint8_t* day_icon = ui_weather_icon_for_condition(day ? day->condition : "");
        epaper->loadBMP(day_icon, slot_center_x - 32, forecast_row_y + 40, 0xf, BBEP_BLACK);

        char day_high[16];
        char day_low[16];
        format_temperature_text(day_high, sizeof(day_high), day && day->high_valid, day ? day->high_c : 0.0f, false);
        format_temperature_text(day_low, sizeof(day_low), day && day->low_valid, day ? day->low_c : 0.0f, false);

        epaper->setFont(Montserrat_Regular_26);
        ui_draw_centered_text(epaper, slot_center_x, forecast_row_y + 140, day_high, true);
        epaper->setFont(Montserrat_Regular_20);
        ui_draw_centered_text(epaper, slot_center_x, forecast_row_y + 176, day_low);
    }

    const int16_t energy_bottom = STANDBY_ENERGY_Y + STANDBY_ENERGY_H;
    const int16_t solar_cx = card_x + card_w / 2;
    const int16_t solar_cy = STANDBY_ENERGY_Y + 94;
    const int16_t grid_cx = card_x + 88;
    const int16_t grid_cy = STANDBY_ENERGY_Y + 262;
    const int16_t home_cx = card_x + card_w - 88;
    const int16_t home_cy = grid_cy;
    const int16_t battery_cx = solar_cx;
    const int16_t battery_cy = energy_bottom - 132;
    const int16_t node_r = 60;

    char solar_value[24];
    char home_value[24];
    char grid_in_value[24];
    char grid_out_value[24];
    char battery_out_value[24];
    char battery_in_value[24];
    char battery_soc_value[24];
    format_energy_text(solar_value, sizeof(solar_value), snapshot->solar_generation_valid, snapshot->solar_generation_kwh, false);
    format_energy_text(home_value, sizeof(home_value), snapshot->house_usage_valid, snapshot->house_usage_kwh, false);
    format_energy_text(grid_in_value, sizeof(grid_in_value), snapshot->grid_input_valid, snapshot->grid_input_kwh, false);
    format_energy_text(grid_out_value, sizeof(grid_out_value), snapshot->grid_export_valid, snapshot->grid_export_kwh, false);
    format_energy_text(battery_out_value, sizeof(battery_out_value), snapshot->battery_usage_valid, snapshot->battery_usage_kwh, false);
    format_energy_text(battery_in_value, sizeof(battery_in_value), snapshot->battery_charge_energy_valid, snapshot->battery_charge_energy_kwh, false);
    format_percent_text(battery_soc_value, sizeof(battery_soc_value), snapshot->battery_charge_valid, snapshot->battery_charge_pct);

    char grid_line1[32];
    char grid_line2[32];
    char battery_line1[32];
    char battery_line2[32];
    snprintf(grid_line1, sizeof(grid_line1), "In %s", grid_in_value);
    snprintf(grid_line2, sizeof(grid_line2), "Out %s", grid_out_value);
    snprintf(battery_line1, sizeof(battery_line1), "Out %s", battery_out_value);
    if (snapshot->battery_charge_valid) {
        snprintf(battery_line2, sizeof(battery_line2), "SoC %s%%", battery_soc_value);
    } else {
        snprintf(battery_line2, sizeof(battery_line2), "In %s", battery_in_value);
    }

    ui_draw_energy_node(epaper, solar_cx, solar_cy, node_r, climate_mode_heat, "Solar");
    ui_draw_energy_node(epaper, home_cx, home_cy, node_r, home_outline, "Home");
    ui_draw_energy_node(epaper, grid_cx, grid_cy, node_r, office_building, "Grid");
    ui_draw_energy_node(epaper, battery_cx, battery_cy, node_r, climate_mode_cool, "Battery");

    const int16_t value_y = node_r + 24;
    const int16_t value_y2 = node_r + 46;
    epaper->setFont(Montserrat_Regular_16);
    ui_draw_centered_text(epaper, solar_cx, solar_cy + value_y, solar_value, true);
    ui_draw_centered_text(epaper, home_cx, home_cy + value_y, home_value, true);
    ui_draw_centered_text(epaper, grid_cx, grid_cy + value_y, grid_line1, true);
    ui_draw_centered_text(epaper, grid_cx, grid_cy + value_y2, grid_line2, false);
    ui_draw_centered_text(epaper, battery_cx, battery_cy + value_y, battery_line1, true);
    ui_draw_centered_text(epaper, battery_cx, battery_cy + value_y2, battery_line2, false);
}

static uint16_t ui_room_controls_light_height_for_counts(uint8_t full_row_count, uint32_t full_row_height_total, uint8_t light_count) {
    const uint8_t light_rows = static_cast<uint8_t>((light_count + 1) / 2);
    if (light_rows == 0) {
        return ROOM_CONTROLS_LIGHT_HEIGHT;
    }

    const uint8_t total_rows = static_cast<uint8_t>(full_row_count + light_rows);
    const int32_t display_bottom = static_cast<int32_t>(DISPLAY_HEIGHT) - ROOM_CONTROLS_BOTTOM_PADDING;
    const int32_t available_height = display_bottom - ROOM_CONTROLS_ITEM_START_Y;
    const int32_t total_gap_height = total_rows > 1 ? static_cast<int32_t>(total_rows - 1) * ROOM_CONTROLS_ITEM_GAP : 0;
    const int32_t available_light_height = available_height - total_gap_height - static_cast<int32_t>(full_row_height_total);

    int32_t candidate = ROOM_CONTROLS_LIGHT_MIN_HEIGHT;
    if (available_light_height > 0) {
        candidate = available_light_height / light_rows;
    }
    if (candidate < ROOM_CONTROLS_LIGHT_MIN_HEIGHT) {
        candidate = ROOM_CONTROLS_LIGHT_MIN_HEIGHT;
    } else if (candidate > ROOM_CONTROLS_LIGHT_HEIGHT) {
        candidate = ROOM_CONTROLS_LIGHT_HEIGHT;
    }

    return static_cast<uint16_t>(candidate);
}

bool ui_build_room_controls(Screen* screen,
                            const RoomControlsSnapshot* snapshot,
                            uint8_t requested_page,
                            uint8_t* page_count,
                            bool* geometry_truncated) {
    *geometry_truncated = snapshot->truncated;
    *page_count = 1;
    screen_clear(screen);

    const uint16_t full_width = static_cast<uint16_t>(DISPLAY_WIDTH - 2 * ROOM_CONTROLS_ITEM_X);
    const uint16_t light_width = static_cast<uint16_t>((full_width - ROOM_CONTROLS_LIGHT_COLUMN_GAP) / 2);
    const uint16_t packing_light_height = ROOM_CONTROLS_LIGHT_MIN_HEIGHT;
    const uint16_t display_bottom = DISPLAY_HEIGHT - ROOM_CONTROLS_BOTTOM_PADDING;
    uint8_t entity_pages[MAX_ENTITIES] = {};

    uint8_t current_page = 0;
    uint16_t pos_y = ROOM_CONTROLS_ITEM_START_Y;
    uint8_t light_col = 0;
    bool impossible_geometry = false;

    auto start_new_page = [&]() {
        if (current_page < 254) {
            current_page++;
        }
        *page_count = current_page + 1;
        pos_y = ROOM_CONTROLS_ITEM_START_Y;
        light_col = 0;
    };

    auto place_entity = [&](uint8_t idx) {
        const CommandType entity_type = snapshot->entity_types[idx];
        const bool is_cover = entity_type == CommandType::SetCoverOpenClose;
        const bool is_climate = entity_type == CommandType::SetClimateModeAndTemperature;
        const bool is_light = !is_climate && !is_cover;
        const uint16_t full_row_height = is_climate ? ROOM_CONTROLS_CLIMATE_HEIGHT : ROOM_CONTROLS_COVER_HEIGHT;
        while (true) {
            if (!is_light) {
                uint16_t row_y = pos_y;
                if (light_col != 0) {
                    row_y = static_cast<uint16_t>(row_y + packing_light_height + ROOM_CONTROLS_ITEM_GAP);
                }

                if (row_y + full_row_height <= display_bottom) {
                    entity_pages[idx] = current_page;
                    pos_y = static_cast<uint16_t>(row_y + full_row_height + ROOM_CONTROLS_ITEM_GAP);
                    light_col = 0;
                    return;
                }

                if (row_y == ROOM_CONTROLS_ITEM_START_Y && light_col == 0) {
                    impossible_geometry = true;
                    *geometry_truncated = true;
                    return;
                }
                start_new_page();
            } else {
                if (pos_y + packing_light_height <= display_bottom) {
                    entity_pages[idx] = current_page;

                    if (light_col == 0) {
                        light_col = 1;
                    } else {
                        light_col = 0;
                        pos_y = static_cast<uint16_t>(pos_y + packing_light_height + ROOM_CONTROLS_ITEM_GAP);
                    }
                    return;
                }

                if (pos_y == ROOM_CONTROLS_ITEM_START_Y && light_col == 0) {
                    impossible_geometry = true;
                    *geometry_truncated = true;
                    return;
                }
                start_new_page();
            }
        }
    };

    for (uint8_t idx = 0; idx < snapshot->entity_count; idx++) {
        place_entity(idx);
        if (impossible_geometry) {
            break;
        }
    }

    *page_count = current_page + 1;
    uint8_t target_page = requested_page;
    if (target_page >= *page_count && *page_count > 0) {
        target_page = static_cast<uint8_t>(*page_count - 1);
    }

    uint8_t target_full_row_count = 0;
    uint32_t target_full_row_height_total = 0;
    uint8_t target_light_count = 0;
    for (uint8_t idx = 0; idx < snapshot->entity_count; idx++) {
        if (entity_pages[idx] != target_page) {
            continue;
        }
        const CommandType entity_type = snapshot->entity_types[idx];
        if (entity_type == CommandType::SetClimateModeAndTemperature) {
            target_full_row_count++;
            target_full_row_height_total += ROOM_CONTROLS_CLIMATE_HEIGHT;
        } else if (entity_type == CommandType::SetCoverOpenClose) {
            target_full_row_count++;
            target_full_row_height_total += ROOM_CONTROLS_COVER_HEIGHT;
        } else {
            target_light_count++;
        }
    }
    const uint16_t light_height = ui_room_controls_light_height_for_counts(target_full_row_count, target_full_row_height_total, target_light_count);

    uint16_t draw_y = ROOM_CONTROLS_ITEM_START_Y;
    uint8_t draw_light_col = 0;
    for (uint8_t idx = 0; idx < snapshot->entity_count; idx++) {
        if (entity_pages[idx] != target_page) {
            continue;
        }

        const CommandType entity_type = snapshot->entity_types[idx];
        const bool is_cover = entity_type == CommandType::SetCoverOpenClose;
        const bool is_climate = entity_type == CommandType::SetClimateModeAndTemperature;
        const bool is_light = !is_climate && !is_cover;
        if (!is_light) {
            if (draw_light_col != 0) {
                draw_y = static_cast<uint16_t>(draw_y + light_height + ROOM_CONTROLS_ITEM_GAP);
                draw_light_col = 0;
            }
            const uint16_t full_row_height = is_climate ? ROOM_CONTROLS_CLIMATE_HEIGHT : ROOM_CONTROLS_COVER_HEIGHT;
            if (draw_y + full_row_height > display_bottom) {
                *geometry_truncated = true;
                break;
            }

            if (is_climate) {
                screen_add_climate(
                    ClimateConfig{
                        .entity_ref = EntityRef{.index = snapshot->entity_ids[idx]},
                        .label = snapshot->entity_names[idx],
                        .climate_mode_mask = snapshot->entity_climate_mode_masks[idx],
                        .pos_x = ROOM_CONTROLS_ITEM_X,
                        .pos_y = draw_y,
                        .width = full_width,
                        .height = ROOM_CONTROLS_CLIMATE_HEIGHT,
                    },
                    screen);
            } else {
                screen_add_cover(
                    CoverConfig{
                        .entity_ref = EntityRef{.index = snapshot->entity_ids[idx]},
                        .label = snapshot->entity_names[idx],
                        .pos_x = ROOM_CONTROLS_ITEM_X,
                        .pos_y = draw_y,
                        .width = full_width,
                        .height = ROOM_CONTROLS_COVER_HEIGHT,
                    },
                    screen);
            }
            draw_y = static_cast<uint16_t>(draw_y + full_row_height + ROOM_CONTROLS_ITEM_GAP);
        } else {
            if (draw_y + light_height > display_bottom) {
                *geometry_truncated = true;
                break;
            }

            screen_add_button(
                ButtonConfig{
                    .entity_ref = EntityRef{.index = snapshot->entity_ids[idx]},
                    .label = snapshot->entity_names[idx],
                    .icon_on = lightbulb_outline,
                    .icon_off = lightbulb_off_outline,
                    .pos_x = static_cast<uint16_t>(ROOM_CONTROLS_ITEM_X + draw_light_col * (light_width + ROOM_CONTROLS_LIGHT_COLUMN_GAP)),
                    .pos_y = draw_y,
                    .width = light_width,
                    .height = light_height,
                },
                screen);

            if (draw_light_col == 0) {
                draw_light_col = 1;
            } else {
                draw_light_col = 0;
                draw_y = static_cast<uint16_t>(draw_y + light_height + ROOM_CONTROLS_ITEM_GAP);
            }
        }
    }

    return screen->widget_count > 0 || snapshot->entity_count == 0;
}

void ui_draw_room_controls_header(FASTEPD* epaper, const char* room_name, uint8_t room_controls_page, uint8_t room_controls_page_count, bool truncated) {
    epaper->setFont(Montserrat_Regular_20);
    epaper->setTextColor(BBEP_BLACK);

    epaper->fillRect(0, 0, DISPLAY_WIDTH, ROOM_CONTROLS_HEADER_HEIGHT, 0xe);
    ui_draw_back_button(epaper);

    char room_label[MAX_ROOM_NAME_LEN];
    strncpy(room_label, room_name ? room_name : "", sizeof(room_label) - 1);
    room_label[sizeof(room_label) - 1] = '\0';
    truncate_with_ellipsis(epaper, room_label, sizeof(room_label), DISPLAY_WIDTH - (ROOM_CONTROLS_BACK_X + ROOM_CONTROLS_BACK_W + 32) - 8);
    draw_text_at(epaper, ROOM_CONTROLS_BACK_X + ROOM_CONTROLS_BACK_W + 32, ROOM_CONTROLS_BACK_Y + 30, room_label, true);

    epaper->setFont(Montserrat_Regular_16);
    draw_text_at(epaper, ROOM_CONTROLS_BACK_X + ROOM_CONTROLS_BACK_W + 32, ROOM_CONTROLS_BACK_Y + 56, "Controls", true);

    if (room_controls_page_count > 1) {
        char page_text[20];
        snprintf(page_text, sizeof(page_text), "Page %u/%u", static_cast<unsigned>(room_controls_page + 1),
                 static_cast<unsigned>(room_controls_page_count));
        BB_RECT page_rect = get_text_box(epaper, page_text);
        const int16_t badge_w = page_rect.w + 20;
        const int16_t badge_x = DISPLAY_WIDTH - ROOM_CONTROLS_ITEM_X - badge_w;
        const int16_t badge_y = ROOM_CONTROLS_BACK_Y + 36;

        epaper->fillRoundRect(badge_x, badge_y, badge_w, 26, 10, 0xe);
        epaper->drawRoundRect(badge_x, badge_y, badge_w, 26, 10, BBEP_BLACK);
        draw_text_at(epaper, badge_x + 10, badge_y + 18, page_text, true);
    }

    epaper->drawLine(0, ROOM_CONTROLS_HEADER_HEIGHT, DISPLAY_WIDTH, ROOM_CONTROLS_HEADER_HEIGHT, BBEP_BLACK);

    if (truncated) {
        epaper->setFont(Montserrat_Regular_16);
        draw_text_at(epaper, ROOM_CONTROLS_ITEM_X, DISPLAY_HEIGHT - 20, "Some controls could not be displayed", true);
    }
}

void ui_task(void* arg) {
    UITaskArgs* ctx = static_cast<UITaskArgs*>(arg);
    UIState current_state = {};
    UIState displayed_state = {};
    bool display_is_dirty = false;
    static FloorListSnapshot floor_list_snapshot;
    static RoomListSnapshot room_list_snapshot;
    static RoomControlsSnapshot room_controls_snapshot;
    static StandbySnapshot standby_snapshot;
    static WifiSettingsSnapshot wifi_settings_snapshot;
    static WifiPasswordSnapshot wifi_password_snapshot;
    bool room_controls_truncated = false;
    uint8_t room_controls_page_count = 1;

    memset(&floor_list_snapshot, 0, sizeof(floor_list_snapshot));
    memset(&room_list_snapshot, 0, sizeof(room_list_snapshot));
    memset(&room_controls_snapshot, 0, sizeof(room_controls_snapshot));
    memset(&standby_snapshot, 0, sizeof(standby_snapshot));
    memset(&wifi_settings_snapshot, 0, sizeof(wifi_settings_snapshot));
    memset(&wifi_password_snapshot, 0, sizeof(wifi_password_snapshot));

    xTaskNotifyGive(xTaskGetCurrentTaskHandle()); // First refresh needs a notification

    while (1) {
        TickType_t notify_timeout = portMAX_DELAY;
        if (display_is_dirty) {
            notify_timeout = pdMS_TO_TICKS(DISPLAY_FULL_REDRAW_TIMEOUT_MS);
        }

        if (ulTaskNotifyTake(pdTRUE, notify_timeout)) {
            store_update_ui_state(ctx->store, ctx->screen, &current_state);

            const bool mode_changed = current_state.mode != displayed_state.mode;
            const bool floor_changed = current_state.selected_floor != displayed_state.selected_floor;
            const bool room_changed = current_state.selected_room != displayed_state.selected_room;
            const bool rooms_changed = current_state.rooms_revision != displayed_state.rooms_revision;
            const bool floor_list_page_changed = current_state.floor_list_page != displayed_state.floor_list_page;
            const bool room_list_page_changed = current_state.room_list_page != displayed_state.room_list_page;
            const bool room_controls_page_changed = current_state.room_controls_page != displayed_state.room_controls_page;
            const bool settings_changed = current_state.settings_revision != displayed_state.settings_revision;
            const bool standby_changed = current_state.standby_revision != displayed_state.standby_revision;

            if (current_state.mode == UiMode::RoomControls && (mode_changed || room_changed || room_controls_page_changed || rooms_changed)) {
                if (store_get_room_controls_snapshot(ctx->store, current_state.selected_room, &room_controls_snapshot)) {
                    ui_build_room_controls(ctx->screen, &room_controls_snapshot, current_state.room_controls_page, &room_controls_page_count,
                                           &room_controls_truncated);
                    store_update_ui_state(ctx->store, ctx->screen, &current_state);
                } else {
                    current_state.mode = UiMode::GenericError;
                }
            } else if (current_state.mode != UiMode::RoomControls && mode_changed) {
                screen_clear(ctx->screen);
            }

            if (current_state.mode == UiMode::Standby && (mode_changed || standby_changed)) {
                store_get_standby_snapshot(ctx->store, &standby_snapshot);
                ctx->epaper->setMode(BB_MODE_4BPP);
                ui_draw_standby(ctx->epaper, &standby_snapshot);
                ctx->epaper->fullUpdate(CLEAR_FAST, true);
                display_is_dirty = false;
            } else if (current_state.mode == UiMode::SettingsMenu && (mode_changed || settings_changed)) {
                ctx->epaper->setMode(BB_MODE_4BPP);
                ctx->epaper->fillScreen(0xf);
                ui_draw_settings_menu(ctx->epaper);
                ctx->epaper->fullUpdate(CLEAR_FAST, true);
                display_is_dirty = false;
            } else if (current_state.mode == UiMode::WifiSettings && (mode_changed || settings_changed)) {
                store_get_wifi_settings_snapshot(ctx->store, &wifi_settings_snapshot);
                ctx->epaper->setMode(BB_MODE_4BPP);
                ctx->epaper->fillScreen(0xf);
                ui_draw_wifi_settings(ctx->epaper, &wifi_settings_snapshot);
                ctx->epaper->fullUpdate(CLEAR_FAST, true);
                display_is_dirty = false;
            } else if (current_state.mode == UiMode::WifiPassword && (mode_changed || settings_changed)) {
                if (!store_get_wifi_password_snapshot(ctx->store, &wifi_password_snapshot)) {
                    current_state.mode = UiMode::GenericError;
                    ctx->epaper->setMode(BB_MODE_4BPP);
                    ctx->epaper->fillScreen(0xf);
                    ui_show_message(current_state.mode, ctx->epaper);
                    ctx->epaper->fullUpdate(CLEAR_FAST, true);
                } else {
                    ctx->epaper->setMode(BB_MODE_4BPP);
                    ctx->epaper->fillScreen(0xf);
                    ui_draw_wifi_password(ctx->epaper, &wifi_password_snapshot);
                    ctx->epaper->fullUpdate(CLEAR_FAST, true);
                }
                display_is_dirty = false;
            } else if (current_state.mode == UiMode::FloorList && (mode_changed || rooms_changed || floor_list_page_changed)) {
                store_get_floor_list_snapshot(ctx->store, &floor_list_snapshot);

                ctx->epaper->setMode(BB_MODE_4BPP);
                ctx->epaper->fillScreen(0xf);
                ui_draw_floor_list(ctx->epaper, &floor_list_snapshot, current_state.floor_list_page);
                ctx->epaper->fullUpdate(CLEAR_FAST, true);
                display_is_dirty = false;
            } else if (current_state.mode == UiMode::RoomList && (mode_changed || rooms_changed || floor_changed || room_list_page_changed)) {
                if (!store_get_room_list_snapshot(ctx->store, current_state.selected_floor, &room_list_snapshot)) {
                    current_state.mode = UiMode::GenericError;
                    ctx->epaper->setMode(BB_MODE_4BPP);
                    ctx->epaper->fillScreen(0xf);
                    ui_show_message(current_state.mode, ctx->epaper);
                    ctx->epaper->fullUpdate(CLEAR_FAST, true);
                    display_is_dirty = false;
                } else {
                    ctx->epaper->setMode(BB_MODE_4BPP);
                    ctx->epaper->fillScreen(0xf);
                    ui_draw_room_list(ctx->epaper, &room_list_snapshot, current_state.room_list_page);
                    ctx->epaper->fullUpdate(CLEAR_FAST, true);
                    display_is_dirty = false;
                }
            } else if (current_state.mode == UiMode::RoomControls &&
                       (mode_changed || room_changed || room_controls_page_changed || rooms_changed)) {
                ctx->epaper->setMode(BB_MODE_4BPP);
                ctx->epaper->fillScreen(0xf);
                ui_draw_room_controls_header(ctx->epaper, room_controls_snapshot.room_name, current_state.room_controls_page,
                                             room_controls_page_count, room_controls_truncated);
                ui_room_controls_draw_widgets(&current_state, BitDepth::BD_4BPP, ctx->screen, ctx->epaper);
                ctx->epaper->fullUpdate(CLEAR_FAST, true);

                ctx->epaper->setMode(BB_MODE_1BPP);
                ctx->epaper->fillScreen(BBEP_WHITE);
                ui_draw_room_controls_header(ctx->epaper, room_controls_snapshot.room_name, current_state.room_controls_page,
                                             room_controls_page_count, room_controls_truncated);
                ui_room_controls_draw_widgets(&current_state, BitDepth::BD_1BPP, ctx->screen, ctx->epaper);
                ctx->epaper->backupPlane();
                display_is_dirty = false;
            } else if (current_state.mode == UiMode::RoomControls) {
                Rect damage_accum = {};

                for (uint8_t widget_idx = 0; widget_idx < ctx->screen->widget_count; widget_idx++) {
                    uint8_t displayed_value = displayed_state.widget_values[widget_idx];
                    uint8_t current_value = current_state.widget_values[widget_idx];

                    if (displayed_value != current_value) {
                        Rect damage = ctx->screen->widgets[widget_idx]->partialDraw(ctx->epaper, BitDepth::BD_1BPP, displayed_value,
                                                                                    current_value);
                        accumulate_damage(damage_accum, damage);
                    }
                }

                if (damage_accum.w > 0 || damage_accum.h > 0) {
                    ctx->epaper->partialUpdate(true,
                                               DISPLAY_WIDTH - (damage_accum.x + damage_accum.w), // row start (reversed)
                                               DISPLAY_WIDTH - damage_accum.x                     // row end (reversed)
                    );
                    display_is_dirty = true;
                }
            } else if (mode_changed) {
                ctx->epaper->setMode(BB_MODE_4BPP);
                ctx->epaper->fillScreen(0xf);
                ui_show_message(current_state.mode, ctx->epaper);
                ctx->epaper->fullUpdate(CLEAR_FAST, true);
                display_is_dirty = false;
            }

            displayed_state = current_state;
            ui_state_set(ctx->shared_state, &displayed_state);
        } else if (display_is_dirty && displayed_state.mode == UiMode::RoomControls) {
            ESP_LOGI(TAG, "Forcing a full refresh of the display");

            ctx->epaper->setMode(BB_MODE_4BPP);
            ctx->epaper->fillScreen(0xf);
            ui_draw_room_controls_header(ctx->epaper, room_controls_snapshot.room_name, displayed_state.room_controls_page,
                                         room_controls_page_count, room_controls_truncated);
            ui_room_controls_draw_widgets(&displayed_state, BitDepth::BD_4BPP, ctx->screen, ctx->epaper);
            ctx->epaper->fullUpdate(CLEAR_FAST, true);

            ctx->epaper->setMode(BB_MODE_1BPP);
            ctx->epaper->fillScreen(BBEP_WHITE);
            ui_draw_room_controls_header(ctx->epaper, room_controls_snapshot.room_name, displayed_state.room_controls_page,
                                         room_controls_page_count, room_controls_truncated);
            ui_room_controls_draw_widgets(&displayed_state, BitDepth::BD_1BPP, ctx->screen, ctx->epaper);
            ctx->epaper->backupPlane();

            display_is_dirty = false;
        }
    }
}
