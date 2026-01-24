#include "ui_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstring>

void ui_state_init(SharedUIState* state) {
    state->mutex = xSemaphoreCreateMutex();
}

void ui_state_set(SharedUIState* state, const UIState* new_state) {
    xSemaphoreTake(state->mutex, portMAX_DELAY);
    state->version++;
    memcpy(&state->state, new_state, sizeof(UIState));
    xSemaphoreGive(state->mutex);
}

void ui_state_copy(SharedUIState* state, uint32_t* local_version, UIState* local_state) {
    xSemaphoreTake(state->mutex, portMAX_DELAY);
    const uint32_t shared_version = state->version;
    if (shared_version != *local_version) {
        memcpy(local_state, &state->state, sizeof(UIState));
        *local_version = shared_version;
    }
    xSemaphoreGive(state->mutex);
}