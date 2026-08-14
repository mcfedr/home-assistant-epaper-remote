#include "managers/power.h"
#include "managers/touch.h"
#include "boards.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include <Arduino.h>
#include <WiFi.h>
#include <cstdio>

static const char* TAG = "power";

// Wake cause survives the Wi-Fi recovery ladder's esp_restart (RTC RAM persists
// across software resets and deep sleep, only a power cycle clears it).
static constexpr uint32_t WAKE_MAGIC = 0x57414B45; // "WAKE"
RTC_NOINIT_ATTR static uint32_t g_wake_magic;
RTC_NOINIT_ATTR static uint32_t g_wake_code; // 0 none, 1 touch, 2 button, 3 timer

// Measured on battery 2026-08-11: 112mA original; modem sleep -> 95mA;
// + 80MHz idle -> 80mA. Draws always run boosted, so display timing is unaffected.
static bool g_modem_sleep = true;
static uint32_t g_idle_cpu_mhz = 80;
static bool g_boosted = false;
static bool g_sleep_hold = false;

void power_init() {
    switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT1:
        g_wake_code = (esp_sleep_get_ext1_wakeup_status() & (1ULL << TOUCH_INT)) ? 1 : 2;
        g_wake_magic = WAKE_MAGIC;
        break;
    case ESP_SLEEP_WAKEUP_TIMER:
        g_wake_code = 3;
        g_wake_magic = WAKE_MAGIC;
        break;
    default:
        if (g_wake_magic != WAKE_MAGIC) {
            g_wake_code = 0;
        }
        break;
    }
    setCpuFrequencyMhz(g_idle_cpu_mhz);
    ESP_LOGI(TAG, "modem_sleep=%d idle_cpu=%lu", g_modem_sleep ? 1 : 0, static_cast<unsigned long>(g_idle_cpu_mhz));
}

void power_apply_wifi_sleep() {
    WiFi.setSleep(g_modem_sleep && !g_sleep_hold);
}

// Modem sleep cycles the PHY on every DTIM beacon, and each re-enable does an
// esp_timer_create that aborts under heap exhaustion (phy_track_pll_init
// ESP_ERR_NO_MEM). Large receive bursts (HA discovery) hold sleep off so the
// PHY stays enabled while internal heap is under pressure.
void power_wifi_sleep_hold(bool hold) {
    if (g_sleep_hold == hold) {
        return;
    }
    g_sleep_hold = hold;
    power_apply_wifi_sleep();
    ESP_LOGI(TAG, "wifi sleep hold -> %d", hold ? 1 : 0);
}

bool power_set_modem_sleep(bool enabled) {
    g_modem_sleep = enabled;
    const bool ok = WiFi.setSleep(enabled && !g_sleep_hold);
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
    snprintf(out, out_len, "modem_sleep=%d sleep_hold=%d idle_cpu_mhz=%lu current_cpu_mhz=%lu", g_modem_sleep ? 1 : 0, g_sleep_hold ? 1 : 0,
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

void power_deep_sleep_test(uint32_t timer_backstop_s) {
    const bool touch_armed = touch_prepare_for_sleep();
    ESP_LOGI(TAG, "GT911 falling-edge wake mode: %s", touch_armed ? "armed" : "FAILED");

    // EXT1 any-low fires immediately if INT is still pulsing from the tap that
    // started the test; require 300ms of quiet line before sleeping
    uint32_t high_since = 0;
    const uint32_t settle_deadline = millis() + 3000;
    while (millis() < settle_deadline) {
        if (digitalRead(TOUCH_INT) == LOW) {
            high_since = 0;
        } else if (high_since == 0) {
            high_since = millis();
        } else if (millis() - high_since >= 300) {
            break;
        }
        delay(10);
    }

    // GT911 INT pulses low on touch events; the home button is active low,
    // so both share one any-low wake mask
    uint64_t mask = 1ULL << TOUCH_INT;
    if (HOME_BUTTON_PIN >= 0) {
        mask |= 1ULL << HOME_BUTTON_PIN;
    }
    esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(timer_backstop_s) * 1000000ULL);
    ESP_LOGI(TAG, "entering deep sleep: wake on touch/button or %lus timer", static_cast<unsigned long>(timer_backstop_s));

    // Deauth from the AP first; vanishing mid-session leaves a ghost session
    // that UniFi punishes with AUTH_EXPIRE loops on rejoin.
    WiFi.disconnect(true);
    delay(100);

    Serial.flush();
    delay(50);
    esp_deep_sleep_start();
}

const char* power_wake_cause() {
    switch (g_wake_code) {
    case 1:
        return "touch";
    case 2:
        return "button";
    case 3:
        return "timer";
    default:
        return "none";
    }
}
