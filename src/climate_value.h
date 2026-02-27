#pragma once

#include <cstdint>

enum class ClimateMode : uint8_t {
    Off = 0,
    Heat = 1,
    Cool = 2,
};

constexpr uint8_t CLIMATE_MODE_MASK_OFF = 1 << 0;
constexpr uint8_t CLIMATE_MODE_MASK_HEAT = 1 << 1;
constexpr uint8_t CLIMATE_MODE_MASK_COOL = 1 << 2;
constexpr uint8_t CLIMATE_MODE_MASK_DEFAULT = CLIMATE_MODE_MASK_OFF | CLIMATE_MODE_MASK_HEAT | CLIMATE_MODE_MASK_COOL;

constexpr float CLIMATE_TEMP_MIN_C = 10.0f;
constexpr float CLIMATE_TEMP_MAX_C = 32.0f;
constexpr float CLIMATE_TEMP_STEP_C = 0.5f;
constexpr uint8_t CLIMATE_TEMP_MAX_STEPS = static_cast<uint8_t>((CLIMATE_TEMP_MAX_C - CLIMATE_TEMP_MIN_C) / CLIMATE_TEMP_STEP_C + 0.5f);

static_assert(CLIMATE_TEMP_MAX_STEPS <= 63, "Climate temperature steps must fit in 6 bits");

inline uint8_t climate_clamp_temp_steps(int32_t steps) {
    if (steps < 0) {
        return 0;
    }
    if (steps > CLIMATE_TEMP_MAX_STEPS) {
        return CLIMATE_TEMP_MAX_STEPS;
    }
    return static_cast<uint8_t>(steps);
}

inline uint8_t climate_pack_value(ClimateMode mode, uint8_t temp_steps) {
    uint8_t mode_bits = static_cast<uint8_t>(mode);
    if (mode_bits > 3) {
        mode_bits = 0;
    }
    return static_cast<uint8_t>((mode_bits << 6) | (climate_clamp_temp_steps(temp_steps) & 0x3f));
}

inline uint8_t climate_mode_mask_for_mode(ClimateMode mode) {
    switch (mode) {
    case ClimateMode::Heat:
        return CLIMATE_MODE_MASK_HEAT;
    case ClimateMode::Cool:
        return CLIMATE_MODE_MASK_COOL;
    case ClimateMode::Off:
    default:
        return CLIMATE_MODE_MASK_OFF;
    }
}

inline bool climate_is_mode_supported(uint8_t mode_mask, ClimateMode mode) {
    return (mode_mask & climate_mode_mask_for_mode(mode)) != 0;
}

inline uint8_t climate_normalize_mode_mask(uint8_t mode_mask) {
    mode_mask |= CLIMATE_MODE_MASK_OFF;
    if ((mode_mask & (CLIMATE_MODE_MASK_HEAT | CLIMATE_MODE_MASK_COOL)) == 0) {
        mode_mask |= CLIMATE_MODE_MASK_HEAT;
    }
    return mode_mask;
}

inline ClimateMode climate_default_enabled_mode(uint8_t mode_mask) {
    mode_mask = climate_normalize_mode_mask(mode_mask);
    if (mode_mask & CLIMATE_MODE_MASK_HEAT) {
        return ClimateMode::Heat;
    }
    if (mode_mask & CLIMATE_MODE_MASK_COOL) {
        return ClimateMode::Cool;
    }
    return ClimateMode::Off;
}

inline ClimateMode climate_unpack_mode(uint8_t value) {
    uint8_t mode_bits = static_cast<uint8_t>((value >> 6) & 0x03);
    if (mode_bits > static_cast<uint8_t>(ClimateMode::Cool)) {
        mode_bits = static_cast<uint8_t>(ClimateMode::Off);
    }
    return static_cast<ClimateMode>(mode_bits);
}

inline uint8_t climate_unpack_temp_steps(uint8_t value) {
    return climate_clamp_temp_steps(value & 0x3f);
}

inline float climate_steps_to_celsius(uint8_t temp_steps) {
    return CLIMATE_TEMP_MIN_C + static_cast<float>(climate_clamp_temp_steps(temp_steps)) * CLIMATE_TEMP_STEP_C;
}

inline uint8_t climate_celsius_to_steps(float celsius) {
    int32_t steps = static_cast<int32_t>((celsius - CLIMATE_TEMP_MIN_C) / CLIMATE_TEMP_STEP_C + 0.5f);
    return climate_clamp_temp_steps(steps);
}
