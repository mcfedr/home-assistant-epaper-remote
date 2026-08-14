#include "wifi.h"
#include "config.h"
#include "managers/power.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "store.h"
#include <Preferences.h>
#include <WiFi.h>
#include <cstring>

static const char* TAG = "wifi";

// Kept low to avoid brownouts while the e-ink panel draws; 15dBm tested worse
// (zero associations), so raising it does not help the auth loops.
static constexpr wifi_power_t WIFI_TX_POWER = WIFI_POWER_8_5dBm;

// Auth-failure loops sometimes poison the Wi-Fi driver session itself: a reboot
// (or connecting to a different network and back) fixes them instantly, while
// plain reconnects fail for 40+ minutes. Recovery escalates accordingly:
// deep driver reset (stop+deinit, reboot-equivalent) -> quiet backoff for any
// AP-side state -> esp_restart as the last resort.
static constexpr uint32_t WIFI_RECONNECT_BACKOFF_MS = 45000;
static constexpr uint32_t WIFI_RECONNECT_BACKOFF_LONG_MS = 150000; // second and later backoffs: AP-side throttling needs real quiet
static constexpr uint8_t WIFI_DEEP_RESETS_BEFORE_BACKOFF = 2;
static constexpr uint8_t WIFI_BACKOFFS_BEFORE_RESTART = 2;
static constexpr uint32_t WIFI_MAX_AUTO_RESTARTS = 2; // per unbroken outage, survives esp_restart

RTC_NOINIT_ATTR static uint32_t g_wifi_auto_restart_count;

// A single AUTH_FAIL does not prove a bad password: WPA3 transition-mode APs
// emit transient AUTH_FAIL/AUTH_EXPIRE during SAE negotiation hiccups.
static constexpr uint8_t WIFI_AUTH_FAILS_FOR_BAD_CREDENTIALS = 3;
static constexpr const char* WIFI_PREFS_NS = "wifi";
static constexpr const char* WIFI_PREF_SSID_KEY = "ssid";
static constexpr const char* WIFI_PREF_PASS_KEY = "pass";
static constexpr const char* WIFI_PREF_NET_COUNT_KEY = "nc";

struct WifiContext {
    Configuration* config = nullptr;
    EntityStore* store = nullptr;
    bool scan_requested = false;
    bool scan_running = false;
    bool recovery_requested = false;
    uint8_t recovery_reason = 0;
    uint32_t last_info_refresh_ms = 0;
    uint32_t last_recovery_ms = 0;
    uint8_t consecutive_disconnects = 0;
    uint8_t consecutive_auth_fails = 0;
    uint32_t reconnect_pause_until_ms = 0;
    uint8_t deep_resets_since_connect = 0;
    uint8_t backoffs_since_connect = 0;
    bool restart_requested = false;
    bool active_custom_profile = false;
    char active_ssid[MAX_WIFI_SSID_LEN] = {0};
    char active_password[MAX_WIFI_PASSWORD_LEN + 1] = {0};
    uint32_t boot_connect_started_ms = 0;
    bool boot_settings_fallback_shown = false;
    bool boot_settings_auto_opened = false;
    bool pause_reconnect_for_scan = false;
};

static WifiContext g_wifi = {};

