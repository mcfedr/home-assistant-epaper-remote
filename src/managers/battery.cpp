#include "managers/battery.h"
#include "boards.h"
#include "constants.h"
#include "esp_log.h"
#include <Arduino.h>
#include <Wire.h>

static const char* TAG = "battery";

// BQ27220 fuel gauge, standard commands (little-endian words)
static constexpr uint8_t BQ27220_ADDR = 0x55;
static constexpr uint8_t BQ27220_CMD_VOLTAGE = 0x08;       // mV
static constexpr uint8_t BQ27220_CMD_CURRENT = 0x0C;       // signed mA, positive = charging
static constexpr uint8_t BQ27220_CMD_STATE_OF_CHARGE = 0x2C; // percent

static bool bq27220_read_word(uint8_t command, uint16_t* out) {
    Wire.beginTransmission(BQ27220_ADDR);
    Wire.write(command);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    if (Wire.requestFrom(static_cast<int>(BQ27220_ADDR), 2) != 2) {
        return false;
    }
    const uint8_t low = Wire.read();
    const uint8_t high = Wire.read();
    *out = static_cast<uint16_t>(low | (high << 8));
    return true;
}

void battery_poll(EntityStore* store) {
    if (!HAS_BQ27220_FUEL_GAUGE) {
        return;
    }

    static uint32_t last_sample_ms = 0;
    const uint32_t now = millis();
    if (last_sample_ms != 0 && now - last_sample_ms < BATTERY_SAMPLE_INTERVAL_MS) {
        return;
    }
    last_sample_ms = now;

    uint16_t soc = 0;
    uint16_t millivolts = 0;
    uint16_t current_raw = 0;
    const bool ok = bq27220_read_word(BQ27220_CMD_STATE_OF_CHARGE, &soc) &&
                    bq27220_read_word(BQ27220_CMD_VOLTAGE, &millivolts) &&
                    bq27220_read_word(BQ27220_CMD_CURRENT, &current_raw);
    if (!ok) {
        ESP_LOGW(TAG, "fuel gauge read failed");
        store_set_battery(store, false, 0, 0, 0);
        return;
    }

    store_set_battery(store, true, static_cast<uint8_t>(soc > 100 ? 100 : soc), millivolts, static_cast<int16_t>(current_raw));
}
