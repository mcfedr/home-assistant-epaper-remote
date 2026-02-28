#include "managers/touch.h"
#include "boards.h"
#include "constants.h"
#include <Wire.h>

static const char* TAG = "touch";

static bool i2c_read_reg8(uint8_t addr, uint8_t reg, uint8_t* buf, size_t len) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }

    if (Wire.requestFrom(static_cast<int>(addr), static_cast<int>(len)) != static_cast<int>(len)) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        buf[i] = Wire.read();
    }
    return true;
}

static bool i2c_device_present(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

static bool i2c_read_reg16(uint8_t addr, uint16_t reg, uint8_t* buf, size_t len) {
    Wire.beginTransmission(addr);
    Wire.write(static_cast<uint8_t>(reg >> 8));
    Wire.write(static_cast<uint8_t>(reg & 0xFF));
    if (Wire.endTransmission(false) != 0) {
        return false;
    }

    if (Wire.requestFrom(static_cast<int>(addr), static_cast<int>(len)) != static_cast<int>(len)) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        buf[i] = Wire.read();
    }
    return true;
}

static bool i2c_write_reg16(uint8_t addr, uint16_t reg, uint8_t value) {
    Wire.beginTransmission(addr);
    Wire.write(static_cast<uint8_t>(reg >> 8));
    Wire.write(static_cast<uint8_t>(reg & 0xFF));
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

static bool i2c_read_reg16_block(uint8_t addr, uint16_t reg, uint8_t* buf, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > 16) {
            chunk = 16;
        }
        if (!i2c_read_reg16(addr, reg + offset, buf + offset, chunk)) {
            return false;
        }
        offset += chunk;
    }
    return true;
}

static bool detect_gt911_address(uint8_t* out_addr) {
    if (i2c_device_present(GT911_ADDR1)) {
        *out_addr = GT911_ADDR1;
        return true;
    }
    if (i2c_device_present(GT911_ADDR2)) {
        *out_addr = GT911_ADDR2;
        return true;
    }
    return false;
}

static bool configure_gt911_low_level_query(uint8_t addr) {
    constexpr uint16_t GT911_CFG_START_REG = 0x8047;
    constexpr uint16_t GT911_MODULE_SWITCH_1_REG = 0x804D;
    constexpr uint16_t GT911_CFG_CHECKSUM_REG = 0x80FF;
    constexpr uint16_t GT911_CFG_FRESH_REG = 0x8100;

    uint8_t module_switch = 0;
    if (!i2c_read_reg16(addr, GT911_MODULE_SWITCH_1_REG, &module_switch, 1)) {
        return false;
    }
    const uint8_t desired_mode = static_cast<uint8_t>((module_switch & 0xFC) | 0x02); // LOW_LEVEL_QUERY

    if (!i2c_write_reg16(addr, GT911_MODULE_SWITCH_1_REG, desired_mode)) {
        return false;
    }

    uint8_t config[184] = {0};
    if (!i2c_read_reg16_block(addr, GT911_CFG_START_REG, config, sizeof(config))) {
        return false;
    }
    config[GT911_MODULE_SWITCH_1_REG - GT911_CFG_START_REG] = desired_mode;

    uint8_t checksum = 0;
    for (size_t i = 0; i < sizeof(config); i++) {
        checksum = static_cast<uint8_t>(checksum + config[i]);
    }
    checksum = static_cast<uint8_t>(~checksum + 1);

    if (!i2c_write_reg16(addr, GT911_CFG_CHECKSUM_REG, checksum)) {
        return false;
    }
    if (!i2c_write_reg16(addr, GT911_CFG_FRESH_REG, 0x01)) {
        return false;
    }
    return true;
}

static bool gt911_home_button_pressed_edge(uint8_t gt911_addr) {
    static bool was_pressed = false;
    uint8_t status = 0;
    if (!i2c_read_reg16(gt911_addr, GT911_POINT_INFO, &status, 1)) {
        return false;
    }

    const bool pressed = (status & 0x10) != 0;

    const bool is_edge = pressed && !was_pressed;
    was_pressed = pressed;
    return is_edge;
}

