/*
 * SPDX-License-Identifier: MIT
 */

#include "oralb_ble.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "sdkconfig.h"

static const char *TAG = "ble";

static const ble_uuid128_t s_uuid_model =
    BLE_UUID128_INIT(0x42, 0x2d, 0x6c, 0x61, 0x72, 0x4f, 0x08, 0x82,
                     0x53, 0x4d, 0x47, 0x50, 0x02, 0xff, 0xf0, 0xa0);
static const ble_uuid128_t s_uuid_state =
    BLE_UUID128_INIT(0x42, 0x2d, 0x6c, 0x61, 0x72, 0x4f, 0x08, 0x82,
                     0x53, 0x4d, 0x47, 0x50, 0x04, 0xff, 0xf0, 0xa0);
static const ble_uuid128_t s_uuid_battery =
    BLE_UUID128_INIT(0x42, 0x2d, 0x6c, 0x61, 0x72, 0x4f, 0x08, 0x82,
                     0x53, 0x4d, 0x47, 0x50, 0x05, 0xff, 0xf0, 0xa0);

static uint8_t s_own_addr_type;
static bool s_ready;
static bool s_scanning;
static bool s_connect_pending;
static bool s_connecting;
static bool s_transaction_complete;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static ble_addr_t s_peer_addr;

static uint16_t s_model_handle;
static uint16_t s_state_handle;
static uint16_t s_battery_handle;
static oralb_device_info_t s_pending_device;
static uint8_t s_pending_state;

static int64_t s_last_success_us;
static int64_t s_retry_after_us;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static oralb_ble_adv_t s_adv;
static oralb_ble_status_t s_status;

/* Optional address filter from Kconfig, in display order. */
static uint8_t s_filter[6];
static bool s_filter_set;

typedef enum {
    READ_STEP_NONE,
    READ_STEP_MODEL,
    READ_STEP_STATE,
    READ_STEP_BATTERY,
} read_step_t;

static read_step_t s_read_step;

