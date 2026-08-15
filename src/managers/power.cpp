#include "managers/power.h"
#include "managers/beacon.h"
#include "managers/mqtt.h"
#include "managers/touch.h"
#include "boards.h"
#include "constants.h"
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
RTC_NOINIT_ATTR static uint8_t g_slept_from_standby;
RTC_NOINIT_ATTR static uint8_t g_wake_boot_streak; // wake boots that never proved healthy
RTC_NOINIT_ATTR static uint32_t g_rtc_standby_hash;

static PowerBootMode g_boot_mode = PowerBootMode::Normal;
static FASTEPD* g_epaper = nullptr;
static bool g_silent_boot = false;
static bool g_standby_sleep = true;
static bool g_sleep_guard_tripped = false;
static const char* volatile g_sleep_inhibit = "boot";

// Measured on battery 2026-08-11: 112mA original; modem sleep -> 95mA;
// + 80MHz idle -> 80mA. Draws always run boosted, so display timing is unaffected.
static bool g_modem_sleep = true;
static uint32_t g_idle_cpu_mhz = 80;
static bool g_boosted = false;
static bool g_sleep_hold = false;

void power_init() {
    if (g_wake_magic != WAKE_MAGIC) { // power-on: RTC RAM is garbage
        g_wake_code = 0;
        g_slept_from_standby = 0;
        g_wake_boot_streak = 0;
        g_rtc_standby_hash = 0;
        g_wake_magic = WAKE_MAGIC;
    }

    bool fresh_wake = true;
    switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT1:
        g_wake_code = (esp_sleep_get_ext1_wakeup_status() & (1ULL << TOUCH_INT)) ? 1 : 2;
        break;
    case ESP_SLEEP_WAKEUP_TIMER:
        g_wake_code = 3;
        break;
    default:
        fresh_wake = false; // esp_restart or reset: keep the stored wake code
        break;
    }

    if (fresh_wake && g_slept_from_standby) {
        g_boot_mode = (g_wake_code == 3) ? PowerBootMode::SilentRefresh : PowerBootMode::WakeToRoom;
        g_wake_boot_streak++;
        if (g_wake_boot_streak > WAKE_BOOT_STREAK_LIMIT) {
            // Wake-path boots keep dying before proving healthy; fall back to
            // a normal always-on boot so a bug here can never boot-loop
            ESP_LOGE(TAG, "wake boot streak %u exceeded limit; disabling standby sleep this boot",
                     static_cast<unsigned>(g_wake_boot_streak));
            g_boot_mode = PowerBootMode::Normal;
            g_sleep_guard_tripped = true;
        }
    }
    g_slept_from_standby = 0; // consumed
    g_silent_boot = g_boot_mode == PowerBootMode::SilentRefresh;

    setCpuFrequencyMhz(g_idle_cpu_mhz);
    ESP_LOGI(TAG, "modem_sleep=%d idle_cpu=%lu boot_mode=%d wake=%s", g_modem_sleep ? 1 : 0,
             static_cast<unsigned long>(g_idle_cpu_mhz), static_cast<int>(g_boot_mode), power_wake_cause());
}

PowerBootMode power_boot_mode() {
    return g_boot_mode;
}

void power_set_epaper(FASTEPD* epaper) {
    g_epaper = epaper;
}

void power_rails(bool on) {
    if (g_epaper != nullptr) {
        const int rc = g_epaper->einkPower(on ? 1 : 0);
        ESP_LOGI(TAG, "eink rails -> %d (rc=%d)", on ? 1 : 0, rc);
    }
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

static void power_enter_sleep(uint32_t timer_s) {
    const bool touch_armed = touch_prepare_for_sleep();
    ESP_LOGI(TAG, "GT911 falling-edge wake mode: %s", touch_armed ? "armed" : "FAILED");

    // EXT1 any-low fires immediately if INT is still pulsing from the touch
    // that got us here; require 300ms of quiet line before sleeping
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
    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(timer_s) * 1000000ULL);
    ESP_LOGI(TAG, "entering deep sleep: wake on touch/button or %lus timer", static_cast<unsigned long>(timer_s));

    // The standby/sleep-test draws already parked the drivers and cut the
    // panel rails (bKeepOn=false); einkPower(0) is a last resort if they were
    // somehow left on. deInit() then floats the control lines and puts the
    // TPS65185 fully asleep so deep-sleep pin isolation can't glitch the
    // panel — the pattern FastEPD's own low-power clock example uses.
    if (g_epaper != nullptr) {
        g_epaper->einkPower(0);
        g_epaper->deInit();
    }

    // Deauth from the AP first; vanishing mid-session leaves a ghost session
    // that UniFi punishes with AUTH_EXPIRE loops on rejoin.
    WiFi.disconnect(true);
    delay(100);

    Serial.flush();
    delay(50);
    esp_deep_sleep_start();
}

