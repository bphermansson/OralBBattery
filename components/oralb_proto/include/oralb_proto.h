/*
 * oralb_proto - pure C decoders for the Oral-B iO BLE protocol.
 *
 * Kept free of ESP-IDF and NimBLE so every byte layout can be regression
 * tested on a host with nothing but a C compiler.
 *
 * SPDX-License-Identifier: MIT
 *
 * The FF05/FF02/FF29 layouts and the state table are ported from oralb-ha
 * (https://github.com/thomasgregg/oralb-ha), custom_components/oralb_live/
 * protocol.py and const.py at tag v0.7.40, which is MIT licensed.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Manufacturer specific data: company id 0x00DC (Procter & Gamble). */
#define ORALB_MFG_ID 0x00DCu

/*
 * Length of the iO advertisement payload *after* the 2-byte company id.
 * The iO Sense charger advertises 14 bytes under the same company id; a
 * length check is the cheapest way to tell the two apart.
 */
#define ORALB_ADV_LEN 11u

/* State byte 0xFF and anything unmapped is reported as "unknown". */
#define ORALB_STATE_UNKNOWN 0xFFu

/* FF05 battery status. */
typedef struct {
    bool has_percent;
    uint8_t percent; /* 0..100 */

    bool has_runtime;
    uint16_t runtime_s; /* remaining brushing time */

    bool has_voltage;
    uint16_t voltage_mv;

    bool has_current;
    int16_t current_ma;

    bool has_temperature;
    int8_t temperature_c;
} oralb_battery_t;

/* FF02 model / protocol / firmware. */
typedef struct {
    uint8_t model_id;
    uint8_t protocol_version;
    uint8_t firmware_revision;
} oralb_device_info_t;

/*
 * Advertisement payload (11 bytes, after the company id).
 *
 * [0] protocol        [1] model           [2] firmware
 * [3] state           [4] pressure/flags  [5..6] brush time mm:ss
 * [7] mode            [8] sector/face     [9] sector timer
 * [10] total sectors
 *
 * The battery level is NOT advertised: only FF05 has it, so a connection is
 * mandatory. This struct exists to decide *when* a connection is safe.
 */
typedef struct {
    uint8_t protocol;
    uint8_t model;
    uint8_t firmware;
    uint8_t state;
    uint8_t pressure_flags;
    uint8_t time_minutes;
    uint8_t time_seconds;
    uint8_t mode;
    uint8_t sector;
    uint8_t sector_timer;
    uint8_t total_sectors;
} oralb_adv_t;

/*
 * Decode FF05. `protocol_version` is the FF02 byte 1 value, or -1 when it is
 * not known yet (protocol 6 treats a zero runtime as "unavailable").
 * Always zeroes *out first, so a short notification cannot leave stale fields.
 */
void oralb_parse_battery(const uint8_t *data, size_t len,
                         int protocol_version, oralb_battery_t *out);

/* Decode FF02. Returns false (and zeroes *out) if fewer than 3 bytes. */
bool oralb_parse_device_info(const uint8_t *data, size_t len,
                             oralb_device_info_t *out);

/*
 * Decode the 11-byte advertisement payload. `mfg` must point past the 2-byte
 * company id. Returns false unless len == ORALB_ADV_LEN.
 */
bool oralb_parse_adv(const uint8_t *mfg, size_t len, oralb_adv_t *out);

/* Human readable state; never NULL. */
const char *oralb_state_name(uint8_t state);

/* Marketing name for an FF02/advertisement model id; never NULL. */
const char *oralb_model_name(uint8_t model_id);

/*
 * States in which the brush will hold a connection at all
 * (AWAKE_STATES: 1, 2, 3, 4, 5, 8, 9, 10).
 */
bool oralb_state_is_awake(uint8_t state);

/*
 * States quiet enough to connect to without disturbing a brushing session.
 * This is the gate the poller uses: idle, charging, and the two summary
 * states. Running, menus, sleeping and transport are never synced.
 */
bool oralb_state_is_quiet(uint8_t state);

#ifdef __cplusplus
}
#endif
