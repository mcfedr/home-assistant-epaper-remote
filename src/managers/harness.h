#pragma once
#include "screen.h"
#include "store.h"
#include "ui_state.h"
#include <FastEPD.h>
#include <bb_captouch.h>

struct HarnessTaskArgs {
    EntityStore* store;
    Screen* screen;
    FASTEPD* epaper;
    SharedUIState* shared_state;
};

void launch_harness(HarnessTaskArgs* args);

// Drop-in replacement for bbct->getSamples() in the touch task: returns a pending
// synthetic gesture sample when one is active, otherwise reads the hardware.
bool harness_get_samples(BBCapTouch* bbct, TOUCHINFO* ti);
