#pragma once
#include <cstddef>
#include <cstdint>

void power_init();
void power_apply_wifi_sleep(); // re-apply after the Wi-Fi driver is (re)initialized
bool power_set_modem_sleep(bool enabled);
bool power_set_idle_cpu_mhz(uint32_t mhz); // 80, 160 or 240
void power_report(char* out, size_t out_len);

// The e-ink waveform timing is tuned for full speed; draws run boosted
void power_draw_boost_begin();
void power_draw_boost_end();
