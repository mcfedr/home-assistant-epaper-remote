#pragma once
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdint>

// Two clocks with different deep-sleep behavior. millis()/esp_timer are
// restored from the RTC after a deep-sleep wake (and survive esp_restart),
// while the FreeRTOS tick restarts at zero every boot. Mixing them fired the
// standby idle timeout instantly on wake boots — pick by name instead.

// Continuous across deep sleep and software resets; same base as millis()
static inline uint32_t uptime_ms() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

// Time since this boot started; use for "has this boot proven itself" checks
static inline uint32_t since_boot_ms() {
    return static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
}