static void copy_string(char* dst, size_t dst_len, const char* src) {
    if (dst_len == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

struct WifiHistoryEntry {
    char ssid[MAX_WIFI_SSID_LEN];
    char password[MAX_WIFI_PASSWORD_LEN + 1];
};

static uint8_t wifi_load_history(WifiHistoryEntry* out, uint8_t max_count) {
    Preferences prefs;
    if (!prefs.begin(WIFI_PREFS_NS, true)) {
        return 0;
    }
    uint8_t count = prefs.getUChar(WIFI_PREF_NET_COUNT_KEY, 0);
    if (count > max_count) {
        count = max_count;
    }
    char key[6];
    for (uint8_t i = 0; i < count; i++) {
        snprintf(key, sizeof(key), "ss%u", i);
        String ssid = prefs.getString(key, "");
        snprintf(key, sizeof(key), "sp%u", i);
        String pass = prefs.getString(key, "");
        copy_string(out[i].ssid, sizeof(out[i].ssid), ssid.c_str());
        copy_string(out[i].password, sizeof(out[i].password), pass.c_str());
    }
    prefs.end();
    return count;
}

static void wifi_save_history(const WifiHistoryEntry* entries, uint8_t count) {
    if (count > MAX_WIFI_SAVED_NETWORKS) {
        count = MAX_WIFI_SAVED_NETWORKS;
    }
    Preferences prefs;
    if (!prefs.begin(WIFI_PREFS_NS, false)) {
        ESP_LOGW(TAG, "failed to open NVS for saving Wi-Fi history");
        return;
    }
    prefs.putUChar(WIFI_PREF_NET_COUNT_KEY, count);
    char key[6];
    for (uint8_t i = 0; i < count; i++) {
        snprintf(key, sizeof(key), "ss%u", i);
        prefs.putString(key, entries[i].ssid);
        snprintf(key, sizeof(key), "sp%u", i);
        prefs.putString(key, entries[i].password);
    }
    prefs.end();
}

static void wifi_add_to_history(const char* ssid, const char* password) {
    if (!ssid || ssid[0] == '\0') {
        return;
    }
    WifiHistoryEntry entries[MAX_WIFI_SAVED_NETWORKS];
    uint8_t count = wifi_load_history(entries, MAX_WIFI_SAVED_NETWORKS);

    WifiHistoryEntry new_entries[MAX_WIFI_SAVED_NETWORKS];
    copy_string(new_entries[0].ssid, sizeof(new_entries[0].ssid), ssid);
    copy_string(new_entries[0].password, sizeof(new_entries[0].password), password ? password : "");
    uint8_t new_count = 1;
    for (uint8_t i = 0; i < count && new_count < MAX_WIFI_SAVED_NETWORKS; i++) {
        if (strcmp(entries[i].ssid, ssid) != 0) {
            new_entries[new_count++] = entries[i];
        }
    }
    wifi_save_history(new_entries, new_count);
    ESP_LOGI(TAG, "saved '%s' to network history (%u total)", ssid, new_count);
}

bool wifi_find_saved_password(const char* ssid, char* pass_out, size_t pass_len) {
    WifiHistoryEntry entries[MAX_WIFI_SAVED_NETWORKS];
    uint8_t count = wifi_load_history(entries, MAX_WIFI_SAVED_NETWORKS);
    for (uint8_t i = 0; i < count; i++) {
        if (strcmp(entries[i].ssid, ssid) == 0) {
            copy_string(pass_out, pass_len, entries[i].password);
            return true;
        }
    }
    return false;
}

static bool wifi_load_saved_profile(char* ssid_out, size_t ssid_out_len, char* pass_out, size_t pass_out_len) {
    Preferences prefs;
    if (!prefs.begin(WIFI_PREFS_NS, true)) {
        return false;
    }

    String saved_ssid = prefs.getString(WIFI_PREF_SSID_KEY, "");
    String saved_pass = prefs.getString(WIFI_PREF_PASS_KEY, "");
    prefs.end();

    if (saved_ssid.length() == 0) {
        return false;
    }

    copy_string(ssid_out, ssid_out_len, saved_ssid.c_str());
    copy_string(pass_out, pass_out_len, saved_pass.c_str());
    return true;
}

static void wifi_save_custom_profile(const char* ssid, const char* password) {
    Preferences prefs;
    if (!prefs.begin(WIFI_PREFS_NS, false)) {
        ESP_LOGW(TAG, "failed to open NVS for saving Wi-Fi profile");
        return;
    }

    prefs.putString(WIFI_PREF_SSID_KEY, ssid ? ssid : "");
    prefs.putString(WIFI_PREF_PASS_KEY, password ? password : "");
    prefs.end();
    ESP_LOGI(TAG, "saved active custom profile for SSID '%s'", ssid ? ssid : "");
}

static void wifi_clear_custom_profile() {
    Preferences prefs;
    if (!prefs.begin(WIFI_PREFS_NS, false)) {
        ESP_LOGW(TAG, "failed to open NVS for clearing Wi-Fi profile");
        return;
    }

    prefs.remove(WIFI_PREF_SSID_KEY);
    prefs.remove(WIFI_PREF_PASS_KEY);
    prefs.end();
    ESP_LOGI(TAG, "cleared active custom profile");
}

// arduino-esp32 3.x defaults sae_pwe_h2e to WPA3_SAE_PWE_BOTH, which WPA2/WPA3
// transition APs (e.g. UniFi) require; nothing extra to configure since the
// core 2.x -> 3.x migration.
static void wifi_connect_with_h2e(const char* ssid, const char* password) {
    WiFi.begin(ssid, password);
}

static bool wifi_start_connection(const char* ssid, const char* password, bool custom_profile_active) {
    if (!g_wifi.store || !ssid || ssid[0] == '\0') {
        return false;
    }

    copy_string(g_wifi.active_ssid, sizeof(g_wifi.active_ssid), ssid);
    copy_string(g_wifi.active_password, sizeof(g_wifi.active_password), password ? password : "");
    g_wifi.active_custom_profile = custom_profile_active;
    g_wifi.recovery_requested = false;
    g_wifi.recovery_reason = 0;
    g_wifi.consecutive_disconnects = 0;
    g_wifi.consecutive_auth_fails = 0;
    g_wifi.reconnect_pause_until_ms = 0;
    g_wifi.pause_reconnect_for_scan = false;
    store_set_wifi_profile(g_wifi.store, g_wifi.active_ssid, g_wifi.active_custom_profile);

    ESP_LOGI(TAG, "connecting to SSID '%s' (%s profile)", ssid, custom_profile_active ? "custom" : "default");
    store_set_wifi_connecting(g_wifi.store, true);
    store_set_wifi_connect_error(g_wifi.store, nullptr);
    store_set_wifi_state(g_wifi.store, ConnState::Initializing);
    store_set_wifi_connection_info(g_wifi.store, false, "", "", -127);

    WiFi.setAutoReconnect(true);
    WiFi.disconnect(false, true);
    delay(120);
    wifi_connect_with_h2e(ssid, password ? password : "");
    return true;
}

static void wifi_resume_connection_after_scan() {
    if (!g_wifi.pause_reconnect_for_scan) {
        return;
    }
    g_wifi.pause_reconnect_for_scan = false;

    if (WiFi.status() == WL_CONNECTED || g_wifi.active_ssid[0] == '\0') {
        return;
    }

    ESP_LOGI(TAG, "resuming connection to SSID '%s' after scan", g_wifi.active_ssid);
    store_set_wifi_connecting(g_wifi.store, true);
    store_set_wifi_state(g_wifi.store, ConnState::Initializing);
    wifi_connect_with_h2e(g_wifi.active_ssid, g_wifi.active_password);
}

static void wifi_request_recovery(uint8_t reason) {
    g_wifi.recovery_requested = true;
    g_wifi.recovery_reason = reason;
}

static void wifi_perform_recovery() {
    if (!g_wifi.store || g_wifi.active_ssid[0] == '\0') {
        return;
    }

    ESP_LOGW(TAG, "performing Wi-Fi recovery (reason=%u, ssid=%s)", static_cast<unsigned>(g_wifi.recovery_reason), g_wifi.active_ssid);

    store_set_wifi_connecting(g_wifi.store, true);
    store_set_wifi_connect_error(g_wifi.store, "Recovering Wi-Fi...");
    store_set_wifi_state(g_wifi.store, ConnState::Initializing);

    g_wifi.deep_resets_since_connect++;
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, true);
    delay(120);
    // Full stop + esp_wifi_deinit: auth loops can poison the driver session in a
    // way only a from-scratch init (or reboot) clears
    WiFi.mode(WIFI_OFF);
    delay(200);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    power_apply_wifi_sleep();
    WiFi.setTxPower(WIFI_TX_POWER);
    wifi_connect_with_h2e(g_wifi.active_ssid, g_wifi.active_password);
}

static bool wifi_reason_invalid_credentials(uint8_t reason) {
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
        return true;
    default:
        return false;
    }
}

