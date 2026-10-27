/*
 * SPDX-License-Identifier: MIT
 * Ported from oralb-ha v0.7.40 (protocol.py, const.py). See oralb_proto.h.
 */

#include "oralb_proto.h"

#include <string.h>

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

void oralb_parse_battery(const uint8_t *data, size_t len,
                         int protocol_version, oralb_battery_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    if (data == NULL || len == 0) {
        return;
    }

    if (data[0] <= 100) {
        out->percent = data[0];
        out->has_percent = true;
    }

    if (len >= 3) {
        const uint16_t seconds = le16(&data[1]);
        const bool unavailable =
            seconds == 0xFFFF || (protocol_version == 6 && seconds == 0);
        if (!unavailable) {
            out->runtime_s = seconds;
            out->has_runtime = true;
        }
    }

    if (len >= 5) {
        const uint16_t millivolts = le16(&data[3]);
        if (millivolts > 0 && millivolts < 10000) {
            out->voltage_mv = millivolts;
            out->has_voltage = true;
        }
    }

    if (len >= 7) {
        const int16_t milliamps = (int16_t)le16(&data[5]);
        if (milliamps != -1) {
            out->current_ma = milliamps;
            out->has_current = true;
        }
    }

    if (len >= 8) {
        const int8_t temperature = (int8_t)data[7];
        if (temperature >= -40 && temperature <= 125) {
            out->temperature_c = temperature;
            out->has_temperature = true;
        }
    }
}

bool oralb_parse_device_info(const uint8_t *data, size_t len,
                             oralb_device_info_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    if (data == NULL || len < 3) {
        return false;
    }

    out->model_id = data[0];
    out->protocol_version = data[1];
    out->firmware_revision = data[2];
    return true;
}

bool oralb_parse_adv(const uint8_t *mfg, size_t len, oralb_adv_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    if (mfg == NULL || len != ORALB_ADV_LEN) {
        return false;
    }

    out->protocol = mfg[0];
    out->model = mfg[1];
    out->firmware = mfg[2];
    out->state = mfg[3];
    out->pressure_flags = mfg[4];
    out->time_minutes = mfg[5];
    out->time_seconds = mfg[6];
    out->mode = mfg[7];
    out->sector = mfg[8];
    out->sector_timer = mfg[9];
    out->total_sectors = mfg[10];
    return true;
}

const char *oralb_state_name(uint8_t state)
{
    switch (state) {
    case 0:
        return "unknown";
    case 1:
        return "initializing";
    case 2:
        return "idle";
    case 3:
        return "running";
    case 4:
        return "charging";
    case 5:
        return "setup";
    case 6:
        return "flight_menu";
    case 7:
        return "charge_forbidden";
    case 8:
        return "selection_menu";
    case 9:
        return "session_summary";
    case 10:
        return "post_brushing_summary";
    case 113:
        return "final_test";
    case 114:
        return "pcb_test";
    case 115:
        return "sleeping";
    case 116:
        return "transport";
    case 117:
        return "calibration_test";
    default:
        return "unknown";
    }
}

const char *oralb_model_name(uint8_t model_id)
{
    if (model_id <= 2) {
        return "Triumph D36";
    }
    if (model_id >= 32 && model_id <= 34) {
        return "Genius Series D701";
    }
    if (model_id >= 39 && model_id <= 41) {
        return "Smart/Pro Series D700";
    }
    switch (model_id) {
    case 48:
    case 49:
    case 50:
    case 54:
        return "iO Series";
    case 52:
        return "iO Series 4";
    case 53:
        return "iO Series 5";
    default:
        break;
    }
    if (model_id >= 64 && model_id <= 70) {
        return "Smart Series D21";
    }
    if (model_id >= 80 && model_id <= 87) {
        return "Pro Series D601";
    }
    if ((model_id >= 112 && model_id <= 114) ||
        (model_id >= 117 && model_id <= 119)) {
        return "Genius X D706";
    }
    return "unknown";
}

bool oralb_state_is_awake(uint8_t state)
{
    switch (state) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 8:
    case 9:
    case 10:
        return true;
    default:
        return false;
    }
}

bool oralb_state_is_quiet(uint8_t state)
{
    switch (state) {
    case 2: /* idle */
    case 4: /* charging */
    case 9: /* session_summary */
    case 10: /* post_brushing_summary */
        return true;
    default:
        return false;
    }
}
