
#include "ssd1306.h"

static const char * TAG = "SSD1306";


void initI2C(struct ssd1306 * device)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .scl_io_num = SSD1306SCL,
        .sda_io_num = SSD1306SDA,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));

    device->address = SSD1306ADDRESS;
    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = device->address,
        .scl_speed_hz = SSD1306CLKSPEED,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &device_config, &device->i2c_device));

    const uint8_t init_commands[] = {
        0x00,
        0xae, 0xa8, (uint8_t)(HEIGHT - 1),
        0xd3, 0x00, 0x40, 0xa0, 0xc8,
        0xd5, 0x80, 0xda, (HEIGHT == 32) ? 0x02 : 0x12,
        0x81, 0xff, 0xa4, 0xdb, 0x40,
        0x20, 0x02, 0x00, 0x10, 0x8d, 0x14,
        0x2e, 0xa6, 0xd5, 0x80, 0xaf,
    };
    ESP_ERROR_CHECK(i2c_master_transmit(device->i2c_device, init_commands,
                                         sizeof(init_commands), I2CCOMMANDWAITMS));
    ESP_LOGI(TAG, "Successfully initialized SSD1306.");
}


void writeDisplay(struct ssd1306 * device)
{
    for (int i = 0; i < NUM_PAGES; i++)
    {
        const uint8_t page_commands[] = {0x00, 0x00, 0x10, (uint8_t)(0xb0 | i)};
        uint8_t page_data[BYTES_PER_PAGE + 1] = {0x40};
        memcpy(&page_data[1], device->framebuffer[i], BYTES_PER_PAGE);

        ESP_ERROR_CHECK(i2c_master_transmit(device->i2c_device, page_commands,
                                             sizeof(page_commands), I2CCOMMANDWAITMS));
        ESP_ERROR_CHECK(i2c_master_transmit(device->i2c_device, page_data,
                                             sizeof(page_data), I2CCOMMANDWAITMS));
    }
}


void clearDisplay(struct ssd1306 * device)
{
    memset(device->framebuffer, 0x00, TOTALDISPLAYMEMORY);

    writeDisplay(device);
}


int writeBitmap(struct ssd1306 * device, uint8_t * bitmap, size_t bitmapSize)
{
    if (bitmapSize != TOTALDISPLAYMEMORY)
    {
        ESP_LOGE(TAG, "Bitmap is wrong size for display.");
        return -1;
    }

    // The display expects bitmap data to come in a different format. This consists in something similar to a transpose operation.
    // For example, a bitmap image looking like:

    // Byte 0: 11111111
    // Byte 1: 00000000
    // Byte 2: 11111111
    // Byte 3: 00000000
    // Byte 4: 11111111
    // Byte 5: 00000000
    // Byte 6: 11111111
    // Byte 7: 00000000
    // ...

    // Is transformed to look like:

    // Byte 0: 10101010
    // Byte 1: 10101010
    // Byte 2: 10101010
    // Byte 3: 10101010
    // Byte 4: 10101010
    // Byte 5: 10101010
    // Byte 6: 10101010
    // Byte 7: 10101010
    // ...

    // The display has a memory layout like this:

    //                 SEG0  SEG1  ...  SEG127
    // Page 0:  Bit 0   x     x            x
    //          Bit 1   x     x            x
    //          Bit 2   x     x            x
    //          Bit 3   x     x            x
    //          Bit 4   x     x            x
    //          Bit 5   x     x            x
    //          Bit 6   x     x            x
    //          Bit 7   x     x            x

    // Which is akin to how the bits are actually shown on the display.

    clearDisplay(device);

    // Populate the bitmap bit-by-bit since we are mangling every byte into a new format.
    for (int page = 0; page < NUM_PAGES; page++)
    {
        // Loop through every bit of this page.
        for (int i = 0; i < (BYTES_PER_PAGE * BITS_PER_BYTE); i++)
        {
            int arr2width = BYTES_PER_PAGE;
            int arr2row = i / arr2width;
            int arr2col = arr2width - 1 - (i % arr2width);
            int arr1idx = (i / BITS_PER_BYTE) + (page * BYTES_PER_PAGE);
            int arr1bit = 7 - (i % BITS_PER_BYTE);

            // Perform bitmask of the current bit, populate the framebuffer with it in the correct offset.
            device->framebuffer[page][arr2col] |= (((bitmap[arr1idx] & (0x1 << arr1bit)) >> arr1bit) << (arr2row));
        }
    }

    writeDisplay(device);

    return 0;
}


int displayText(struct ssd1306 * device, char * text)
{
    if (device == NULL || text == NULL)
    {
        ESP_LOGE(TAG, "Display and text must not be null.");
        return -1;
    }

    const int fontSize = 14;
    const int cellWidth = fontSize + 1;
    const int rowHeight = fontSize + 1;
    const int bytesPerRow = (WIDTH + 7) / 8;

    const int charsPerRow = WIDTH / cellWidth;
    const int numRows = HEIGHT / rowHeight;
    size_t stringLength = strlen(text);
    const size_t maxStringLength = (size_t)charsPerRow * numRows;

    if (stringLength > maxStringLength)
    {
        stringLength = maxStringLength;
        ESP_LOGW(TAG, "Text truncated to fit the display.");
    }

    uint8_t bitmap[TOTALDISPLAYMEMORY];

    memset(bitmap, 0x00, TOTALDISPLAYMEMORY);

    for (size_t character = 0; character < stringLength; character++)
    {
        const int textWrapRow = character / charsPerRow;
        const int textColumn = character % charsPerRow;
        const uint8_t characterCode = (uint8_t)text[character];
        const uint8_t glyphIndex = characterCode < 128 ? characterCode : '?';

        for (int y = 0; y < fontSize; y++)
        {
            const int sourceY = y * 8 / fontSize;
            const int screenY = textWrapRow * rowHeight + y;

            for (int x = 0; x < fontSize; x++)
            {
                const int sourceX = x * 8 / fontSize;
                const uint8_t glyphRow =
                    (uint8_t)font8x8_basic[glyphIndex][sourceY];
                if ((glyphRow & (1u << sourceX)) != 0)
                {
                    const int screenX = textColumn * cellWidth + x;
                    const size_t bitmapIndex =
                        (size_t)screenY * bytesPerRow + screenX / 8;
                    bitmap[bitmapIndex] |= (uint8_t)(1u << (7 - screenX % 8));
                }
            }
        }
    }

    writeBitmap(device, bitmap, TOTALDISPLAYMEMORY);

    return 0;

}