static const char* wifi_reason_message(uint8_t reason) {
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_AUTH_FAIL:
        return "Authentication failed";
    case WIFI_REASON_ASSOC_FAIL:
        return "Association failed";
    case WIFI_REASON_NO_AP_FOUND:
        return "Network not found";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "Handshake timeout";
    default:
        return "Connection lost";
    }
}

static void wifi_refresh_connection_info() {
    if (!g_wifi.store) {
        return;
    }
    const bool connected = WiFi.status() == WL_CONNECTED;
    if (!connected) {
        store_set_wifi_connection_info(g_wifi.store, false, "", "", -127);
        return;
    }

    String ssid = WiFi.SSID();
    String ip = WiFi.localIP().toString();
    int16_t rssi = static_cast<int16_t>(WiFi.RSSI());
    store_set_wifi_connection_info(g_wifi.store, true, ssid.c_str(), ip.c_str(), rssi);
}

static void wifi_start_scan_now() {
    if (!g_wifi.store) {
        return;
    }

    g_wifi.pause_reconnect_for_scan = WiFi.status() != WL_CONNECTED;
    if (g_wifi.pause_reconnect_for_scan) {
        ESP_LOGI(TAG, "pausing reconnect while scanning");
        WiFi.disconnect(false, false);
        delay(80);
    }

    WiFi.scanDelete();
    int16_t rc = WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/true, /*passive=*/false, /*max_ms_per_chan=*/500);
    ESP_LOGI(TAG, "scan start rc=%d", rc);
    if (rc == WIFI_SCAN_RUNNING || rc >= 0) {
        g_wifi.scan_running = true;
        store_set_wifi_scan_state(g_wifi.store, true);
    } else {
        g_wifi.scan_running = false;
        store_set_wifi_scan_state(g_wifi.store, false);
        store_set_wifi_connect_error(g_wifi.store, "Wi-Fi scan failed");
        wifi_resume_connection_after_scan();
    }
}

