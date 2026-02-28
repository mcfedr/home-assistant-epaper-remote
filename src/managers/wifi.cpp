#include "wifi.h"
#include "config.h"
#include "esp_log.h"
#include "esp_wifi_types.h"
#include "store.h"
#include <Preferences.h>
#include <WiFi.h>
#include <cstring>

static const char* TAG = "wifi";
static constexpr const char* WIFI_PREFS_NS = "wifi";
static constexpr const char* WIFI_PREF_SSID_KEY = "ssid";
static constexpr const char* WIFI_PREF_PASS_KEY = "pass";

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
    bool active_custom_profile = false;
    char active_ssid[MAX_WIFI_SSID_LEN] = {0};
    char active_password[MAX_WIFI_PASSWORD_LEN + 1] = {0};
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
    store_set_wifi_profile(g_wifi.store, g_wifi.active_ssid, g_wifi.active_custom_profile);

    ESP_LOGI(TAG, "connecting to SSID '%s' (%s profile)", ssid, custom_profile_active ? "custom" : "default");
    store_set_wifi_connecting(g_wifi.store, true);
    store_set_wifi_connect_error(g_wifi.store, nullptr);
    store_set_wifi_state(g_wifi.store, ConnState::Initializing);
    store_set_wifi_connection_info(g_wifi.store, false, "", "", -127);

    WiFi.disconnect(false, true);
    delay(120);
    WiFi.begin(ssid, password ? password : "");
    return true;
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

    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, true);
    delay(180);
    WiFi.mode(WIFI_MODE_NULL);
    delay(120);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    WiFi.begin(g_wifi.active_ssid, g_wifi.active_password);
}

static bool wifi_reason_invalid_credentials(uint8_t reason) {
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
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

    WiFi.scanDelete();
    int16_t rc = WiFi.scanNetworks(true, true);
    if (rc == WIFI_SCAN_RUNNING || rc >= 0) {
        g_wifi.scan_running = true;
        store_set_wifi_scan_state(g_wifi.store, true);
    } else {
        g_wifi.scan_running = false;
        store_set_wifi_scan_state(g_wifi.store, false);
        store_set_wifi_connect_error(g_wifi.store, "Wi-Fi scan failed");
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

    store_set_wifi_scan_results(g_wifi.store, networks, stored_count);
    store_set_wifi_scan_state(g_wifi.store, false);
    store_set_wifi_connect_error(g_wifi.store, nullptr);
    WiFi.scanDelete();
    g_wifi.scan_running = false;
}

void launch_wifi(Configuration* config, EntityStore* store) {
    g_wifi.config = config;
    g_wifi.store = store;
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

    WiFi.onEvent([store](WiFiEvent_t event, WiFiEventInfo_t info) {
        ESP_LOGI(TAG, "received wifi event: %d", event);

        switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            ESP_LOGI(TAG, "obtained IP address");
            g_wifi.consecutive_disconnects = 0;
            g_wifi.recovery_requested = false;
            g_wifi.recovery_reason = 0;
            store_set_wifi_state(store, ConnState::Up);
            store_set_wifi_connecting(store, false);
            store_set_wifi_connect_error(store, nullptr);
            wifi_refresh_connection_info();
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
            const uint8_t reason = info.wifi_sta_disconnected.reason;
            const bool invalid_credentials = wifi_reason_invalid_credentials(reason);
            const bool auth_expired = reason == WIFI_REASON_AUTH_EXPIRE;
            ESP_LOGI(TAG, "disconnected (reason=%d)", reason);
            g_wifi.consecutive_disconnects++;
            store_set_wifi_connection_info(store, false, "", "", -127);

            if (invalid_credentials && g_wifi.active_custom_profile && g_wifi.config && g_wifi.config->wifi_ssid &&
                g_wifi.config->wifi_ssid[0] != '\0') {
                ESP_LOGW(TAG, "custom profile credentials failed; falling back to default profile");
                store_set_wifi_connect_error(store, "Custom Wi-Fi failed, using default");
                wifi_clear_custom_profile();
                wifi_start_connection(g_wifi.config->wifi_ssid, g_wifi.config->wifi_password, false);
                break;
            }

            if (invalid_credentials) {
                store_set_wifi_connecting(store, false);
                store_set_wifi_state(store, ConnState::InvalidCredentials);
                store_set_wifi_connect_error(store, wifi_reason_message(reason));
                break;
            }

            store_set_wifi_connecting(store, true);
            store_set_wifi_state(store, ConnState::Initializing);
            store_set_wifi_connect_error(store, wifi_reason_message(reason));

            // Recover from repeated AUTH_EXPIRE loops by resetting the STA driver and reconnecting.
            if (auth_expired && g_wifi.consecutive_disconnects >= 3) {
                uint32_t now = millis();
                if (now - g_wifi.last_recovery_ms > 8000) {
                    g_wifi.last_recovery_ms = now;
                    wifi_request_recovery(reason);
                }
            } else {
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
    } else {
        copy_string(boot_ssid, sizeof(boot_ssid), config->wifi_ssid ? config->wifi_ssid : "");
        copy_string(boot_password, sizeof(boot_password), config->wifi_password ? config->wifi_password : "");
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
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);

    wifi_start_connection(boot_ssid, boot_password, use_custom_profile);
    store_set_wifi_scan_state(store, false);
    wifi_refresh_connection_info();
}

void wifi_request_scan() {
    g_wifi.scan_requested = true;
}

bool wifi_connect_to_network(const char* ssid, const char* password) {
    if (!g_wifi.store || !ssid || ssid[0] == '\0') {
        return false;
    }

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

void wifi_poll() {
    if (!g_wifi.store) {
        return;
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
        }
    }

    uint32_t now = millis();
    if (now - g_wifi.last_info_refresh_ms >= 5000) {
        wifi_refresh_connection_info();
        g_wifi.last_info_refresh_ms = now;
    }
}