static bool parse_addr(const char *text, uint8_t out[6])
{
    unsigned int bytes[6];
    if (sscanf(text, "%x:%x:%x:%x:%x:%x", &bytes[0], &bytes[1], &bytes[2],
               &bytes[3], &bytes[4], &bytes[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (bytes[i] > 0xFF) {
            return false;
        }
        out[i] = (uint8_t)bytes[i];
    }
    return true;
}

static bool addr_matches(const ble_addr_t *addr)
{
    if (!s_filter_set) {
        return true;
    }
    for (int i = 0; i < 6; i++) {
        /* ble_addr_t.val is little endian: val[0] is the last printed octet. */
        if (addr->val[5 - i] != s_filter[i]) {
            return false;
        }
    }
    return true;
}

static bool is_oralb_payload(const struct ble_hs_adv_fields *fields)
{
    if (fields->mfg_data == NULL ||
        fields->mfg_data_len != 2 + ORALB_ADV_LEN) {
        return false;
    }

    /* The Sense charger uses the same company id with a longer payload. */
    return fields->mfg_data[0] == (ORALB_MFG_ID & 0xFF) &&
           fields->mfg_data[1] == (ORALB_MFG_ID >> 8);
}

static void log_adv(const oralb_ble_adv_t *adv, bool first)
{
    static uint8_t last_state = 0xFF;
    static int64_t last_log_us;
    const int64_t now = esp_timer_get_time();

    if (!first && adv->adv.state == last_state &&
        now - last_log_us < 30 * 1000 * 1000) {
        return;
    }

    char addr[18];
    oralb_ble_addr_str(adv->addr, addr);

    ESP_LOGI(TAG,
             "%s state=%s(%u) model=%s proto=%u fw=%u rssi=%d "
             "brush=%u:%02u sector=%u/%u",
             addr, oralb_state_name(adv->adv.state), adv->adv.state,
             oralb_model_name(adv->adv.model), adv->adv.protocol,
             adv->adv.firmware, adv->rssi, adv->adv.time_minutes,
             adv->adv.time_seconds, adv->adv.sector, adv->adv.total_sectors);

    last_state = adv->adv.state;
    last_log_us = now;
}

static void start_scan(void);
static void start_connect(void);
static int gap_event(struct ble_gap_event *event, void *arg);
static void fail_poll(const char *operation, int status);

static bool poll_due(const oralb_ble_adv_t *adv, int64_t now_us)
{
    if (!oralb_state_is_quiet(adv->adv.state) ||
        now_us < s_retry_after_us) {
        return false;
    }

    if (s_last_success_us == 0) {
        return true;
    }

    const int interval_minutes =
        adv->adv.state == 4 ? CONFIG_APP_POLL_CHARGING_MINUTES
                            : CONFIG_APP_POLL_IDLE_MINUTES;
    const int64_t interval_us =
        (int64_t)interval_minutes * 60 * 1000 * 1000;
    return now_us - s_last_success_us >= interval_us;
}

static void handle_advertisement(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0) {
        return;
    }
    if (!is_oralb_payload(&fields) || !addr_matches(&disc->addr)) {
        return;
    }

    oralb_ble_adv_t adv = {0};
    if (!oralb_parse_adv(fields.mfg_data + 2, ORALB_ADV_LEN, &adv.adv)) {
        return;
    }

    adv.valid = true;
    adv.rssi = disc->rssi;
    adv.seen_us = esp_timer_get_time();
    for (int i = 0; i < 6; i++) {
        adv.addr[i] = disc->addr.val[5 - i];
    }

    taskENTER_CRITICAL(&s_lock);
    const bool first = !s_adv.valid;
    s_adv = adv;
    taskEXIT_CRITICAL(&s_lock);

    log_adv(&adv, first);

    if (!s_connect_pending && !s_connecting &&
        s_conn_handle == BLE_HS_CONN_HANDLE_NONE &&
        poll_due(&adv, adv.seen_us)) {
        s_peer_addr = disc->addr;
        s_connect_pending = true;
        ESP_LOGI(TAG, "quiet state %s; starting battery poll",
                 oralb_state_name(adv.adv.state));

        const int rc = ble_gap_disc_cancel();
        if (rc != 0) {
            s_connect_pending = false;
            s_retry_after_us = esp_timer_get_time() + 60 * 1000 * 1000;
            ESP_LOGE(TAG, "could not stop scan before connecting: %d", rc);
        } else {
            s_scanning = false;
            start_connect();
        }
    }
}

static uint16_t handle_for_step(read_step_t step)
{
    switch (step) {
    case READ_STEP_MODEL:
        return s_model_handle;
    case READ_STEP_STATE:
        return s_state_handle;
    case READ_STEP_BATTERY:
        return s_battery_handle;
    case READ_STEP_NONE:
    default:
        return 0;
    }
}

static void start_read(read_step_t step);

static int characteristic_discovered(uint16_t conn_handle,
                                     const struct ble_gatt_error *error,
                                     const struct ble_gatt_chr *chr,
                                     void *arg)
{
    (void)arg;
    if (error == NULL) {
        fail_poll("characteristic discovery", BLE_HS_EUNKNOWN);
        return 0;
    }

    if (error->status == 0 && chr != NULL) {
        if (ble_uuid_cmp(&chr->uuid.u, &s_uuid_model.u) == 0) {
            s_model_handle = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &s_uuid_state.u) == 0) {
            s_state_handle = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &s_uuid_battery.u) == 0) {
            s_battery_handle = chr->val_handle;
        }
        return 0;
    }

    if (error->status != BLE_HS_EDONE) {
        fail_poll("characteristic discovery", error->status);
        return 0;
    }

    if (s_model_handle == 0 || s_state_handle == 0 || s_battery_handle == 0) {
        ESP_LOGE(TAG, "brush is missing FF02, FF04, or FF05");
        fail_poll("required characteristic missing", BLE_HS_ENOENT);
        return 0;
    }

    ESP_LOGI(TAG, "found FF02/FF04/FF05 characteristics on handle %u",
             conn_handle);
    start_read(READ_STEP_MODEL);
    return 0;
}