static void wifi_handle_scan_complete(int16_t count) {
    if (!g_wifi.store) {
        return;
    }

    WifiNetwork networks[MAX_WIFI_NETWORKS] = {};
    uint8_t stored_count = 0;

    for (int16_t i = 0; i < count && stored_count < MAX_WIFI_NETWORKS; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) {
            continue;
        }

        int16_t rssi = static_cast<int16_t>(WiFi.RSSI(i));
        bool secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;

        int16_t existing_idx = -1;
        for (uint8_t idx = 0; idx < stored_count; idx++) {
            if (strcmp(networks[idx].ssid, ssid.c_str()) == 0) {
                existing_idx = static_cast<int16_t>(idx);
                break;
            }
        }

        if (existing_idx >= 0) {
            WifiNetwork& existing = networks[existing_idx];
            if (rssi > existing.rssi) {
                existing.rssi = rssi;
            }
            existing.secure = existing.secure || secure;
            continue;
        }

        WifiNetwork& out = networks[stored_count++];
        copy_string(out.ssid, sizeof(out.ssid), ssid.c_str());
        out.rssi = rssi;
        out.secure = secure;
    }

    for (uint8_t i = 0; i < stored_count; i++) {
        for (uint8_t j = static_cast<uint8_t>(i + 1); j < stored_count; j++) {
            if (networks[j].rssi > networks[i].rssi) {
                WifiNetwork tmp = networks[i];
                networks[i] = networks[j];
                networks[j] = tmp;
            }
        }
    }

    WifiHistoryEntry history[MAX_WIFI_SAVED_NETWORKS];
    uint8_t history_count = wifi_load_history(history, MAX_WIFI_SAVED_NETWORKS);
    for (uint8_t i = 0; i < stored_count; i++) {
        networks[i].known = false;
        for (uint8_t j = 0; j < history_count; j++) {
            if (strcmp(networks[i].ssid, history[j].ssid) == 0) {
                networks[i].known = true;
                break;
            }
        }
    }

    ESP_LOGI(TAG, "scan complete raw=%d unique=%u", count, static_cast<unsigned>(stored_count));
    store_set_wifi_scan_results(g_wifi.store, networks, stored_count);
    store_set_wifi_scan_state(g_wifi.store, false);
    store_set_wifi_connect_error(g_wifi.store, nullptr);
    WiFi.scanDelete();
    g_wifi.scan_running = false;
    wifi_resume_connection_after_scan();
}

