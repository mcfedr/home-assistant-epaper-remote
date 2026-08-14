#pragma once

void launch_beacon();
void beacon_mark_boot_healthy(); // clears the wedge guard once a boot proves stable
void beacon_clear_guard();       // console: re-arm after a guarded (skipped) boot
void beacon_stop();              // stop advertising (before deep sleep)
const char* beacon_status();     // "off" | "guarded" | "starting" | "advertising" | "error"
