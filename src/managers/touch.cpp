#include "managers/touch.h"
#include "boards.h"

static const char* TAG = "touch";

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

            // We're already targeting a widget
            if (active_widget != -1) {
                if (touch_event.x == ti.x[0] && touch_event.y == ti.y[0]) {
                    // Finger did not move, ignore
                } else {
                    touch_event.x = ti.x[0];
                    touch_event.y = ti.y[0];
                    ESP_LOGI(TAG, "Widget %d, Coordinates: %d %d", active_widget, touch_event.x, touch_event.y);

                    // Get the new value
                    widget_current_value = screen->widgets[active_widget]->getValueFromTouch(&touch_event, widget_original_value);

                    store_send_command(store, screen->entity_ids[active_widget], widget_current_value);
                }
            } else if (touching == false) {
                touch_event.x = ti.x[0];
                touch_event.y = ti.y[0];
                touching = true;
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