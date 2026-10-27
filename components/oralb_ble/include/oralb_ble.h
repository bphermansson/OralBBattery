/*
 * Oral-B iO BLE central: passive advertisement watching.
 *
 * The brush takes exactly one BLE client and stops advertising while that
 * slot is taken. This client connects briefly in quiet states to read battery
 * data; nothing is written to the brush and no bonding is used.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "oralb_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    oralb_adv_t adv;
    uint8_t addr[6]; /* display order, most significant first */
    int8_t rssi;
    int64_t seen_us; /* esp_timer timestamp */
} oralb_ble_adv_t;

typedef struct {
    bool valid;
    oralb_device_info_t device;
    oralb_battery_t battery;
    uint8_t state;
    int64_t updated_us;
    uint32_t generation;
} oralb_ble_status_t;

/* Start NimBLE, scan continuously, and poll FF02/FF04/FF05 in quiet states. */
esp_err_t oralb_ble_init(void);

/* Copy the most recent Oral-B advertisement. False until one is seen. */
bool oralb_ble_get_adv(oralb_ble_adv_t *out);

/* Copy the most recent successful battery poll. False until one completes. */
bool oralb_ble_get_status(oralb_ble_status_t *out);

/* True once the host has synced and scanning has started. */
bool oralb_ble_is_ready(void);

/* Format a BLE address as "aa:bb:cc:dd:ee:ff". */
void oralb_ble_addr_str(const uint8_t addr[6], char out[18]);

#ifdef __cplusplus
}
#endif
