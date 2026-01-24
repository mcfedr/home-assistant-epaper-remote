#pragma once
#include "screen.h"
#include "store.h"
#include "ui_state.h"
#include <bb_captouch.h>

struct TouchTaskArgs {
    SharedUIState* state;
    EntityStore* store;
    BBCapTouch* bbct;
    Screen* screen;
};

void touch_task(void* arg);