static bool cst226_home_button_pressed_edge() {
    static bool was_pressed = false;
    uint8_t buf[28] = {0};
    if (!i2c_read_reg8(CST226_ADDR, 0x00, buf, sizeof(buf))) {
        return false;
    }

    const bool pressed = buf[0] == 0x83 && buf[1] == 0x17 && buf[5] == 0x80;
    const bool is_edge = pressed && !was_pressed;
    was_pressed = pressed;
    return is_edge;
}

static bool is_back_button_touched(const TouchEvent* touch_event) {
    return touch_event->x >= ROOM_CONTROLS_BACK_X && touch_event->x < ROOM_CONTROLS_BACK_X + ROOM_CONTROLS_BACK_W &&
           touch_event->y >= ROOM_CONTROLS_BACK_Y && touch_event->y < ROOM_CONTROLS_BACK_Y + ROOM_CONTROLS_BACK_H;
}

static int8_t list_swipe_delta(const TouchEvent* start_touch, const TouchEvent* end_touch) {
    const int16_t dx = static_cast<int16_t>(end_touch->x) - static_cast<int16_t>(start_touch->x);
    const int16_t dy = static_cast<int16_t>(end_touch->y) - static_cast<int16_t>(start_touch->y);
    const int16_t abs_dx = dx >= 0 ? dx : -dx;
    const int16_t abs_dy = dy >= 0 ? dy : -dy;

    if (abs_dx < ROOM_LIST_SWIPE_THRESHOLD_X || abs_dx <= abs_dy) {
        return 0;
    }

    // Swipe left -> next page, swipe right -> previous page.
    return dx < 0 ? 1 : -1;
}

struct ListGridLayout {
    int16_t columns;
    int16_t rows;
    int16_t items_per_page;
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
        layout.rows = static_cast<int16_t>((item_count + 1) / 2);
    }
    layout.items_per_page = layout.columns * layout.rows;
    return layout;
}

static int16_t list_index_from_touch(const TouchEvent* touch_event, uint8_t item_count, uint8_t list_page, uint16_t grid_start_y,
                                     bool expand_single_page_layout) {
    if (touch_event->x < ROOM_LIST_GRID_MARGIN_X || touch_event->x >= DISPLAY_WIDTH - ROOM_LIST_GRID_MARGIN_X) {
        return -1;
    }
    if (touch_event->y < grid_start_y || touch_event->y >= ROOM_LIST_GRID_BOTTOM_Y) {
        return -1;
    }

    const uint8_t page_count = item_count == 0 ? 1 : static_cast<uint8_t>((item_count + ROOM_LIST_ROOMS_PER_PAGE - 1) / ROOM_LIST_ROOMS_PER_PAGE);
    const uint8_t page = list_page >= page_count ? static_cast<uint8_t>(page_count - 1) : list_page;
    const ListGridLayout layout = list_grid_layout(item_count, page_count, expand_single_page_layout);

    const int16_t grid_w = DISPLAY_WIDTH - 2 * ROOM_LIST_GRID_MARGIN_X;
    const int16_t grid_h = ROOM_LIST_GRID_BOTTOM_Y - grid_start_y;
    const int16_t tile_w = (grid_w - (layout.columns - 1) * ROOM_LIST_GRID_GAP_X) / layout.columns;
    const int16_t tile_h = (grid_h - (layout.rows - 1) * ROOM_LIST_GRID_GAP_Y) / layout.rows;

    const int16_t rel_x = touch_event->x - ROOM_LIST_GRID_MARGIN_X;
    const int16_t rel_y = touch_event->y - grid_start_y;

    const int16_t col_stride = tile_w + ROOM_LIST_GRID_GAP_X;
    const int16_t row_stride = tile_h + ROOM_LIST_GRID_GAP_Y;
    const int16_t col = rel_x / col_stride;
    const int16_t row = rel_y / row_stride;
    if (col < 0 || col >= layout.columns || row < 0 || row >= layout.rows) {
        return -1;
    }

    const int16_t x_in_tile = rel_x % col_stride;
    const int16_t y_in_tile = rel_y % row_stride;
    if (x_in_tile >= tile_w || y_in_tile >= tile_h) {
        return -1;
    }

    const int16_t slot = row * layout.columns + col;
    const int16_t item_idx = page * layout.items_per_page + slot;
    return item_idx < item_count ? item_idx : -1;
}

