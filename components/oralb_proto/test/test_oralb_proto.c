/*
 * Host tests for the Oral-B protocol decoders. No ESP-IDF required:
 *
 *     cd components/oralb_proto/test && make
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "oralb_proto.h"

static int failures;
static int checks;

#define CHECK(cond)                                                            \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                      \
    } while (0)

#define CHECK_STR(actual, expected)                                            \
    do {                                                                       \
        const char *a_ = (actual);                                             \
        const char *e_ = (expected);                                           \
        checks++;                                                              \
        if (a_ == NULL || strcmp(a_, e_) != 0) {                               \
            failures++;                                                        \
            fprintf(stderr, "FAIL %s:%d: got \"%s\", want \"%s\"\n", __FILE__, \
                    __LINE__, a_ ? a_ : "(null)", e_);                         \
        }                                                                      \
    } while (0)

static oralb_battery_t battery(const uint8_t *data, size_t len, int protocol)
{
    oralb_battery_t b;
    oralb_parse_battery(data, len, protocol, &b);
    return b;
}

static void test_battery_percent_only(void)
{
    const uint8_t ff05[] = {87};
    const oralb_battery_t b = battery(ff05, sizeof(ff05), -1);

    CHECK(b.has_percent);
    CHECK(b.percent == 87);
    CHECK(!b.has_runtime);
    CHECK(!b.has_voltage);
    CHECK(!b.has_current);
    CHECK(!b.has_temperature);
}

static void test_battery_empty_and_null(void)
{
    const oralb_battery_t empty = battery(NULL, 0, -1);
    const uint8_t one[] = {0};
    const oralb_battery_t zero = battery(one, sizeof(one), -1);

    CHECK(!empty.has_percent);
    CHECK(!empty.has_runtime);

    /* A zero first byte is a valid 0% reading, not "missing". */
    CHECK(zero.has_percent);
    CHECK(zero.percent == 0);
}

static void test_battery_percent_out_of_range(void)
{
    const uint8_t high[] = {101, 0x10, 0x0E};
    const uint8_t ff[] = {0xFF};
    const oralb_battery_t a = battery(high, sizeof(high), -1);
    const oralb_battery_t b = battery(ff, sizeof(ff), -1);

    CHECK(!a.has_percent);
    CHECK(a.has_runtime); /* later fields still parse */
    CHECK(!b.has_percent);
}

static void test_battery_runtime(void)
{
    const uint8_t running[] = {42, 0x10, 0x0E}; /* 3600 s */
    const uint8_t sentinel[] = {42, 0xFF, 0xFF};
    const uint8_t p6_zero[] = {42, 0x00, 0x00};
    const uint8_t p8_zero[] = {42, 0x00, 0x00};

    const oralb_battery_t a = battery(running, sizeof(running), -1);
    const oralb_battery_t b = battery(sentinel, sizeof(sentinel), -1);
    const oralb_battery_t c = battery(p6_zero, sizeof(p6_zero), 6);
    const oralb_battery_t d = battery(p8_zero, sizeof(p8_zero), 8);

    CHECK(a.has_runtime);
    CHECK(a.runtime_s == 3600);

    CHECK(!b.has_runtime);
    CHECK(!c.has_runtime);
    CHECK(d.has_runtime);
    CHECK(d.runtime_s == 0);
}

static void test_battery_full_payload(void)
{
    /* 87 %, 3600 s, 3296 mV, current unavailable, 23 C */
    const uint8_t ff05[] = {87, 0x10, 0x0E, 0xE0, 0x0C, 0xFF, 0xFF, 23};
    const oralb_battery_t b = battery(ff05, sizeof(ff05), 8);

    CHECK(b.has_percent && b.percent == 87);
    CHECK(b.has_runtime && b.runtime_s == 3600);
    CHECK(b.has_voltage && b.voltage_mv == 3296);
    CHECK(!b.has_current);
    CHECK(b.has_temperature && b.temperature_c == 23);
}

