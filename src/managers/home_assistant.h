#pragma once

#include "config.h"
#include "store.h"

struct HomeAssistantTaskArgs {
    EntityStore* store;
    Configuration* config;
};

void home_assistant_task(void* arg);