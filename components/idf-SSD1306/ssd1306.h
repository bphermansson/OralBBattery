
#ifndef SSD1306_H_
#define SSD1306_H_

#include "stdint.h"
#include "driver/i2c_master.h"
#include "string.h"
#include "esp_log.h"
#include "font8x8.h"
#include "stddef.h"

#define WIDTH 128
#define HEIGHT 64
#define NUM_PAGES ((WIDTH * HEIGHT) / (BYTES_PER_PAGE * 8))
#define BYTES_PER_PAGE 128
#define SSD1306ADDRESS 0x3c
#define BITS_PER_BYTE 8
#define SSD1306SDA 8
#define SSD1306SCL 9
#define SSD1306CLKSPEED 400000
#define I2CCOMMANDWAITMS 100
#define TOTALDISPLAYMEMORY NUM_PAGES * BYTES_PER_PAGE


struct ssd1306
{
    uint8_t address;
    uint8_t framebuffer[NUM_PAGES][BYTES_PER_PAGE];
    i2c_master_dev_handle_t i2c_device;
};


void initI2C(struct ssd1306 * device);

void writeDisplay(struct ssd1306 * device);

void clearDisplay(struct ssd1306 * device);

int writeBitmap(struct ssd1306 * device, uint8_t * bitmap, size_t bitmapSize);

int displayText(struct ssd1306 * device, char * text);

#endif // SSD1306_H_