static void test_battery_current_signed(void)
{
    /* -50 mA is 0xFFCE little endian. */
    const uint8_t discharge[] = {50, 0, 0, 0xE0, 0x0C, 0xCE, 0xFF, 20};
    /* +750 mA is 0x02EE. */
    const uint8_t charge[] = {50, 0, 0, 0xE0, 0x0C, 0xEE, 0x02, 20};

    const oralb_battery_t a = battery(discharge, sizeof(discharge), 8);
    const oralb_battery_t b = battery(charge, sizeof(charge), 8);

    CHECK(a.has_current && a.current_ma == -50);
    CHECK(b.has_current && b.current_ma == 750);
}

static void test_battery_voltage_range(void)
{
    const uint8_t zero[] = {50, 0, 0, 0x00, 0x00};
    const uint8_t too_high[] = {50, 0, 0, 0x10, 0x27}; /* 10000 mV */
    const uint8_t max_valid[] = {50, 0, 0, 0x0F, 0x27}; /* 9999 mV */
    const uint8_t len4[] = {50, 0, 0, 0xE0};

    CHECK(!battery(zero, sizeof(zero), 8).has_voltage);
    CHECK(!battery(too_high, sizeof(too_high), 8).has_voltage);
    CHECK(battery(max_valid, sizeof(max_valid), 8).has_voltage);
    CHECK(!battery(len4, sizeof(len4), 8).has_voltage);
}

static void test_battery_temperature_range(void)
{
    const uint8_t cold[] = {50, 0, 0, 0, 0, 0, 0, (uint8_t)-40};
    const uint8_t too_cold[] = {50, 0, 0, 0, 0, 0, 0, (uint8_t)-41};
    const uint8_t hot[] = {50, 0, 0, 0, 0, 0, 0, 125};
    const uint8_t too_hot[] = {50, 0, 0, 0, 0, 0, 0, 126};
    const uint8_t negative[] = {50, 0, 0, 0, 0, 0, 0, (uint8_t)-5};

    CHECK(battery(cold, sizeof(cold), 8).has_temperature);
    CHECK(battery(cold, sizeof(cold), 8).temperature_c == -40);
    CHECK(!battery(too_cold, sizeof(too_cold), 8).has_temperature);
    CHECK(battery(hot, sizeof(hot), 8).has_temperature);
    CHECK(battery(hot, sizeof(hot), 8).temperature_c == 125);
    CHECK(!battery(too_hot, sizeof(too_hot), 8).has_temperature);
    CHECK(battery(negative, sizeof(negative), 8).temperature_c == -5);
}

static void test_battery_truncated_payloads(void)
{
    const uint8_t long_payload[] = {50, 0x10, 0x0E, 0xE0, 0x0C,
                                    0xEE, 0x02, 20};
    size_t len;

    for (len = 0; len <= sizeof(long_payload); len++) {
        const oralb_battery_t b = battery(long_payload, len, 8);

        CHECK(b.has_percent == (len >= 1));
        CHECK(b.has_runtime == (len >= 3));
        CHECK(b.has_voltage == (len >= 5));
        CHECK(b.has_current == (len >= 7));
        CHECK(b.has_temperature == (len >= 8));
    }
}

static void test_device_info(void)
{
    const uint8_t ff02[] = {0x36, 0x08, 0x52};
    const uint8_t short_payload[] = {0x36, 0x08};
    oralb_device_info_t info;

    CHECK(oralb_parse_device_info(ff02, sizeof(ff02), &info));
    CHECK(info.model_id == 0x36);
    CHECK(info.protocol_version == 8);
    CHECK(info.firmware_revision == 0x52);

    CHECK(!oralb_parse_device_info(short_payload, sizeof(short_payload), &info));
    CHECK(info.model_id == 0);
    CHECK(!oralb_parse_device_info(NULL, 0, &info));
    CHECK(!oralb_parse_device_info(ff02, sizeof(ff02), NULL));
}

