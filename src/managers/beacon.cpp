#include "managers/beacon.h"
#include "constants.h"
#include "esp_log.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

#include <cstring>

static const char* TAG = "beacon";
static const char* volatile g_beacon_status = "off";

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
void launch_beacon(EntityStore* store) {
    (void)store;
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

const char* beacon_status() {
    return g_beacon_status;
}