void launch_wifi(Configuration* config, EntityStore* store) {
    g_wifi.config = config;
    g_wifi.store = store;
    if (esp_reset_reason() != ESP_RST_SW) {
        g_wifi_auto_restart_count = 0; // RTC_NOINIT memory is garbage on power-on
    }
    g_wifi.scan_requested = true;
    g_wifi.scan_running = false;
    g_wifi.recovery_requested = false;
    g_wifi.recovery_reason = 0;
    g_wifi.last_info_refresh_ms = 0;
    g_wifi.last_recovery_ms = 0;
    g_wifi.consecutive_disconnects = 0;
    g_wifi.active_custom_profile = false;
    g_wifi.active_ssid[0] = '\0';
    g_wifi.active_password[0] = '\0';
    g_wifi.boot_connect_started_ms = millis();
    g_wifi.boot_settings_fallback_shown = false;
    g_wifi.pause_reconnect_for_scan = false;

    WiFi.onEvent([store](WiFiEvent_t event, WiFiEventInfo_t info) {
        ESP_LOGI(TAG, "received wifi event: %d", event);

        switch (event) {
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            ESP_LOGI(TAG, "associated (authmode=%d)", static_cast<int>(info.wifi_sta_connected.authmode));
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            ESP_LOGI(TAG, "obtained IP address");
            g_wifi.consecutive_disconnects = 0;
            g_wifi.consecutive_auth_fails = 0;
            g_wifi.deep_resets_since_connect = 0;
            g_wifi.backoffs_since_connect = 0;
            g_wifi_auto_restart_count = 0;
            g_wifi.recovery_requested = false;
            g_wifi.recovery_reason = 0;
            g_wifi.boot_settings_fallback_shown = true;
            g_wifi.pause_reconnect_for_scan = false;
            store_set_wifi_state(store, ConnState::Up);
            store_set_wifi_connecting(store, false);
            store_set_wifi_connect_error(store, nullptr);
            wifi_refresh_connection_info();
            if (g_wifi.boot_settings_auto_opened) {
                // The startup fallback opened Wi-Fi settings; leave it once connected
                g_wifi.boot_settings_auto_opened = false;
                if (store_close_wifi_settings_if_open(store)) {
                    ESP_LOGI(TAG, "closing auto-opened Wi-Fi settings after connect");
                }
            }
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
            const uint8_t reason = info.wifi_sta_disconnected.reason;
            const bool auth_fail = wifi_reason_invalid_credentials(reason);
            const bool auth_expired = reason == WIFI_REASON_AUTH_EXPIRE;
            const bool self_inflicted = reason == WIFI_REASON_ASSOC_LEAVE || reason == WIFI_REASON_AUTH_LEAVE ||
                                        reason == WIFI_REASON_STA_LEAVING;
            ESP_LOGI(TAG, "disconnected (reason=%d)", reason);
            if (!self_inflicted) { // our own resets/disconnects must not escalate the ladder
                g_wifi.consecutive_disconnects++;
            }
            g_wifi.consecutive_auth_fails = auth_fail ? static_cast<uint8_t>(g_wifi.consecutive_auth_fails + 1) : 0;
            store_set_wifi_connection_info(store, false, "", "", -127);

            if (g_wifi.pause_reconnect_for_scan) {
                ESP_LOGI(TAG, "disconnect happened during scan, reconnect paused");
                break;
            }

            if (g_wifi.reconnect_pause_until_ms != 0 || g_wifi.restart_requested) {
                break; // backing off: stay quiet, wifi_poll resumes when the pause elapses
            }

            if (auth_fail && g_wifi.consecutive_auth_fails >= WIFI_AUTH_FAILS_FOR_BAD_CREDENTIALS) {
                ESP_LOGW(TAG, "auth failed %u times for SSID '%s'; keeping saved credentials for manual retry/edit",
                         static_cast<unsigned>(g_wifi.consecutive_auth_fails), g_wifi.active_ssid);
                store_set_wifi_connecting(store, false);
                store_set_wifi_state(store, ConnState::InvalidCredentials);
                store_set_wifi_connect_error(store, wifi_reason_message(reason));
                break;
            }

            store_set_wifi_connect_error(store, wifi_reason_message(reason));

            const bool auth_issue = auth_expired || auth_fail;
            if (auth_issue && g_wifi.consecutive_disconnects >= 3 &&
                g_wifi.deep_resets_since_connect < WIFI_DEEP_RESETS_BEFORE_BACKOFF) {
                // Deep driver reset first: it is fast and clears the poisoned session state.
                uint32_t now = millis();
                if (now - g_wifi.last_recovery_ms > 8000) {
                    g_wifi.last_recovery_ms = now;
                    store_set_wifi_connecting(store, true);
                    store_set_wifi_state(store, ConnState::Initializing);
                    wifi_request_recovery(reason);
                }
            } else if ((auth_issue && g_wifi.consecutive_disconnects >= 3) || g_wifi.consecutive_disconnects >= 5) {
                if (auth_issue && g_wifi.backoffs_since_connect >= WIFI_BACKOFFS_BEFORE_RESTART &&
                    g_wifi_auto_restart_count < WIFI_MAX_AUTO_RESTARTS) {
                    // Deep resets and quiet periods both failed: reboot clears
                    // device-side poisoning outages.
                    g_wifi.restart_requested = true;
                } else {
                    // Pause so any AP-side session state can expire, and escape the
                    // Boot screen so the user can tap Retry or open Wi-Fi Settings.
                    g_wifi.recovery_requested = false; // a queued deep reset would undo the quiet period
                    const uint32_t backoff_ms =
                        g_wifi.backoffs_since_connect == 0 ? WIFI_RECONNECT_BACKOFF_MS : WIFI_RECONNECT_BACKOFF_LONG_MS;
                    g_wifi.backoffs_since_connect++;
                    g_wifi.reconnect_pause_until_ms = millis() + backoff_ms;
                    WiFi.setAutoReconnect(false); // the core would otherwise keep retrying on its own
                    ESP_LOGW(TAG, "%u consecutive failures; backing off reconnect for %lu ms",
                             static_cast<unsigned>(g_wifi.consecutive_disconnects), static_cast<unsigned long>(backoff_ms));
                }
                store_set_wifi_connecting(store, false);
                store_set_wifi_state(store, ConnState::ConnectionError);
            } else {
                store_set_wifi_connecting(store, true);
                store_set_wifi_state(store, ConnState::Initializing);
                WiFi.reconnect();
            }
            break;
        }
        case ARDUINO_EVENT_WIFI_STA_LOST_IP:
            ESP_LOGI(TAG, "lost IP, reconnecting");
            store_set_wifi_connecting(store, false);
            store_set_wifi_state(store, ConnState::ConnectionError);
            store_set_wifi_connect_error(store, "Lost IP address");
            store_set_wifi_connection_info(store, false, "", "", -127);
            WiFi.reconnect();
            break;
        default:
            break;
        }
    });

    char boot_ssid[MAX_WIFI_SSID_LEN] = {0};
    char boot_password[MAX_WIFI_PASSWORD_LEN + 1] = {0};
    bool use_custom_profile = false;
    if (wifi_load_saved_profile(boot_ssid, sizeof(boot_ssid), boot_password, sizeof(boot_password))) {
        use_custom_profile = true;
        ESP_LOGI(TAG, "loaded saved custom profile for SSID '%s'", boot_ssid);
    } else {
        copy_string(boot_ssid, sizeof(boot_ssid), config->wifi_ssid ? config->wifi_ssid : "");
        copy_string(boot_password, sizeof(boot_password), config->wifi_password ? config->wifi_password : "");
        ESP_LOGI(TAG, "no saved custom profile, using default SSID '%s'", boot_ssid);
    }

    if (boot_ssid[0] == '\0') {
        ESP_LOGE(TAG, "no Wi-Fi SSID configured");
        store_set_wifi_state(store, ConnState::ConnectionError);
        store_set_wifi_connect_error(store, "No Wi-Fi SSID configured");
        return;
    }

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, true);
    WiFi.setAutoReconnect(true);
    power_apply_wifi_sleep();
    WiFi.setTxPower(WIFI_TX_POWER);

    wifi_start_connection(boot_ssid, boot_password, use_custom_profile);
    store_set_wifi_scan_state(store, false);
    wifi_refresh_connection_info();
}

