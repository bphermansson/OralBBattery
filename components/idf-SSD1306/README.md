# idf-ssd1306-driver

An IDF component for driving the SSD1306 and similar OLED displays.

## Usage

Modify the HEIGHT and WIDTH constants to suit the display you are using. The default is 128x64.
Change the I2C SCL and SDA lines in the header file 

Clone this repository into a ./components folder in the root directory of your idf project.

Include the header like
```
  #include "ssd1306.h"
```

