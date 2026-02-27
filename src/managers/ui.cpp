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
#include <cstdio>
#include <cstring>

static const char* TAG = "ui";
static const char* const TEXT_BOOT[] = {"Home Assistant", "e-paper remote", nullptr};
static const char* const TEXT_WIFI_DISCONNECTED[] = {"Not connected", "to Wifi", nullptr};
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
    epaper->setCursor(label_x + (label_w - rect1.w) / 2, top);
    epaper->write(line1);

    if (two_lines) {
        epaper->setCursor(label_x + (label_w - rect2.w) / 2, top + rect1.h + gap);
        epaper->write(line2);
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

static uint8_t list_page_count(uint8_t item_count) {
    if (item_count == 0) {
        return 1;
    }
    return static_cast<uint8_t>((item_count + ROOM_LIST_ROOMS_PER_PAGE - 1) / ROOM_LIST_ROOMS_PER_PAGE);
}

static void ui_draw_name_grid(FASTEPD* epaper, const char names[][MAX_ROOM_NAME_LEN], const char icons[][MAX_ICON_NAME_LEN], uint8_t item_count,
                              uint8_t list_page, uint16_t grid_start_y) {
    const uint8_t total_pages = list_page_count(item_count);
    const uint8_t page = std::min(list_page, static_cast<uint8_t>(total_pages - 1));
    const uint8_t first_idx = page * ROOM_LIST_ROOMS_PER_PAGE;
    const uint8_t last_idx = std::min<uint8_t>(item_count, first_idx + ROOM_LIST_ROOMS_PER_PAGE);

    const int16_t grid_w = DISPLAY_WIDTH - 2 * ROOM_LIST_GRID_MARGIN_X;
    const int16_t grid_h = ROOM_LIST_GRID_BOTTOM_Y - grid_start_y;
    const int16_t tile_w = (grid_w - (ROOM_LIST_COLUMNS - 1) * ROOM_LIST_GRID_GAP_X) / ROOM_LIST_COLUMNS;
    const int16_t tile_h = (grid_h - (ROOM_LIST_ROWS - 1) * ROOM_LIST_GRID_GAP_Y) / ROOM_LIST_ROWS;

    for (uint8_t idx = first_idx; idx < last_idx; idx++) {
        const uint8_t slot = idx - first_idx;
        const uint8_t row = slot / ROOM_LIST_COLUMNS;
        const uint8_t col = slot % ROOM_LIST_COLUMNS;
        const int16_t tile_x = ROOM_LIST_GRID_MARGIN_X + col * (tile_w + ROOM_LIST_GRID_GAP_X);
        const int16_t tile_y = grid_start_y + row * (tile_h + ROOM_LIST_GRID_GAP_Y);

        epaper->drawRect(tile_x, tile_y, tile_w, tile_h, BBEP_BLACK);
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
        const int16_t label_width = label_rect.w;
        epaper->setCursor(DISPLAY_WIDTH - ROOM_LIST_GRID_MARGIN_X - label_width, ROOM_LIST_FOOTER_Y);
        epaper->write(page_text);
    }
}

void ui_draw_floor_list(FASTEPD* epaper, const FloorListSnapshot* snapshot, uint8_t floor_list_page) {
    epaper->setTextColor(BBEP_BLACK);

    if (snapshot->floor_count == 0) {
        epaper->setFont(Montserrat_Regular_26);
        epaper->setCursor(ROOM_LIST_GRID_MARGIN_X, FLOOR_LIST_GRID_START_Y + 40);
        epaper->write("No floors found");
        return;
    }

    ui_draw_name_grid(epaper, snapshot->floor_names, snapshot->floor_icons, snapshot->floor_count, floor_list_page, FLOOR_LIST_GRID_START_Y);
}

void ui_draw_room_list_header(FASTEPD* epaper, const char* floor_name) {
    epaper->drawRect(ROOM_CONTROLS_BACK_X, ROOM_CONTROLS_BACK_Y, ROOM_CONTROLS_BACK_W, ROOM_CONTROLS_BACK_H, BBEP_BLACK);
    ui_draw_back_icon(epaper);

    epaper->setFont(Montserrat_Regular_20);
    char floor_label[MAX_FLOOR_NAME_LEN];
    strncpy(floor_label, floor_name ? floor_name : "", sizeof(floor_label) - 1);
    floor_label[sizeof(floor_label) - 1] = '\0';
    truncate_with_ellipsis(epaper, floor_label, sizeof(floor_label), DISPLAY_WIDTH - (ROOM_CONTROLS_BACK_X + ROOM_CONTROLS_BACK_W + 32) - 8);
    epaper->setCursor(ROOM_CONTROLS_BACK_X + ROOM_CONTROLS_BACK_W + 32, ROOM_CONTROLS_BACK_Y + 36);
    epaper->write(floor_label);

    epaper->drawLine(0, ROOM_LIST_HEADER_HEIGHT, DISPLAY_WIDTH, ROOM_LIST_HEADER_HEIGHT, BBEP_BLACK);
}

void ui_draw_room_list(FASTEPD* epaper, const RoomListSnapshot* snapshot, uint8_t room_list_page) {
    epaper->setTextColor(BBEP_BLACK);
    ui_draw_room_list_header(epaper, snapshot->floor_name);

    if (snapshot->room_count == 0) {
        epaper->setFont(Montserrat_Regular_26);
        epaper->setCursor(ROOM_LIST_GRID_MARGIN_X, ROOM_LIST_GRID_START_Y + 40);
        epaper->write("No rooms found");
        return;
    }

    ui_draw_name_grid(epaper, snapshot->room_names, snapshot->room_icons, snapshot->room_count, room_list_page, ROOM_LIST_GRID_START_Y);
}

bool ui_build_room_controls(Screen* screen, const RoomControlsSnapshot* snapshot, bool* geometry_truncated) {
    *geometry_truncated = snapshot->truncated;
    screen_clear(screen);

    const uint16_t full_width = static_cast<uint16_t>(DISPLAY_WIDTH - 2 * ROOM_CONTROLS_ITEM_X);
    const uint16_t light_width =
        static_cast<uint16_t>((full_width - ROOM_CONTROLS_LIGHT_COLUMN_GAP) / 2);

    uint16_t pos_y = ROOM_CONTROLS_ITEM_START_Y;
    uint8_t light_col = 0;
    for (uint8_t idx = 0; idx < snapshot->entity_count; idx++) {
        const bool is_climate = snapshot->entity_types[idx] == CommandType::SetClimateModeAndTemperature;

        if (is_climate) {
            if (light_col != 0) {
                pos_y += ROOM_CONTROLS_LIGHT_HEIGHT + ROOM_CONTROLS_ITEM_GAP;
                light_col = 0;
            }

            if (pos_y + ROOM_CONTROLS_CLIMATE_HEIGHT > DISPLAY_HEIGHT) {
                *geometry_truncated = true;
                break;
            }

            screen_add_climate(
                ClimateConfig{
                    .entity_ref = EntityRef{.index = snapshot->entity_ids[idx]},
                    .label = snapshot->entity_names[idx],
                    .climate_mode_mask = snapshot->entity_climate_mode_masks[idx],
                    .pos_x = ROOM_CONTROLS_ITEM_X,
                    .pos_y = pos_y,
                    .width = full_width,
                    .height = ROOM_CONTROLS_CLIMATE_HEIGHT,
                },
                screen);

            pos_y += ROOM_CONTROLS_CLIMATE_HEIGHT + ROOM_CONTROLS_ITEM_GAP;
        } else {
            if (pos_y + ROOM_CONTROLS_LIGHT_HEIGHT > DISPLAY_HEIGHT) {
                *geometry_truncated = true;
                break;
            }

            screen_add_button(
                ButtonConfig{
                    .entity_ref = EntityRef{.index = snapshot->entity_ids[idx]},
                    .label = snapshot->entity_names[idx],
                    .icon_on = lightbulb_outline,
                    .icon_off = lightbulb_off_outline,
                    .pos_x = static_cast<uint16_t>(ROOM_CONTROLS_ITEM_X + light_col * (light_width + ROOM_CONTROLS_LIGHT_COLUMN_GAP)),
                    .pos_y = pos_y,
                    .width = light_width,
                    .height = ROOM_CONTROLS_LIGHT_HEIGHT,
                },
                screen);

            if (light_col == 0) {
                light_col = 1;
            } else {
                light_col = 0;
                pos_y += ROOM_CONTROLS_LIGHT_HEIGHT + ROOM_CONTROLS_ITEM_GAP;
            }
        }
    }

    return screen->widget_count > 0 || snapshot->entity_count == 0;
}

void ui_draw_room_controls_header(FASTEPD* epaper, const char* room_name, bool truncated) {
    epaper->setFont(Montserrat_Regular_20);
    epaper->setTextColor(BBEP_BLACK);

    epaper->drawRect(ROOM_CONTROLS_BACK_X, ROOM_CONTROLS_BACK_Y, ROOM_CONTROLS_BACK_W, ROOM_CONTROLS_BACK_H, BBEP_BLACK);
    ui_draw_back_icon(epaper);

    char room_label[MAX_ROOM_NAME_LEN];
    strncpy(room_label, room_name ? room_name : "", sizeof(room_label) - 1);
    room_label[sizeof(room_label) - 1] = '\0';
    truncate_with_ellipsis(epaper, room_label, sizeof(room_label), DISPLAY_WIDTH - (ROOM_CONTROLS_BACK_X + ROOM_CONTROLS_BACK_W + 32) - 8);
    epaper->setCursor(ROOM_CONTROLS_BACK_X + ROOM_CONTROLS_BACK_W + 32, ROOM_CONTROLS_BACK_Y + 36);
    epaper->write(room_label);

    epaper->drawLine(0, ROOM_CONTROLS_HEADER_HEIGHT, DISPLAY_WIDTH, ROOM_CONTROLS_HEADER_HEIGHT, BBEP_BLACK);

    if (truncated) {
        epaper->setCursor(ROOM_CONTROLS_ITEM_X, DISPLAY_HEIGHT - 20);
        epaper->write("Too many controls for one page");
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
    bool room_controls_truncated = false;

    memset(&floor_list_snapshot, 0, sizeof(floor_list_snapshot));
    memset(&room_list_snapshot, 0, sizeof(room_list_snapshot));
    memset(&room_controls_snapshot, 0, sizeof(room_controls_snapshot));

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

            if (current_state.mode == UiMode::RoomControls && (mode_changed || room_changed)) {
                if (store_get_room_controls_snapshot(ctx->store, current_state.selected_room, &room_controls_snapshot)) {
                    ui_build_room_controls(ctx->screen, &room_controls_snapshot, &room_controls_truncated);
                    store_update_ui_state(ctx->store, ctx->screen, &current_state);
                } else {
                    current_state.mode = UiMode::GenericError;
                }
            } else if (current_state.mode != UiMode::RoomControls && mode_changed) {
                screen_clear(ctx->screen);
            }

            if (current_state.mode == UiMode::FloorList && (mode_changed || rooms_changed || floor_list_page_changed)) {
                store_get_floor_list_snapshot(ctx->store, &floor_list_snapshot);

                ctx->epaper->setMode(BB_MODE_4BPP);
                ctx->epaper->fillScreen(0xf);
                ui_draw_floor_list(ctx->epaper, &floor_list_snapshot, current_state.floor_list_page);
                ctx->epaper->fullUpdate(CLEAR_SLOW, true);
                display_is_dirty = false;
            } else if (current_state.mode == UiMode::RoomList && (mode_changed || rooms_changed || floor_changed || room_list_page_changed)) {
                if (!store_get_room_list_snapshot(ctx->store, current_state.selected_floor, &room_list_snapshot)) {
                    current_state.mode = UiMode::GenericError;
                    ctx->epaper->setMode(BB_MODE_4BPP);
                    ctx->epaper->fillScreen(0xf);
                    ui_show_message(current_state.mode, ctx->epaper);
                    ctx->epaper->fullUpdate(CLEAR_SLOW, true);
                    display_is_dirty = false;
                } else {
                    ctx->epaper->setMode(BB_MODE_4BPP);
                    ctx->epaper->fillScreen(0xf);
                    ui_draw_room_list(ctx->epaper, &room_list_snapshot, current_state.room_list_page);
                    ctx->epaper->fullUpdate(CLEAR_SLOW, true);
                    display_is_dirty = false;
                }
            } else if (current_state.mode == UiMode::RoomControls && (mode_changed || room_changed)) {
                ctx->epaper->setMode(BB_MODE_4BPP);
                ctx->epaper->fillScreen(0xf);
                ui_draw_room_controls_header(ctx->epaper, room_controls_snapshot.room_name, room_controls_truncated);
                ui_room_controls_draw_widgets(&current_state, BitDepth::BD_4BPP, ctx->screen, ctx->epaper);
                ctx->epaper->fullUpdate(CLEAR_SLOW, true);

                ctx->epaper->setMode(BB_MODE_1BPP);
                ctx->epaper->fillScreen(BBEP_WHITE);
                ui_draw_room_controls_header(ctx->epaper, room_controls_snapshot.room_name, room_controls_truncated);
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
                ctx->epaper->fullUpdate(CLEAR_SLOW, true);
                display_is_dirty = false;
            }

            displayed_state = current_state;
            ui_state_set(ctx->shared_state, &displayed_state);
        } else if (display_is_dirty && displayed_state.mode == UiMode::RoomControls) {
            ESP_LOGI(TAG, "Forcing a full refresh of the display");

            ctx->epaper->setMode(BB_MODE_4BPP);
            ctx->epaper->fillScreen(0xf);
            ui_draw_room_controls_header(ctx->epaper, room_controls_snapshot.room_name, room_controls_truncated);
            ui_room_controls_draw_widgets(&displayed_state, BitDepth::BD_4BPP, ctx->screen, ctx->epaper);
            ctx->epaper->fullUpdate(CLEAR_FAST, true);

            ctx->epaper->setMode(BB_MODE_1BPP);
            ctx->epaper->fillScreen(BBEP_WHITE);
            ui_draw_room_controls_header(ctx->epaper, room_controls_snapshot.room_name, room_controls_truncated);
            ui_room_controls_draw_widgets(&displayed_state, BitDepth::BD_1BPP, ctx->screen, ctx->epaper);
            ctx->epaper->backupPlane();

            display_is_dirty = false;
        }
    }
}
