#include "ui.h"
#include "assets/icons.h"
#include "boards.h"
#include "constants.h"
#include "draw.h"
#include "store.h"
#include "widgets/Widget.h"
#include <algorithm>

static const char* TAG = "ui";
static const char* const TEXT_BOOT[] = {"Home Assistant", "e-paper remote", nullptr};
static const char* const TEXT_WIFI_DISCONNECTED[] = {"Not connected", "to Wifi", nullptr};
static const char* const TEXT_HASS_DISCONNECTED[] = {"Not connected", "to Home Assistant", nullptr};
static const char* const TEXT_HASS_INVALID_KEY[] = {"Cannot connect", "to Home Assistant:", "invalid token", nullptr};
static const char* const TEXT_GENERIC_ERROR[] = {"Unknown error", nullptr};

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

void ui_main_screen_full_draw(UIState* state, BitDepth depth, Screen* screen, FASTEPD* epaper) {
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
    }

    drawCenteredIconWithText(epaper, icon, text_lines, 30, 100);
}

void ui_task(void* arg) {
    UITaskArgs* ctx = static_cast<UITaskArgs*>(arg);
    UIState current_state = {};
    UIState displayed_state = {};
    bool display_is_dirty = false;

    xTaskNotifyGive(xTaskGetCurrentTaskHandle()); // First refresh needs a notification

    while (1) {
        TickType_t notify_timeout = portMAX_DELAY;
        if (display_is_dirty) {
            notify_timeout = pdMS_TO_TICKS(DISPLAY_FULL_REDRAW_TIMEOUT_MS);
        }

        if (ulTaskNotifyTake(pdTRUE, notify_timeout)) {
            store_update_ui_state(ctx->store, ctx->screen, &current_state);

            // Handle screen change
            size_t widget_idx;
            if (current_state.mode != displayed_state.mode) {
                ctx->epaper->setMode(BB_MODE_4BPP);
                ctx->epaper->fillScreen(0xf);

                if (current_state.mode == UiMode::MainScreen) {
                    // Display the main screen in its full glory
                    ui_main_screen_full_draw(&current_state, BitDepth::BD_4BPP, ctx->screen, ctx->epaper);
                    ctx->epaper->fullUpdate(CLEAR_SLOW, true);

                    // Preload the 1BPP version for fast updates
                    ctx->epaper->setMode(BB_MODE_1BPP);
                    ctx->epaper->fillScreen(BBEP_WHITE);
                    ui_main_screen_full_draw(&displayed_state, BitDepth::BD_1BPP, ctx->screen, ctx->epaper);
                    ctx->epaper->backupPlane();
                } else {
                    ui_show_message(current_state.mode, ctx->epaper);
                    ctx->epaper->fullUpdate(CLEAR_SLOW, true);
                }
                display_is_dirty = false;
            } else if (current_state.mode == UiMode::MainScreen) {
                Rect damage_accum = {};

                for (widget_idx = 0; widget_idx < ctx->screen->widget_count; widget_idx++) {
                    uint8_t displayed_value = displayed_state.widget_values[widget_idx];
                    uint8_t current_value = current_state.widget_values[widget_idx];

                    if (displayed_value != current_value) {
                        ESP_LOGI(TAG, "updating widget %d from %d to %d", widget_idx, displayed_value, current_value);
                        Rect damage = ctx->screen->widgets[widget_idx]->partialDraw(ctx->epaper, BitDepth::BD_1BPP, displayed_value,
                                                                                    current_value);
                        accumulate_damage(damage_accum, damage);
                    }
                }
                if (damage_accum.w > 0 || damage_accum.h > 0) {
                    ESP_LOGI(TAG, "Launching partial update rows %d to %d", damage_accum.x, damage_accum.x + damage_accum.w);
                    ctx->epaper->partialUpdate(true,
                                               DISPLAY_WIDTH - (damage_accum.x + damage_accum.w), // row start (reversed)
                                               DISPLAY_WIDTH - damage_accum.x                     // row end (reversed)
                    );
                    display_is_dirty = true;
                }
            }

            // Save new state
            displayed_state = current_state;
            ui_state_set(ctx->shared_state, &displayed_state);
        } else if (display_is_dirty) {
            ESP_LOGI(TAG, "Forcing a full refresh of the display");

            // Cleanup the display
            ctx->epaper->setMode(BB_MODE_4BPP);
            ctx->epaper->fillScreen(0xf);
            ui_main_screen_full_draw(&displayed_state, BitDepth::BD_4BPP, ctx->screen, ctx->epaper);
            ctx->epaper->fullUpdate(CLEAR_FAST, true);

            // Preload the 1bpp version for fast updates
            ctx->epaper->setMode(BB_MODE_1BPP);
            ctx->epaper->fillScreen(BBEP_WHITE);
            ui_main_screen_full_draw(&displayed_state, BitDepth::BD_1BPP, ctx->screen, ctx->epaper);
            ctx->epaper->backupPlane();

            display_is_dirty = false;
        }
    }
}