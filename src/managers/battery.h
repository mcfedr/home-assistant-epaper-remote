#pragma once
#include "store.h"

// Samples the fuel gauge periodically; call from loop()
void battery_poll(EntityStore* store);