void touch_task(void* arg) {
    TouchTaskArgs* ctx = static_cast<TouchTaskArgs*>(arg);
    BBCapTouch* bbct = ctx->bbct;
    EntityStore* store = ctx->store;
    Screen* screen = ctx->screen;

    // UI State values
    uint32_t ui_state_version = 0;
    auto* ui_state = new UIState{};

    // Touch infos
    TOUCHINFO ti;
    TouchEvent touch_event = TouchEvent{};
    TouchEvent touch_start = TouchEvent{};
    TouchEvent touch_end = TouchEvent{};
    static FloorListSnapshot floor_list_snapshot = {};
    static RoomListSnapshot room_list_snapshot = {};
    bool touching = false;
    int active_widget = -1;
    uint32_t last_touch_ms = 0;
    uint8_t widget_original_value = 0;
    uint8_t widget_current_value = 0;

    // Initialize touch
    ESP_LOGI(TAG, "Initializing touchscreen...");
    int rc = bbct->init(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT);
    ESP_LOGI(TAG, "init() rc = %d", rc);
    int type = bbct->sensorType();
    ESP_LOGI(TAG, "Sensor type = %d", type);
    uint8_t gt911_addr = 0;
    const bool gt911_addr_present = detect_gt911_address(&gt911_addr);
    const bool cst226_addr_present = i2c_device_present(CST226_ADDR);
    const bool poll_gt911_home = (type == CT_TYPE_GT911) || gt911_addr_present;
    const bool poll_cst226_home = (type == CT_TYPE_CST226) || cst226_addr_present;
    ESP_LOGI(TAG, "Touch home key polling: GT911=%d CST226=%d", poll_gt911_home ? 1 : 0, poll_cst226_home ? 1 : 0);
    if (poll_gt911_home && gt911_addr_present) {
        if (configure_gt911_low_level_query(gt911_addr)) {
            ESP_LOGI(TAG, "Configured GT911 interrupt mode to LOW_LEVEL_QUERY");
        } else {
            ESP_LOGW(TAG, "Failed to configure GT911 interrupt mode");
        }
    }
    while (true) {
        if (!touching) {
            bool go_home = false;
            if (poll_gt911_home && gt911_addr_present && gt911_home_button_pressed_edge(gt911_addr)) {
                ESP_LOGI(TAG, "Home button pressed (GT911 key)");
                go_home = true;
            }
            if (poll_cst226_home && cst226_home_button_pressed_edge()) {
                ESP_LOGI(TAG, "Home button pressed (CST226 key)");
                go_home = true;
            }
            if (go_home) {
                store_go_home(store);
                vTaskDelay(pdMS_TO_TICKS(25));
                continue;
            }
        }

        if (bbct->getSamples(&ti)) {
            last_touch_ms = millis();
            ui_state_copy(ctx->state, &ui_state_version, ui_state);

            if (ui_state->mode != UiMode::RoomControls) {
                active_widget = -1;
            }

            if (ui_state->mode == UiMode::FloorList || ui_state->mode == UiMode::RoomList) {
                if (!touching) {
                    touch_event.x = ti.x[0];
                    touch_event.y = ti.y[0];

                    if (ui_state->mode == UiMode::RoomList && is_back_button_touched(&touch_event)) {
                        ESP_LOGI(TAG, "Back to floor list");
                        store_select_floor(store, -1);
                        continue;
                    }

                    touch_start.x = ti.x[0];
                    touch_start.y = ti.y[0];
                    touch_end = touch_start;
                    touching = true;
                } else {
                    touch_end.x = ti.x[0];
                    touch_end.y = ti.y[0];
                }
                continue;
            }

            if (ui_state->mode != UiMode::RoomControls) {
                continue;
            }

            if (touching == false) {
                touch_event.x = ti.x[0];
                touch_event.y = ti.y[0];
                touch_start = touch_event;
                touch_end = touch_event;
                touching = true;

                if (is_back_button_touched(&touch_event)) {
                    ESP_LOGI(TAG, "Back to room list");
                    store_select_room(store, -1);
                    continue;
                }

                for (size_t widget_idx = 0; widget_idx < screen->widget_count; widget_idx++) {
                    if (screen->widgets[widget_idx]->isTouching(&touch_event)) {
                        ESP_LOGI(TAG, "Starting touch on widget %d", widget_idx);
                        active_widget = widget_idx;
                        break;
                    }
                }
            } else {
                touch_end.x = ti.x[0];
                touch_end.y = ti.y[0];
            }
        } else {
            if (touching) {
                ui_state_copy(ctx->state, &ui_state_version, ui_state);

                if ((ui_state->mode == UiMode::FloorList || ui_state->mode == UiMode::RoomList) &&
                    millis() - last_touch_ms > TOUCH_RELEASE_TIMEOUT_MS) {
                    int8_t page_delta = list_swipe_delta(&touch_start, &touch_end);
                    if (ui_state->mode == UiMode::FloorList) {
                        if (page_delta != 0) {
                            if (store_shift_floor_list_page(store, page_delta)) {
                                ESP_LOGI(TAG, "Swiped floor list to page delta %d", page_delta);
                            }
                        } else {
                            store_get_floor_list_snapshot(store, &floor_list_snapshot);
                            int16_t floor_idx = list_index_from_touch(&touch_start, floor_list_snapshot.floor_count,
                                                                      ui_state->floor_list_page, FLOOR_LIST_GRID_START_Y, true);
                            if (floor_idx >= 0) {
                                ESP_LOGI(TAG, "Selecting floor %d", floor_idx);
                                store_select_floor(store, static_cast<int8_t>(floor_idx));
                            }
                        }
                    } else {
                        if (page_delta != 0) {
                            if (store_shift_room_list_page(store, page_delta)) {
                                ESP_LOGI(TAG, "Swiped room list to page delta %d", page_delta);
                            }
                        } else if (store_get_room_list_snapshot(store, ui_state->selected_floor, &room_list_snapshot)) {
                            int16_t room_list_idx = list_index_from_touch(&touch_start, room_list_snapshot.room_count,
                                                                          ui_state->room_list_page, ROOM_LIST_GRID_START_Y, false);
                            if (room_list_idx >= 0) {
                                int8_t room_idx = room_list_snapshot.room_indices[room_list_idx];
                                ESP_LOGI(TAG, "Selecting room %d", room_idx);
                                store_select_room(store, room_idx);
                            }
                        }
                    }

                    touching = false;
                    active_widget = -1;
                    continue;
                }

                if (ui_state->mode == UiMode::RoomControls && millis() - last_touch_ms > TOUCH_RELEASE_TIMEOUT_MS) {
                    int8_t page_delta = list_swipe_delta(&touch_start, &touch_end);
                    if (page_delta != 0) {
                        if (store_shift_room_controls_page(store, page_delta)) {
                            ESP_LOGI(TAG, "Swiped room controls to page delta %d", page_delta);
                        }
                    } else if (active_widget != -1) {
                        widget_original_value = ui_state->widget_values[active_widget];
                        widget_current_value = screen->widgets[active_widget]->getValueFromTouch(&touch_end, widget_original_value);
                        if (widget_current_value != widget_original_value) {
                            store_send_command(store, screen->entity_ids[active_widget], widget_current_value);
                        }
                    }

                    ESP_LOGI(TAG, "End of touch");
                    touching = false;
                    active_widget = -1;
                    continue;
                }

                if (millis() - last_touch_ms > TOUCH_RELEASE_TIMEOUT_MS) {
                    ESP_LOGI(TAG, "End of touch");
                    touching = false;
                    active_widget = -1;
                }
                vTaskDelay(pdMS_TO_TICKS(25));
            } else {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
        }
    }
}