static void read_failed(const struct ble_gatt_error *error)
{
    fail_poll("GATT read", error != NULL ? error->status : BLE_HS_EUNKNOWN);
}

static int characteristic_read(uint16_t conn_handle,
                               const struct ble_gatt_error *error,
                               struct ble_gatt_attr *attr,
                               void *arg)
{
    (void)conn_handle;
    (void)arg;

    if (error == NULL || error->status != 0 || attr == NULL || attr->om == NULL) {
        read_failed(error);
        return 0;
    }

    uint8_t value[32];
    uint16_t length = 0;
    const int rc = ble_hs_mbuf_to_flat(attr->om, value, sizeof(value), &length);
    if (rc != 0 || length == 0) {
        fail_poll("copying GATT value", rc != 0 ? rc : BLE_HS_EINVAL);
        return 0;
    }

    switch (s_read_step) {
    case READ_STEP_MODEL:
        if (!oralb_parse_device_info(value, length, &s_pending_device)) {
            fail_poll("decoding FF02", BLE_HS_EINVAL);
            return 0;
        }
        start_read(READ_STEP_STATE);
        return 0;

    case READ_STEP_STATE:
        s_pending_state = value[0];
        if (!oralb_state_is_quiet(s_pending_state)) {
            ESP_LOGW(TAG, "brush changed to %s before the read; skipping battery",
                     oralb_state_name(s_pending_state));
            s_retry_after_us = esp_timer_get_time() + 60 * 1000 * 1000;
            s_transaction_complete = true;
            (void)ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        start_read(READ_STEP_BATTERY);
        return 0;

    case READ_STEP_BATTERY: {
        oralb_battery_t battery;
        oralb_parse_battery(value, length, s_pending_device.protocol_version,
                            &battery);
        if (!battery.has_percent) {
            fail_poll("decoding FF05 battery percentage", BLE_HS_EINVAL);
            return 0;
        }

        taskENTER_CRITICAL(&s_lock);
        s_status.valid = true;
        s_status.device = s_pending_device;
        s_status.battery = battery;
        s_status.state = s_pending_state;
        s_status.updated_us = esp_timer_get_time();
        s_status.generation++;
        taskEXIT_CRITICAL(&s_lock);

        s_last_success_us = esp_timer_get_time();
        s_retry_after_us = 0;
        s_transaction_complete = true;
        ESP_LOGI(TAG, "battery %u%%, state %s", battery.percent,
                 oralb_state_name(s_pending_state));
        (void)ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    case READ_STEP_NONE:
    default:
        fail_poll("unexpected GATT read", BLE_HS_EINVAL);
        return 0;
    }
}

static void start_read(read_step_t step)
{
    s_read_step = step;
    const uint16_t handle = handle_for_step(step);
    const int rc = handle == 0
                       ? BLE_HS_ENOENT
                       : ble_gattc_read(s_conn_handle, handle,
                                        characteristic_read, NULL);
    if (rc != 0) {
        fail_poll("starting GATT read", rc);
    }
}

static void fail_poll(const char *operation, int status)
{
    ESP_LOGE(TAG, "%s failed (NimBLE status %d)", operation, status);
    s_retry_after_us = esp_timer_get_time() + 60 * 1000 * 1000;
    s_transaction_complete = true;
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        const int rc =
            ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0) {
            ESP_LOGE(TAG, "could not terminate failed poll connection: %d", rc);
        }
    }
}

