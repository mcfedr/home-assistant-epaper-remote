#include "managers/beacon.h"
#include "constants.h"
#include "esp_log.h"
#include <Preferences.h>

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

#include <cstring>

// The Arduino core releases the BT controller's memory at boot unless this
// weak symbol reports Bluetooth in use; without it btdm_controller_init
// crashes freeing the already-released region.
extern "C" bool btInUse() {
    return true;
}

static const char* TAG = "beacon";
static const char* volatile g_beacon_status = "off";
static bool g_beacon_attempted = false;
static bool g_boot_marked_healthy = false;

static constexpr const char* BEACON_PREFS_NS = "beacon";
static constexpr const char* BEACON_GUARD_KEY = "guard";

// The guard flag is set in NVS before the risky BT init and cleared only once
// the boot proves stable. A boot that wedged leaves it set, so the next boot
// (after a plain power cycle) skips BLE and comes up healthy and flashable.
static void beacon_set_guard(uint8_t value) {
    Preferences prefs;
    if (prefs.begin(BEACON_PREFS_NS, false)) {
        prefs.putUChar(BEACON_GUARD_KEY, value);
        prefs.end();
    }
}

static uint8_t beacon_get_guard() {
    Preferences prefs;
    if (!prefs.begin(BEACON_PREFS_NS, true)) {
        return 0;
    }
    uint8_t value = prefs.getUChar(BEACON_GUARD_KEY, 0);
    prefs.end();
    return value;
}

void beacon_mark_boot_healthy() {
    if (!g_beacon_attempted || g_boot_marked_healthy) {
        return;
    }
    g_boot_marked_healthy = true;
    beacon_set_guard(0);
    ESP_LOGI(TAG, "boot healthy, wedge guard cleared");
}

void beacon_clear_guard() {
    beacon_set_guard(0);
}

static void beacon_start_advertising() {
    ble_svc_gap_device_name_set(BEACON_NAME);

    ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = reinterpret_cast<const uint8_t*>(BEACON_NAME);
    fields.name_len = strlen(BEACON_NAME);
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields rc=%d", rc);
        g_beacon_status = "error";
        return;
    }

    ble_gap_adv_params adv_params = {};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_NON;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(BEACON_ADV_INTERVAL_MIN_MS);
    adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(BEACON_ADV_INTERVAL_MAX_MS);
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, nullptr, BLE_HS_FOREVER, &adv_params, nullptr, nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start rc=%d", rc);
        g_beacon_status = "error";
        return;
    }

    g_beacon_status = "advertising";
    ESP_LOGI(TAG, "advertising as '%s'", BEACON_NAME);
}

static void beacon_on_sync() {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr rc=%d", rc);
        g_beacon_status = "error";
        return;
    }
    beacon_start_advertising();
}

static void beacon_host_task(void*) {
    nimble_port_run(); // returns only on nimble_port_stop
    nimble_port_freertos_deinit();
}

// Must run BEFORE Wi-Fi starts connecting: initializing the BT controller while
// a Wi-Fi link is active disrupts the shared radio and drops the connection
// (observed as an immediate disconnect loop after every GOT_IP). Bringing both
// stacks up at boot lets the coexistence layer arbitrate from the start.
void launch_beacon() {
    if (beacon_get_guard() != 0) {
        ESP_LOGW(TAG, "previous boot did not reach healthy with BLE enabled; skipping beacon init");
        g_beacon_status = "guarded";
        return;
    }
    beacon_set_guard(1);
    g_beacon_attempted = true;

    g_beacon_status = "starting";
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        g_beacon_status = "error";
        return;
    }

    ble_hs_cfg.sync_cb = beacon_on_sync;
    nimble_port_freertos_init(beacon_host_task);
}

void beacon_stop() {
    if (strcmp(g_beacon_status, "advertising") != 0) {
        return;
    }
    ble_gap_adv_stop();
    g_beacon_status = "off";
}

const char* beacon_status() {
    return g_beacon_status;
}
