#include "managers/console.h"
#include "managers/beacon.h"
#include "managers/power.h"
#include "managers/wifi.h"
#include "esp_system.h"
#include <Arduino.h>

// Line-based command console on USB serial: works when Wi-Fi (and thus the
// HTTP harness) is down. Commands: wifi | wifi retry | wifi reset | reboot | help

static void console_dispatch(const char* line) {
    if (strcmp(line, "wifi") == 0) {
        char report[320];
        wifi_debug_report(report, sizeof(report));
        Serial.printf("[console] %s\n", report);
    } else if (strcmp(line, "wifi retry") == 0) {
        Serial.println("[console] reconnecting");
        wifi_reconnect();
    } else if (strcmp(line, "wifi reset") == 0) {
        Serial.println("[console] requesting deep Wi-Fi reset");
        wifi_force_recovery();
    } else if (strcmp(line, "reboot") == 0) {
        Serial.println("[console] rebooting");
        Serial.flush();
        delay(100);
        esp_restart();
    } else if (strcmp(line, "beacon") == 0) {
        Serial.printf("[console] beacon=%s\n", beacon_status());
    } else if (strcmp(line, "beacon retry") == 0) {
        Serial.println("[console] clearing beacon wedge guard and rebooting");
        beacon_clear_guard();
        Serial.flush();
        delay(100);
        esp_restart();
    } else if (strcmp(line, "power") == 0) {
        char report[96];
        power_report(report, sizeof(report));
        Serial.printf("[console] %s\n", report);
    } else if (strcmp(line, "power sleep on") == 0) {
        power_set_modem_sleep(true);
        Serial.println("[console] modem sleep on");
    } else if (strcmp(line, "power sleep off") == 0) {
        power_set_modem_sleep(false);
        Serial.println("[console] modem sleep off");
    } else if (strncmp(line, "power cpu ", 10) == 0) {
        const uint32_t mhz = static_cast<uint32_t>(atoi(line + 10));
        Serial.printf("[console] idle cpu %lu MHz: %s\n", static_cast<unsigned long>(mhz),
                      power_set_idle_cpu_mhz(mhz) ? "ok" : "invalid (80|160|240)");
    } else if (strcmp(line, "help") == 0) {
        Serial.println("[console] commands: wifi | wifi retry | wifi reset | beacon | beacon retry | power | power sleep on/off | power cpu N | reboot");
    } else {
        Serial.printf("[console] unknown command '%s' (try help)\n", line);
    }
}

void console_init() {
    Serial.begin(115200);
}

void console_poll() {
    static char line[64];
    static size_t len = 0;

    while (Serial.available() > 0) {
        const char ch = static_cast<char>(Serial.read());
        if (ch == '\n' || ch == '\r') {
            if (len > 0) {
                line[len] = '\0';
                len = 0;
                console_dispatch(line);
            }
        } else if (len < sizeof(line) - 1) {
            line[len++] = ch;
        }
    }
}
