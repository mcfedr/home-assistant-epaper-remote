#pragma once
#include <FastEPD.h>
#include <cstddef>
#include <cstdint>

#ifdef TARGET_LILYGO_T5_S3_PRO
constexpr uint16_t DISPLAY_WIDTH = 540;
constexpr uint16_t DISPLAY_HEIGHT = 960;
constexpr size_t TOUCH_SDA = 39;
constexpr size_t TOUCH_SCL = 40;
constexpr size_t TOUCH_INT = 3;
constexpr size_t TOUCH_RST = 9;
constexpr int DISPLAY_PANEL = BB_PANEL_EPDIY_V7;
#endif

#ifdef TARGET_M5PAPER_S3
constexpr uint16_t DISPLAY_WIDTH = 540;
constexpr uint16_t DISPLAY_HEIGHT = 960;
constexpr size_t TOUCH_SDA = 41;
constexpr size_t TOUCH_SCL = 42;
constexpr size_t TOUCH_INT = 48;
constexpr size_t TOUCH_RST = 0;
constexpr int DISPLAY_PANEL = BB_PANEL_M5PAPERS3;
#endif