void wifi_request_scan() {
    g_wifi.scan_requested = true;
}

void wifi_debug_report(char* out, size_t out_len) {
    const uint32_t now = millis();
    const long pause_left =
        g_wifi.reconnect_pause_until_ms == 0 ? 0 : static_cast<long>(g_wifi.reconnect_pause_until_ms - now);
    snprintf(out, out_len,
             "status=%d ssid='%s' profile=%s ip=%s rssi=%d fails=%u auth_fails=%u deep_resets=%u backoffs=%u "
             "backoff_left_ms=%ld auto_restarts=%lu",
             static_cast<int>(WiFi.status()), g_wifi.active_ssid, g_wifi.active_custom_profile ? "custom" : "default",
             WiFi.localIP().toString().c_str(), static_cast<int>(WiFi.RSSI()), g_wifi.consecutive_disconnects,
             g_wifi.consecutive_auth_fails, g_wifi.deep_resets_since_connect, g_wifi.backoffs_since_connect,
             pause_left > 0 ? pause_left : 0, static_cast<unsigned long>(g_wifi_auto_restart_count));
}

void wifi_force_recovery() {
    g_wifi.reconnect_pause_until_ms = 0;
    g_wifi.recovery_requested = true;
    g_wifi.recovery_reason = 0;
}

