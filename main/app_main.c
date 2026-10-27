/*
 * Oral-B iO battery monitor with an SSD1306 OLED.
 *
 * Poll the toothbrush over BLE and show its status on the OLED.
 *
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <stdio.h>

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

static void show_battery(struct ssd1306 *display,
                         const oralb_ble_status_t *status)
{
    char text[64];
    const int length = snprintf(text, sizeof(text),
                                "Battery: %u%% %s",
                                status->battery.percent,
                                oralb_state_name(status->state));
    if (length < 0 || (size_t)length >= sizeof(text)) {
        ESP_LOGE(TAG, "could not format OLED status");
        return;
    }

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

    uint32_t displayed_generation = 0;
    uint8_t displayed_adv_state = ORALB_STATE_UNKNOWN;
    bool displayed_waiting = true;
    for (;;) {
        oralb_ble_adv_t adv;
        const bool has_fresh_adv =
            oralb_ble_get_adv(&adv) &&
            esp_timer_get_time() - adv.seen_us < 30 * 1000 * 1000;

        if (has_fresh_adv) {
            if (displayed_adv_state != adv.adv.state || displayed_waiting) {
                show_message(&display,
                             advertisement_message(adv.adv.state));
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

        oralb_ble_status_t status_snapshot;
        if (oralb_ble_get_status(&status_snapshot) &&
            status_snapshot.generation != displayed_generation &&
            has_fresh_adv && oralb_state_is_quiet(adv.adv.state)) {
            show_battery(&display, &status_snapshot);
            displayed_generation = status_snapshot.generation;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