void power_deep_sleep_test(uint32_t timer_backstop_s) {
    g_slept_from_standby = 0; // test wake boots like a cold start
    power_enter_sleep(timer_backstop_s);
}

// Full standby sleep: last telemetry, explicit MQTT offline (a clean stop
// doesn't fire the last-will), advertising off, then the shared sleep entry
static void power_enter_standby_sleep(EntityStore* store, uint32_t timer_s) {
    ESP_LOGI(TAG, "standby sleep: publishing final telemetry and going offline");
    mqtt_publish_now(store);
    delay(200);
    mqtt_publish_offline();
    delay(200);
    beacon_stop();
    g_slept_from_standby = 1;
    g_wake_boot_streak = 0; // reaching a clean sleep entry proves the boot healthy
    power_enter_sleep(timer_s);
}

void power_force_standby_sleep(EntityStore* store, uint32_t timer_s) {
    power_enter_standby_sleep(store, timer_s);
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

bool power_set_standby_sleep(bool enabled) {
    g_standby_sleep = enabled;
    ESP_LOGI(TAG, "standby sleep -> %d", enabled ? 1 : 0);
    return true;
}

bool power_standby_sleep_enabled() {
    return g_standby_sleep;
}

const char* power_sleep_inhibit() {
    return g_sleep_inhibit;
}

bool power_is_silent_boot() {
    return g_silent_boot;
}

void power_mark_boot_healthy() {
    if (g_wake_boot_streak != 0 && !g_sleep_guard_tripped) {
        g_wake_boot_streak = 0;
    }
}

uint32_t power_standby_hash_get() {
    return g_rtc_standby_hash;
}

void power_standby_hash_set(uint32_t hash) {
    g_rtc_standby_hash = hash;
}

// Battery gate: sleep only when clearly discharging. USB power reads as
// charging or idle, and boards without a gauge (M5) never sleep.
static bool power_on_battery(EntityStore* store) {
    BatteryStatus battery;
    store_get_battery(store, &battery);
    return battery.valid && battery.milliamps < -BATTERY_CHARGE_IDLE_BAND_MA;
}

// Silent refresh cycle: standby data is refreshed with the panel untouched,
// telemetry published, then back to sleep. Any user interaction exits to the
// normal awake UI (standby deactivates via the touch wake path).
static void power_silent_refresh_poll(EntityStore* store) {
    static uint32_t refreshed_at = 0;

    if (!store_is_standby_active(store)) {
        if (store_open_standby(store, millis())) {
            ESP_LOGI(TAG, "silent refresh: standby resumed, waiting for data");
        }
    } else if (refreshed_at == 0 && store_standby_data_fresh(store)) {
        refreshed_at = millis();
        ESP_LOGI(TAG, "silent refresh: data landed, lingering %lums for publish/redraw", static_cast<unsigned long>(SILENT_REFRESH_LINGER_MS));
    }

    const bool lingered = refreshed_at != 0 && millis() - refreshed_at >= SILENT_REFRESH_LINGER_MS;
    const bool backstop = millis() >= SILENT_REFRESH_BACKSTOP_MS;
    if (!lingered && !backstop) {
        return;
    }

    g_silent_boot = false;
    if (store_is_standby_active(store)) {
        if (power_on_battery(store) && g_standby_sleep && !g_sleep_guard_tripped) {
            power_enter_standby_sleep(store, STANDBY_SLEEP_TIMER_S); // does not return
        }
        ESP_LOGI(TAG, "silent refresh done but sleep inhibited; staying awake");
    }
    store_notify_ui(store); // wake the UI out of suppression
}

void power_standby_sleep_poll(EntityStore* store) {
    if (g_silent_boot) {
        // User interaction during the refresh window deactivates standby and
        // exits silent mode below via the standby check in the refresh poll —
        // but a direct touch lands in the wake path, so check explicitly too.
        if (store_interaction_seen(store)) {
            ESP_LOGI(TAG, "silent refresh interrupted by user; showing UI");
            g_silent_boot = false;
            store_notify_ui(store);
            return;
        }
        power_silent_refresh_poll(store);
        return;
    }

    static uint32_t standby_since = 0;
    const uint32_t now = millis();

    if (!g_standby_sleep) {
        g_sleep_inhibit = "disabled";
    } else if (g_sleep_guard_tripped) {
        g_sleep_inhibit = "guard";
    } else if (!store_is_standby_active(store)) {
        g_sleep_inhibit = "awake";
        standby_since = 0;
        return;
    } else if (!power_on_battery(store)) {
        g_sleep_inhibit = "usb";
    } else {
        if (standby_since == 0) {
            standby_since = now;
        }
        if (now - standby_since < STANDBY_SLEEP_SETTLE_MS) {
            g_sleep_inhibit = "settling";
        } else {
            g_sleep_inhibit = "none";
            power_enter_standby_sleep(store, STANDBY_SLEEP_TIMER_S); // does not return
        }
    }

    if (standby_since == 0) {
        standby_since = now;
    }
}
