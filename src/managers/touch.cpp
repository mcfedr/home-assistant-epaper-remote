#include "managers/touch.h"
#include "boards.h"
#include "constants.h"

static const char* TAG = "touch";

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

static int16_t list_index_from_touch(const TouchEvent* touch_event, uint8_t item_count, uint8_t list_page, uint16_t grid_start_y) {
    if (touch_event->x < ROOM_LIST_GRID_MARGIN_X || touch_event->x >= DISPLAY_WIDTH - ROOM_LIST_GRID_MARGIN_X) {
        return -1;
    }
    if (touch_event->y < grid_start_y || touch_event->y >= ROOM_LIST_GRID_BOTTOM_Y) {
        return -1;
    }

    const uint8_t page_count = item_count == 0 ? 1 : static_cast<uint8_t>((item_count + ROOM_LIST_ROOMS_PER_PAGE - 1) / ROOM_LIST_ROOMS_PER_PAGE);
    const uint8_t page = list_page >= page_count ? static_cast<uint8_t>(page_count - 1) : list_page;

    const int16_t grid_w = DISPLAY_WIDTH - 2 * ROOM_LIST_GRID_MARGIN_X;
    const int16_t grid_h = ROOM_LIST_GRID_BOTTOM_Y - grid_start_y;
    const int16_t tile_w = (grid_w - (ROOM_LIST_COLUMNS - 1) * ROOM_LIST_GRID_GAP_X) / ROOM_LIST_COLUMNS;
    const int16_t tile_h = (grid_h - (ROOM_LIST_ROWS - 1) * ROOM_LIST_GRID_GAP_Y) / ROOM_LIST_ROWS;

    const int16_t rel_x = touch_event->x - ROOM_LIST_GRID_MARGIN_X;
    const int16_t rel_y = touch_event->y - grid_start_y;

    const int16_t col_stride = tile_w + ROOM_LIST_GRID_GAP_X;
    const int16_t row_stride = tile_h + ROOM_LIST_GRID_GAP_Y;
    const int16_t col = rel_x / col_stride;
    const int16_t row = rel_y / row_stride;
    if (col < 0 || col >= ROOM_LIST_COLUMNS || row < 0 || row >= ROOM_LIST_ROWS) {
        return -1;
    }

    const int16_t x_in_tile = rel_x % col_stride;
    const int16_t y_in_tile = rel_y % row_stride;
    if (x_in_tile >= tile_w || y_in_tile >= tile_h) {
        return -1;
    }

    const int16_t slot = row * ROOM_LIST_COLUMNS + col;
    const int16_t item_idx = page * ROOM_LIST_ROOMS_PER_PAGE + slot;
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
    TouchEvent room_list_touch_start = TouchEvent{};
    TouchEvent room_list_touch_end = TouchEvent{};
    FloorListSnapshot floor_list_snapshot = {};
    RoomListSnapshot room_list_snapshot = {};
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

    while (true) {
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

                    room_list_touch_start.x = ti.x[0];
                    room_list_touch_start.y = ti.y[0];
                    room_list_touch_end = room_list_touch_start;
                    touching = true;
                } else {
                    room_list_touch_end.x = ti.x[0];
                    room_list_touch_end.y = ti.y[0];
                }
                continue;
            }

            if (ui_state->mode != UiMode::RoomControls) {
                continue;
            }

            // We're already targeting a widget
            if (active_widget != -1) {
                if (touch_event.x == ti.x[0] && touch_event.y == ti.y[0]) {
                    // Finger did not move, ignore
                } else {
                    touch_event.x = ti.x[0];
                    touch_event.y = ti.y[0];

                    widget_current_value = screen->widgets[active_widget]->getValueFromTouch(&touch_event, widget_original_value);
                    store_send_command(store, screen->entity_ids[active_widget], widget_current_value);
                }
            } else if (touching == false) {
                touch_event.x = ti.x[0];
                touch_event.y = ti.y[0];
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

                        // Get the new value
                        widget_original_value = ui_state->widget_values[widget_idx];
                        widget_current_value = screen->widgets[widget_idx]->getValueFromTouch(&touch_event, widget_original_value);

                        store_send_command(store, screen->entity_ids[active_widget], widget_current_value);

                        break;
                    }
                }
            }
        } else {
            if (touching) {
                ui_state_copy(ctx->state, &ui_state_version, ui_state);

                if ((ui_state->mode == UiMode::FloorList || ui_state->mode == UiMode::RoomList) &&
                    millis() - last_touch_ms > TOUCH_RELEASE_TIMEOUT_MS) {
                    int8_t page_delta = list_swipe_delta(&room_list_touch_start, &room_list_touch_end);
                    if (ui_state->mode == UiMode::FloorList) {
                        if (page_delta != 0) {
                            if (store_shift_floor_list_page(store, page_delta)) {
                                ESP_LOGI(TAG, "Swiped floor list to page delta %d", page_delta);
                            }
                        } else {
                            store_get_floor_list_snapshot(store, &floor_list_snapshot);
                            int16_t floor_idx = list_index_from_touch(&room_list_touch_start, floor_list_snapshot.floor_count,
                                                                      ui_state->floor_list_page, FLOOR_LIST_GRID_START_Y);
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
                            int16_t room_list_idx = list_index_from_touch(&room_list_touch_start, room_list_snapshot.room_count,
                                                                          ui_state->room_list_page, ROOM_LIST_GRID_START_Y);
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
