#include "managers/power.h"
#include "esp_log.h"
#include <Arduino.h>
#include <WiFi.h>
#include <cstdio>

static const char* TAG = "power";

static bool g_modem_sleep = true;
static uint32_t g_idle_cpu_mhz = 240; // conservative default until measured
static bool g_boosted = false;

void power_init() {
    setCpuFrequencyMhz(g_idle_cpu_mhz);
    ESP_LOGI(TAG, "modem_sleep=%d idle_cpu=%lu", g_modem_sleep ? 1 : 0, static_cast<unsigned long>(g_idle_cpu_mhz));
}

void power_apply_wifi_sleep() {
    WiFi.setSleep(g_modem_sleep);
}

bool power_set_modem_sleep(bool enabled) {
    g_modem_sleep = enabled;
    const bool ok = WiFi.setSleep(enabled);
    ESP_LOGI(TAG, "modem sleep -> %d (ok=%d)", enabled ? 1 : 0, ok ? 1 : 0);
    return ok;
}

bool power_set_idle_cpu_mhz(uint32_t mhz) {
    if (mhz != 80 && mhz != 160 && mhz != 240) {
        return false;
    }
    g_idle_cpu_mhz = mhz;
    if (!g_boosted) {
        setCpuFrequencyMhz(mhz);
    }
    ESP_LOGI(TAG, "idle cpu -> %lu MHz", static_cast<unsigned long>(mhz));
    return true;
}

void power_report(char* out, size_t out_len) {
    snprintf(out, out_len, "modem_sleep=%d idle_cpu_mhz=%lu current_cpu_mhz=%lu", g_modem_sleep ? 1 : 0,
             static_cast<unsigned long>(g_idle_cpu_mhz), static_cast<unsigned long>(getCpuFrequencyMhz()));
}

void power_draw_boost_begin() {
    g_boosted = true;
    if (getCpuFrequencyMhz() != 240) {
        setCpuFrequencyMhz(240);
    }
}

void power_draw_boost_end() {
    g_boosted = false;
    if (getCpuFrequencyMhz() != g_idle_cpu_mhz) {
        setCpuFrequencyMhz(g_idle_cpu_mhz);
    }
}