static void start_connect(void)
{
    s_connect_pending = false;
    s_connecting = true;
    s_transaction_complete = false;
    s_model_handle = 0;
    s_state_handle = 0;
    s_battery_handle = 0;
    s_read_step = READ_STEP_NONE;
    memset(&s_pending_device, 0, sizeof(s_pending_device));

    const int rc = ble_gap_connect(s_own_addr_type, &s_peer_addr, 15000, NULL,
                                   gap_event, NULL);
    if (rc != 0) {
        s_connecting = false;
        s_retry_after_us = esp_timer_get_time() + 60 * 1000 * 1000;
        ESP_LOGE(TAG, "could not start brush connection: %d", rc);
        start_scan();
    }
}

static void start_scan(void)
{
    if (s_scanning || s_connecting ||
        s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    struct ble_gap_disc_params params = {
        .itvl = 0, /* controller defaults: 100 ms interval, 100 ms window */
        .window = 0,
        .filter_duplicates = 0,
        .passive = 1,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0,
    };

    const int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params,
                                gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
        return;
    }
    s_scanning = true;
    taskENTER_CRITICAL(&s_lock);
    s_ready = true;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "scanning for Oral-B advertisements");
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        handle_advertisement(&event->disc);
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        if (s_connect_pending) {
            start_connect();
        } else if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE && !s_connecting) {
            start_scan();
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        s_connecting = false;
        if (event->connect.status != 0) {
            s_retry_after_us = esp_timer_get_time() + 60 * 1000 * 1000;
            ESP_LOGW(TAG, "brush connection failed: %d",
                     event->connect.status);
            start_scan();
            return 0;
        }

        s_conn_handle = event->connect.conn_handle;
        ESP_LOGI(TAG, "connected; discovering Oral-B characteristics");
        {
            const int rc = ble_gattc_disc_all_chrs(
                s_conn_handle, 1, 0xffff, characteristic_discovered, NULL);
            if (rc != 0) {
                fail_poll("starting characteristic discovery", rc);
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "brush disconnected (reason %d)",
                 event->disconnect.reason);
        if (!s_transaction_complete) {
            s_retry_after_us = esp_timer_get_time() + 60 * 1000 * 1000;
            ESP_LOGW(TAG, "poll ended before battery data was read");
        }
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_read_step = READ_STEP_NONE;
        s_transaction_complete = false;
        start_scan();
        return 0;

    default:
        return 0;
    }
}

static void on_sync(void)
{
    const int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "no usable BLE address: %d", rc);
        return;
    }
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "cannot infer own address type");
        return;
    }

    start_scan();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "host reset, reason %d", reason);
    taskENTER_CRITICAL(&s_lock);
    s_ready = false;
    taskEXIT_CRITICAL(&s_lock);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t oralb_ble_init(void)
{
    if (CONFIG_APP_BRUSH_ADDR[0] != '\0') {
        if (parse_addr(CONFIG_APP_BRUSH_ADDR, s_filter)) {
            s_filter_set = true;
            ESP_LOGI(TAG, "address filter %s", CONFIG_APP_BRUSH_ADDR);
        } else {
            ESP_LOGW(TAG, "ignoring malformed address filter \"%s\"",
                     CONFIG_APP_BRUSH_ADDR);
        }
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

bool oralb_ble_get_adv(oralb_ble_adv_t *out)
{
    if (out == NULL) {
        return false;
    }

    taskENTER_CRITICAL(&s_lock);
    *out = s_adv;
    taskEXIT_CRITICAL(&s_lock);
    return out->valid;
}

bool oralb_ble_get_status(oralb_ble_status_t *out)
{
    if (out == NULL) {
        return false;
    }

    taskENTER_CRITICAL(&s_lock);
    *out = s_status;
    taskEXIT_CRITICAL(&s_lock);
    return out->valid;
}

bool oralb_ble_is_ready(void)
{
    taskENTER_CRITICAL(&s_lock);
    const bool ready = s_ready;
    taskEXIT_CRITICAL(&s_lock);
    return ready;
}

void oralb_ble_addr_str(const uint8_t addr[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", addr[0], addr[1],
             addr[2], addr[3], addr[4], addr[5]);
}
