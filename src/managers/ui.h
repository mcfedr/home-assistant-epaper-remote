#pragma once
#include "screen.h"
#include "store.h"
#include "ui_state.h"
#include <FastEPD.h>

struct UITaskArgs {
    EntityStore* store;
    Screen* screen;
    FASTEPD* epaper;
    SharedUIState* shared_state;
};

void ui_task(void* arg);

// Small partial-update indicator drawn during a wake-from-sleep boot while the
// panel still shows the frozen standby screen (called from setup, pre ui_task)
void ui_draw_wake_glyph(FASTEPD* epaper);