# Oral-B iO battery monitor

Firmware for an Oral-B iO battery monitor, using BLE and a 128×64 SSD1306 OLED
for status. No Wi-Fi, no MQTT, no cloud, no phone.

The device is always powered and keeps an eye on the brush's single BLE slot.
It only connects briefly in quiet states and disconnects without writing to
the brush.

The OLED shows the last battery percentage and brush state; before the first
successful poll it displays `Waiting for brush`.

## Hardware

| Part | Note |
|---|---|
| ESP32-C3 board | 4 MB flash configured; USB Serial/JTAG availability depends on the board |
| SSD1306 OLED | 128×64 pixels, I2C, 7-bit address `0x3c` |
| USB supply | Always on, ~40 mA average |

### Wiring

Connect the OLED to the ESP32-C3 board:

| OLED pin | ESP32-C3 | Role |
|---|---|---|
| SCL | GPIO9 | I2C clock |
| SDA | GPIO8 | I2C data |
| GND | GND | Ground |
| VCC | 3V3 | **3.3 V** |

The I2C pins, address and display dimensions are defined in
`components/idf-SSD1306/ssd1306.h`.

## Build

ESP-IDF 5.4, activated by `env.sh`:

```sh
source ./env.sh

idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

On boards with built-in USB Serial/JTAG the console uses
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`; otherwise configure the board's UART
console and serial port.

## How it works

### Knowing when it is safe to connect

The brush accepts exactly **one** BLE client and stops advertising while that
slot is taken. The phone app competes for the same slot, so the monitor watches
advertisements and only makes a short connection when the brush is in a quiet
state:

| Advertisement state | Action |
|---|---|
| 2 `idle` | connect |
| 4 `charging` | connect |
| 9 `session_summary` | connect |
| 10 `post_brushing_summary` | connect |
| 3 `running`, 8 `selection_menu` | **never** — brushing in progress |
| 115 `sleeping`, 116 `transport` | skip |

The state comes from byte 3 of the manufacturer data (company id `0x00DC`,
11-byte payload). The iO Sense charger advertises 14 bytes under the same
company id, so length is the filter.

### Polling

- every **10 minutes** while charging,
- every **60 minutes** while idle off the charger,
- never during a session.

A connection reads FF02 (model/protocol/firmware), FF04 (state) and FF05
(battery), then disconnects. FF04 is checked again before reading the battery
so a state transition into brushing aborts the poll. All reads are read-only;
the monitor never writes to the brush.

### Display

The SSD1306 is initialized over I2C at startup. After each successful BLE poll,
the OLED updates with the battery percentage and brush state. The display
driver is in `components/idf-SSD1306/`, copied from the separate SSD1306
ESP-IDF project and retained under its MIT license.

## Protocol

`FF05` is the only place the battery level exists; it is not in the
advertisement. Little-endian, every field length-gated so older firmware does
not read past the end:

| Offset | Type | Meaning |
|---|---|---|
| 0 | u8 | percent, 0–100 (`>100` = invalid) |
| 1–2 | u16 | remaining runtime in seconds (`0xFFFF` = unknown) |
| 3–5 | u16 | battery voltage in mV (0 or ≥10000 = invalid) |
| 5–6 | i16 | current in mA (`-1` = unknown) |
| 7 | i8 | battery temperature in °C (`-40..125` valid) |

`FF02` is `[model_id, protocol_version, firmware_revision]`; the tested brush
reports `36 08 52` → model 54 ("iO Series"), protocol 8, firmware 82.

See `components/oralb_proto/` for the decoders and their tests.

## Tests

The protocol code is plain C with no ESP-IDF dependency:

```sh
cd components/oralb_proto/test
make
```

## Licence

MIT. The protocol decoders are ported from
[oralb-ha](https://github.com/thomasgregg/oralb-ha) v0.7.40
(`custom_components/oralb_live/protocol.py`, `const.py`), which is MIT
licensed. The SSD1306 driver retains its separate MIT license in
`components/idf-SSD1306/LICENSE`.

Thanks to https://github.com/thomasgregg/oralb-ha for protocol info.

Coded by me, OpenCode and Copilot.