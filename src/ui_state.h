#pragma once

#include "constants.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstdint>

enum class UiMode : uint8_t {
    Blank, // state after boot
    Boot,
    GenericError,
    WifiDisconnected,
    HassDisconnected,
    HassInvalidKey,
    MainScreen,
};

struct UIState {
    UiMode mode = UiMode::Blank;
    uint8_t widget_values[MAX_WIDGETS_PER_SCREEN] = {};
};

// The touch task needs to know the current state of the UI.
// This struct handles the sharing of the UIState safely.
struct SharedUIState {
    SemaphoreHandle_t mutex;
    uint32_t version;
    UIState state;
};

void ui_state_init(SharedUIState* state);
void ui_state_set(SharedUIState* state, const UIState* new_state);
void ui_state_copy(SharedUIState* state, uint32_t* local_version, UIState* local_state);