static void test_adv(void)
{
    /* protocol 8, model 0x36, firmware 0x52, charging, no pressure */
    const uint8_t mfg[] = {0x08, 0x36, 0x52, 0x04, 0x00,
                           0x01, 0x1E, 0x00, 0x00, 0x00, 0x04};
    oralb_adv_t adv;

    CHECK(sizeof(mfg) == ORALB_ADV_LEN);
    CHECK(oralb_parse_adv(mfg, sizeof(mfg), &adv));
    CHECK(adv.protocol == 8);
    CHECK(adv.model == 0x36);
    CHECK(adv.firmware == 0x52);
    CHECK(adv.state == 4);
    CHECK(adv.pressure_flags == 0);
    CHECK(adv.time_minutes == 1);
    CHECK(adv.time_seconds == 30);
    CHECK(adv.mode == 0);
    CHECK(adv.sector == 0);
    CHECK(adv.sector_timer == 0);
    CHECK(adv.total_sectors == 4);

    /* iO Sense charger payload is 14 bytes: must be rejected. */
    {
        const uint8_t sense[14] = {0xDC, 0x00, 0xA2, 0, 0, 0, 0, 0,
                                   0,    0,    0,    0, 0, 0};
        CHECK(!oralb_parse_adv(sense, sizeof(sense), &adv));
    }
    /* Truncated. */
    CHECK(!oralb_parse_adv(mfg, sizeof(mfg) - 1, &adv));
    CHECK(!oralb_parse_adv(NULL, ORALB_ADV_LEN, &adv));
}

static void test_states(void)
{
    CHECK_STR(oralb_state_name(0), "unknown");
    CHECK_STR(oralb_state_name(1), "initializing");
    CHECK_STR(oralb_state_name(2), "idle");
    CHECK_STR(oralb_state_name(3), "running");
    CHECK_STR(oralb_state_name(4), "charging");
    CHECK_STR(oralb_state_name(8), "selection_menu");
    CHECK_STR(oralb_state_name(9), "session_summary");
    CHECK_STR(oralb_state_name(10), "post_brushing_summary");
    CHECK_STR(oralb_state_name(115), "sleeping");
    CHECK_STR(oralb_state_name(116), "transport");
    CHECK_STR(oralb_state_name(200), "unknown");
    CHECK_STR(oralb_state_name(ORALB_STATE_UNKNOWN), "unknown");

    CHECK(oralb_state_is_quiet(2));
    CHECK(oralb_state_is_quiet(4));
    CHECK(oralb_state_is_quiet(9));
    CHECK(oralb_state_is_quiet(10));
    CHECK(!oralb_state_is_quiet(1));
    CHECK(!oralb_state_is_quiet(3));
    CHECK(!oralb_state_is_quiet(5));
    CHECK(!oralb_state_is_quiet(8));
    CHECK(!oralb_state_is_quiet(115));

    CHECK(oralb_state_is_awake(1));
    CHECK(oralb_state_is_awake(2));
    CHECK(oralb_state_is_awake(3));
    CHECK(oralb_state_is_awake(4));
    CHECK(oralb_state_is_awake(5));
    CHECK(oralb_state_is_awake(8));
    CHECK(oralb_state_is_awake(9));
    CHECK(oralb_state_is_awake(10));
    CHECK(!oralb_state_is_awake(0));
    CHECK(!oralb_state_is_awake(6));
    CHECK(!oralb_state_is_awake(7));
    CHECK(!oralb_state_is_awake(115));
    CHECK(!oralb_state_is_awake(116));
}

static void test_models(void)
{
    CHECK_STR(oralb_model_name(0), "Triumph D36");
    CHECK_STR(oralb_model_name(32), "Genius Series D701");
    CHECK_STR(oralb_model_name(40), "Smart/Pro Series D700");
    CHECK_STR(oralb_model_name(0x36), "iO Series"); /* 54, the tested brush */
    CHECK_STR(oralb_model_name(52), "iO Series 4");
    CHECK_STR(oralb_model_name(53), "iO Series 5");
    CHECK_STR(oralb_model_name(64), "Smart Series D21");
    CHECK_STR(oralb_model_name(85), "Pro Series D601");
    CHECK_STR(oralb_model_name(112), "Genius X D706");
    CHECK_STR(oralb_model_name(200), "unknown");
}

int main(void)
{
    test_battery_percent_only();
    test_battery_empty_and_null();
    test_battery_percent_out_of_range();
    test_battery_runtime();
    test_battery_full_payload();
    test_battery_current_signed();
    test_battery_voltage_range();
    test_battery_temperature_range();
    test_battery_truncated_payloads();
    test_device_info();
    test_adv();
    test_states();
    test_models();

    if (failures != 0) {
        fprintf(stderr, "%d/%d checks failed\n", failures, checks);
        return 1;
    }
    printf("ok: %d checks passed\n", checks);
    return 0;
}
