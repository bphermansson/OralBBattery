/*
 * Oral-B iO battery monitor with an SSD1306 OLED.
 *
 * Poll the toothbrush over BLE and show its status on the OLED.
 *
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "oralb_ble.h"
#include "ssd1306.h"

static const char *TAG = "app";

static void show_message(struct ssd1306 *display, const char *message)
{
    char text[32];
    const int length = snprintf(text, sizeof(text), "%s", message);
    if (length < 0 || (size_t)length >= sizeof(text)) {
        ESP_LOGE(TAG, "could not format OLED message");
        return;
    }

    const int err = displayText(display, text);
    if (err != 0) {
        ESP_LOGE(TAG, "OLED update failed: %d", err);
    }
}

static void format_age(char *out, size_t size, int64_t age_us)
{
    const int64_t minutes = age_us / (60 * 1000 * 1000);
    if (minutes < 1) {
        snprintf(out, size, "<1m ago");
    } else if (minutes < 60) {
        snprintf(out, size, "%dm ago", (int)minutes);
    } else if (minutes < 24 * 60) {
        snprintf(out, size, "%dh%02dm", (int)(minutes / 60),
                 (int)(minutes % 60));
    } else {
        snprintf(out, size, "%dd ago", (int)(minutes / (24 * 60)));
    }
}

/* Four fixed 8-character rows, as wide as the display. */
static void show_battery(struct ssd1306 *display,
                         const oralb_ble_status_t *status,
                         const char *live_state, int64_t now_us,
                         char *shown, size_t shown_size)
{
    char age[16];
    format_age(age, sizeof(age), now_us - status->updated_us);

    char level[16];
    snprintf(level, sizeof(level), "%u%%", status->battery.percent);

    char text[40];
    snprintf(text, sizeof(text), "%-8.8s%-8.8s%-8.8s%-8.8s", "Battery", level,
             age, live_state);
    if (strcmp(text, shown) == 0) {
        return;
    }
    snprintf(shown, shown_size, "%s", text);

    const int err = displayText(display, text);
    if (err != 0) {
        ESP_LOGE(TAG, "OLED update failed: %d", err);
    }
}

static const char *advertisement_message(uint8_t state)
{
    switch (state) {
    case 2:
        return "Brush idle";
    case 3:
        return "Working";
    case 4:
        return "Charging";
    case 8:
        return "Brush menu";
    case 9:
    case 10:
        return "Session done";
    default:
        return "Brush detected";
    }
}

// static void log_board(void)
// {
//     esp_chip_info_t chip;
//     esp_chip_info(&chip);

//     uint32_t flash_size = 0;
//     if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
//         flash_size = 0;
//     }

//     ESP_LOGI(TAG, "Oral-B battery monitor, ESP-IDF %s", esp_get_idf_version());
//     ESP_LOGI(TAG, "target %s, %d core(s), silicon rev v%d.%d", CONFIG_IDF_TARGET,
//              chip.cores, chip.revision / 100, chip.revision % 100);
//     ESP_LOGI(TAG, "flash %" PRIu32 " MB", flash_size / (1024u * 1024u));

// }

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    //log_board();

    static struct ssd1306 display;
    initI2C(&display);
    ESP_ERROR_CHECK(displayText(&display, "Waiting for brush"));
    ESP_ERROR_CHECK(oralb_ble_init());

    ESP_LOGI(TAG, "OLED ready; waiting for a quiet Oral-B state");

    uint8_t displayed_adv_state = ORALB_STATE_UNKNOWN;
    bool displayed_waiting = true;
    char shown_battery[48] = "";
    for (;;) {
        const int64_t now_us = esp_timer_get_time();
        oralb_ble_adv_t adv;
        const bool has_fresh_adv =
            oralb_ble_get_adv(&adv) && now_us - adv.seen_us < 30 * 1000 * 1000;

        /* Once a battery level is known it stays on screen with its age. */
        oralb_ble_status_t status;
        if (oralb_ble_get_status(&status)) {
            show_battery(&display, &status,
                         has_fresh_adv ? advertisement_message(adv.adv.state)
                                       : "No signal",
                         now_us, shown_battery, sizeof(shown_battery));
        } else if (has_fresh_adv) {
            if (displayed_adv_state != adv.adv.state || displayed_waiting) {
                show_message(&display, advertisement_message(adv.adv.state));
            }
            displayed_adv_state = adv.adv.state;
            displayed_waiting = false;
        } else {
            if (!displayed_waiting) {
                show_message(&display, "Waiting for brush");
            }
            displayed_adv_state = ORALB_STATE_UNKNOWN;
            displayed_waiting = true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