bool wifi_connect_to_network(const char* ssid, const char* password) {
    if (!g_wifi.store || !ssid || ssid[0] == '\0') {
        return false;
    }

    wifi_add_to_history(ssid, password ? password : "");
    wifi_save_custom_profile(ssid, password ? password : "");
    return wifi_start_connection(ssid, password, true);
}

bool wifi_reset_to_default() {
    if (!g_wifi.store || !g_wifi.config || !g_wifi.config->wifi_ssid || g_wifi.config->wifi_ssid[0] == '\0') {
        return false;
    }

    wifi_clear_custom_profile();
    return wifi_start_connection(g_wifi.config->wifi_ssid, g_wifi.config->wifi_password, false);
}

bool wifi_reconnect() {
    if (!g_wifi.store || g_wifi.active_ssid[0] == '\0') {
        return false;
    }
    g_wifi.consecutive_disconnects = 0;
    return wifi_start_connection(g_wifi.active_ssid, g_wifi.active_password, g_wifi.active_custom_profile);
}

void wifi_poll() {
    if (!g_wifi.store) {
        return;
    }

    if (g_wifi.restart_requested) {
        g_wifi_auto_restart_count++;
        ESP_LOGE(TAG, "Wi-Fi unrecoverable after resets and backoff; restarting device (%lu/%lu)",
                 static_cast<unsigned long>(g_wifi_auto_restart_count), static_cast<unsigned long>(WIFI_MAX_AUTO_RESTARTS));
        delay(100);
        esp_restart();
    }

    if (!g_wifi.boot_settings_fallback_shown) {
        uint32_t now = millis();
        const bool timeout_elapsed = static_cast<uint32_t>(now - g_wifi.boot_connect_started_ms) >= WIFI_BOOT_SETTINGS_FALLBACK_MS;
        const bool connected = WiFi.status() == WL_CONNECTED;
        if (timeout_elapsed && !connected) {
            g_wifi.boot_settings_fallback_shown = true;
            g_wifi.boot_settings_auto_opened = true;
            ESP_LOGW(TAG, "startup Wi-Fi connect timed out after %lu ms; opening Wi-Fi settings", static_cast<unsigned long>(now - g_wifi.boot_connect_started_ms));
            store_open_wifi_settings(g_wifi.store);
            g_wifi.scan_requested = true;
            store_set_wifi_connect_error(g_wifi.store, "Select a Wi-Fi network");
        }
    }

    if (g_wifi.reconnect_pause_until_ms != 0 && !g_wifi.pause_reconnect_for_scan) {
        uint32_t now = millis();
        if (static_cast<int32_t>(now - g_wifi.reconnect_pause_until_ms) >= 0) {
            g_wifi.reconnect_pause_until_ms = 0;
            if (WiFi.status() != WL_CONNECTED && g_wifi.active_ssid[0] != '\0') {
                ESP_LOGI(TAG, "backoff elapsed, retrying SSID '%s'", g_wifi.active_ssid);
                wifi_start_connection(g_wifi.active_ssid, g_wifi.active_password, g_wifi.active_custom_profile);
            }
        }
    }

    if (g_wifi.recovery_requested) {
        g_wifi.recovery_requested = false;
        wifi_perform_recovery();
    }

    if (g_wifi.scan_requested && !g_wifi.scan_running) {
        g_wifi.scan_requested = false;
        wifi_start_scan_now();
    }

    if (g_wifi.scan_running) {
        int16_t scan_state = WiFi.scanComplete();
        if (scan_state >= 0) {
            wifi_handle_scan_complete(scan_state);
        } else if (scan_state == WIFI_SCAN_FAILED) {
            g_wifi.scan_running = false;
            store_set_wifi_scan_state(g_wifi.store, false);
            store_set_wifi_connect_error(g_wifi.store, "Wi-Fi scan failed");
            WiFi.scanDelete();
            wifi_resume_connection_after_scan();
        }
    }

    uint32_t now = millis();
    if (now - g_wifi.last_info_refresh_ms >= 5000) {
        wifi_refresh_connection_info();
        g_wifi.last_info_refresh_ms = now;
    }
}
