#include "boards.h"
#include "config_remote.h"
#include "constants.h"
#include "managers/beacon.h"
#include "managers/console.h"
#include "managers/harness.h"
#include "managers/home_assistant.h"
#include "managers/touch.h"
#include "managers/ui.h"
#include "managers/wifi.h"
#include "screen.h"
#include "store.h"
#include "ui_state.h"
#include "widgets/Slider.h"
#include <Arduino.h>
#include <FastEPD.h>
#include <esp_log.h>

static Configuration config;
static const char* TAG = "main";

static FASTEPD epaper;
static Screen screen;
static BBCapTouch bbct;
static EntityStore store;
static SharedUIState shared_ui_state;

static UITaskArgs ui_task_args;
static TouchTaskArgs touch_task_args;
static HomeAssistantTaskArgs hass_task_args;
static HarnessTaskArgs harness_task_args;

void setup() {
    console_init();

    // BLE beacon for Bermuda room presence. Must initialize before the panel
    // and Wi-Fi come up; guarded so a wedged boot disables it on the next one.
    launch_beacon();

    // Initialize objects
    store_init(&store);
    ui_state_init(&shared_ui_state);
    configure_remote(&config, &store, &screen);
    initialize_slider_sprites();

    // Initialize display
    epaper.initPanel(DISPLAY_PANEL);
    epaper.setPanelSize(DISPLAY_HEIGHT, DISPLAY_WIDTH);
    epaper.setRotation(90);
    epaper.setPasses(DISPLAY_PARTIAL_UPDATE_PASSES, DISPLAY_FULL_UPDATE_PASSES);
    epaper.einkPower(true); // FIXME: Disabling power makes the GT911 unavailable


    // Launch UI task
    ui_task_args.epaper = &epaper;
    ui_task_args.screen = &screen;
    ui_task_args.store = &store;
    ui_task_args.shared_state = &shared_ui_state;
    xTaskCreate(ui_task, "ui", 4096, &ui_task_args, 1, &store.ui_task);

    // Connect to wifi and launch watcher
    launch_wifi(&config, &store);

    // Connect to home assistant
    hass_task_args.config = &config;
    hass_task_args.store = &store;
    xTaskCreate(home_assistant_task, "home_assistant", 8192, &hass_task_args, 1, &store.home_assistant_task);

    // Launch touch task
    touch_task_args.bbct = &bbct;
    touch_task_args.screen = &screen;
    touch_task_args.state = &shared_ui_state;
    touch_task_args.store = &store;
    xTaskCreate(touch_task, "touch", 4096, &touch_task_args, 1, nullptr);

    // Launch test harness HTTP server
    harness_task_args.store = &store;
    harness_task_args.screen = &screen;
    harness_task_args.epaper = &epaper;
    harness_task_args.shared_state = &shared_ui_state;
    launch_harness(&harness_task_args);


    if (HOME_BUTTON_PIN >= 0) {
        if (HOME_BUTTON_ACTIVE_LOW) {
            pinMode(HOME_BUTTON_PIN, INPUT_PULLUP);
        } else {
            pinMode(HOME_BUTTON_PIN, INPUT);
        }
    }
}

void loop() {
    wifi_poll();
    console_poll();

    if (millis() > 60000) {
        beacon_mark_boot_healthy();
    }

    if (HOME_BUTTON_PIN >= 0) {
        static bool was_pressed = false;
        const int raw_level = digitalRead(HOME_BUTTON_PIN);
        const bool pressed = HOME_BUTTON_ACTIVE_LOW ? raw_level == LOW : raw_level == HIGH;

        if (pressed && !was_pressed) {
            ESP_LOGI(TAG, "Home button pressed");
            store_go_home(&store);
        }

        was_pressed = pressed;
    }

    vTaskDelay(pdMS_TO_TICKS(25));
}
