#pragma once
#include "store.h"
#include <cstddef>
#include <cstdint>

enum class PowerBootMode : uint8_t {
    Normal,        // cold boot / reset: today's behavior
    WakeToRoom,    // touch or button woke us from standby sleep
    SilentRefresh, // hourly timer wake: refresh data, never touch the panel unless it changed
};

void power_init();
void power_set_epaper(FASTEPD* epaper); // for panel teardown at sleep entry
PowerBootMode power_boot_mode();
bool power_apply_wifi_sleep(); // re-apply after the Wi-Fi driver is (re)initialized
void power_wifi_sleep_hold(bool hold); // keep the PHY enabled during large receive bursts
bool power_set_modem_sleep(bool enabled);
bool power_set_idle_cpu_mhz(uint32_t mhz); // 80, 160 or 240
void power_report(char* out, size_t out_len);

// The e-ink waveform timing is tuned for full speed; draws run boosted
void power_draw_boost_begin();
void power_draw_boost_end();

const char* power_wake_cause(); // "touch" | "button" | "timer" | "none"

// Deep-sleep standby orchestration (poll from loop())
void power_standby_sleep_poll(EntityStore* store);
void power_set_standby_sleep(bool enabled); // runtime kill switch
bool power_standby_sleep_enabled();
const char* power_sleep_inhibit(); // why we are not sleeping right now ("none" = would sleep)
bool power_is_silent_boot();       // UI suppresses all drawing while true
void power_force_standby_sleep(EntityStore* store, uint32_t timer_s); // sleep now, gates bypassed (settings tile, console)
void power_mark_boot_healthy();    // clears the wake boot-loop guard streak

// Content hash of the standby screen last drawn to the physical panel;
// survives sleep so an unchanged hourly refresh skips the redraw flash
uint32_t power_standby_hash_get();
void power_standby_hash_set(uint32_t hash);
