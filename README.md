# Cardputer DS18x20 Sensor Manager

PlatformIO / Arduino project for the **M5Stack Card Computer (Cardputer v1.1 with StampS3A)**.


## Features

- Detects DS18S20 / DS18B20 family 1-Wire sensors on **Port A**.
- Shows each sensor address, current temperature, and optional user name.
- Saves JSON to `/sensor_names.json` on the SD card whenever a sensor name is stored.
- Loads address-to-name mappings on boot.
- Keeps known sensors in the list even when unplugged and shows temperature as `NA`.
- Detects newly attached and removed sensors automatically while running.

## Controls

### Browse mode

- `;` = move selection up
- `.` = move selection down
- `Enter` = edit selected sensor name
- `fn` = toggle all sensors / active sensors

### Edit mode

- `,` = move cursor left
- `/` = move cursor right
- `Del` = backspace
- `Enter` = save edited name

## SD card JSON format

The file is saved in the SD card root directory as `/sensor_names.json`.

Example:

```json
[
  {
    "address": "28FF641F9316054C",
    "name": "Tank top"
  },
  {
    "address": "1011223344556677",
    "name": "Ambient"
  }
]
```

## Wiring note

This project uses **GPIO2** (yellow) as the 1-Wire data line for Port A.


## Build

1. Open the folder in VS Code with PlatformIO.
2. Build the project.
3. Upload to the device.

