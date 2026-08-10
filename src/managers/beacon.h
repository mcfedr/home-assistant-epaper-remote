#pragma once
#include "store.h"

void launch_beacon(EntityStore* store);
const char* beacon_status(); // "off" | "starting" | "advertising" | "error"
