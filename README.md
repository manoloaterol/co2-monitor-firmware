# CO₂ Monitor firmware

Firmware for an indoor CO₂ monitor built on the **ESP32-S2**, with a
**Sensirion SCD30** sensor (CO₂, temperature and humidity) and a 240×280 colour
**NV3030B** display driven from a full frame buffer.

How to set up and use the monitor is in the **user documentation**:
**TODO: documentation site URL**. This README is for building and changing the
firmware.

## What it does

- **Three screens**, cycled with a 3-way joystick: large CO₂, CO₂ with its
  history chart, and CO₂ with temperature and humidity. A short press of the
  centre button dims the display.
- **Colour scale** anchored to indoor air quality thresholds (RITE, EN 16798-1,
  UBA). The reasoning is in [`docs/co2-color-scale.md`](docs/co2-color-scale.md).
- **MQTT** with any broker, plain or TLS (including client certificates), with
  optional **Home Assistant discovery**.
- **Its own web page** to configure everything, served by the device, with a
  captive portal for first setup.
- **Over-the-air updates** with automatic rollback.
- **Forced recalibration** (FRC) of the SCD30 against a reference.
- A **status LED** for the network link, which can be switched off.

## Layout

```
main/                       application: loop, screens, chart, joystick, LED
components/display/         display driver (NV3030B) and bitmap fonts
components/SCD30/           sensor driver (Sensirion, BSD-3)
components/app_config/      settings in NVS
components/co2_net/         Wi-Fi, MQTT, Home Assistant discovery, mDNS
components/web_config/      web server, captive portal and OTA updates
components/scd30_cfg/       sensor altitude and temperature compensation
components/fw_info/         firmware, hardware and sensor versions
tools/font_gen/             font generators, from TTF and from BDF
docs/                       design notes
partitions.csv              partition table (4 MB, with OTA)
version.txt                 firmware version (SemVer)
sdkconfig.defaults          chip configuration; sdkconfig is not versioned
```

## Building

Requires **ESP-IDF v6.1** or later in the 6.x series. The first build
downloads `mqtt`, `cjson` and `mdns` from the component registry, since they no
longer ship with IDF.

```sh
idf.py set-target esp32s2
idf.py build flash monitor
```

No network or broker is compiled in: a device with empty NVS starts in setup
mode. `idf.py flash` does not erase the NVS, so settings survive reflashing;
after an `erase-flash` the device has to be set up again from its page.

The console runs over **native USB CDC**. On a chip with no firmware yet, put
it in download mode by hand and flash with esptool's
`--before no-reset --after no-reset`, or the port disappears when esptool
resets the chip.

A `.devcontainer/` based on the official `espressif/idf` image is included, to
build without installing anything. `.vscode/` is not versioned: the ESP-IDF
extension regenerates it with each machine's paths and serial port.

## Versions

Each device reports three versions, on the boot screen, on its web page
(System) and in the MQTT `info` topic:

- **Firmware**, in [SemVer](https://semver.org). The source is
  [`version.txt`](version.txt). Only a build of the commit tagged
  `v<version>`, with no uncommitted changes, gets the bare version; anything
  else is marked as a development build with its commit:

  | Build | Version |
  |---|---|
  | commit tagged `v1.3.0`, clean tree | `1.3.0` |
  | any other commit | `1.3.0-dev+abc1234` |
  | with uncommitted changes | `1.3.0-dev+abc1234.dirty` |

- **Hardware**: the board revision, which cannot be read from the hardware. It
  is set at build time in `menuconfig` → *CO2 monitor* → `CO2_HW_REVISION`.
- **Sensor**: the SCD30's own firmware, read at boot.

To release a version:

```sh
git tag -a v1.0.0 -m "1.0.0"   # the number in version.txt
idf.py build                   # builds as 1.0.0
echo 1.0.1 > version.txt       # the next one, in a separate commit
```

The git state is read when CMake configures. A commit, a checkout or a
`git add` triggers a new configuration; an unstaged change does not, so the
`.dirty` mark can lag until the next one.

## MQTT contract

Everything hangs from a **base topic**, by default `co2monitor/<mac>` (the last
three bytes of the MAC), so several devices share a broker with no setup. It can
be replaced from the web page, for example with `home/livingroom/co2`.

| Topic | Payload | Retained | QoS |
|---|---|---|---|
| `<base>/co2` | CO₂ in ppm, integer: `812` | optional | 0 |
| `<base>/temperature` | °C with one decimal: `22.4` | optional | 0 |
| `<base>/humidity` | relative humidity in %, integer: `48` | optional | 0 |
| `<base>/state` | all three as JSON: `{"co2":812,"temperature":22.4,"humidity":48}` | optional | 0 |
| `<base>/status` | `online` / `offline`; also the last will | yes | 1 |
| `<base>/info` | diagnostics as JSON: `fw`, `hw`, `sensor_fw`, `ip`, `mac`, `hostname`, `rssi`, `tx_dbm`, `uptime_s`, `reset` (cause of the last restart), `chart_bar_s` | yes | 1 on connect, then 0 |

- Readings are published once per chart bar, as the average of the sensor
  readings in that time (by default every 20 s, the average of 10 readings).
  `info` is refreshed on connect and every 5 minutes.
- Before a deliberate restart the device publishes `offline` and disconnects
  cleanly. If the base topic changes, it clears its retained messages under the
  old one instead.
- With `mqtts://` the broker's certificate is **always verified**: against the
  public CAs bundled with the firmware, or against a CA pasted on the page.
- With Home Assistant discovery on, retained config messages are published under
  `homeassistant/sensor/…/config`, pointing to the same topics. Turning it off,
  or changing the prefix, removes them at the next restart.

The full description, with examples for each platform, is in the user
documentation.

## Web page and API

The page is a single file, [`www/index.html`](components/web_config/www/index.html),
embedded in the firmware, in English and Spanish. It talks to the device through
a small JSON API: `GET /api/status`, `GET`/`POST /api/config`, `GET /api/scan`
(Wi-Fi networks in range), `POST /api/frc`, `POST /api/reboot` and
`POST /api/ota`.

There is no password: anyone on the local network can use the page. What is
blocked is a website on the internet using it through a browser at home:

- Every request that changes something must carry the `X-CO2-Monitor` header.
  A foreign site cannot add it without a CORS preflight, which the device never
  grants. For your own scripts: `curl -H 'X-CO2-Monitor: 1' …`.
- Only requests addressed to an IP or to the device's own name are served,
  which stops DNS rebinding.
- Changing the broker's host clears the stored MQTT password unless a new one
  is typed, so nobody can redirect the device to their broker to capture it.
- OTA only accepts firmware images of this project.

## Updates and partitions

An update is uploaded from the page. Nothing boots until the image has arrived
complete and been validated. The new image boots **on trial**: unless it
confirms that it works (a real sensor reading and a radio that is not stuck
connecting), the bootloader restores the previous one at the next restart.

Settings are stored as a single, versioned NVS entry, written whole or not at
all, so a power cut in the middle of a save leaves the previous settings.

[`partitions.csv`](partitions.csv): 64 K NVS for the settings and **two 1,792 K
app partitions** for OTA. After changing the table, the first flash on a device
that had the old one needs a full erase (`idf.py erase-flash flash`).

## Wiring

| Signal | GPIO |
|---|---|
| I²C SDA / SCL (SCD30) | 11 / 12 |
| SPI MOSI / CLK (display) | 16 / 15 |
| Display DC / CS / RES / BL | 13 / 14 / 17 / 21 |
| Joystick left / right / press | 42 / 43 / 44 |

The joystick common goes to **+3V3** through a 1 kΩ resistor, so the three
inputs are read with the internal pull-down on a rising edge. Since the console
uses native USB, GPIO 43 and 44 (U0TXD and U0RXD on the ESP32-S2) are free for
the joystick.

The I²C bus runs at 50 kHz with the longest hardware timeout: the SCD30 allows
100 kHz at most and stretches the clock for up to 150 ms. In the console, `-1`
means the sensor does not answer, `-2` an I²C timeout, `-3` an intermittent
failure and `1` a CRC error.

## Fonts

The fonts are bitmap data generated from TTF, except the small 6×9 one, which
is converted from BDF. See [`tools/font_gen/README.md`](tools/font_gen/README.md).

## Licence

Copyright (C) 2026 Manuel Alfonso Terol (MAT).

This firmware is free software: you can redistribute it and/or modify it under
the terms of the **GNU General Public License v3.0**. The full text is in
[`LICENSE`](LICENSE). It comes with no warranty.

## Third-party licences

The notices that must accompany the binary are also shown on the device's web
page, under **System → Licences**.

- **SCD30 driver** (`components/SCD30/`): Sensirion AG, BSD 3-Clause.
- **ESP-IDF** and its `mqtt` and `mdns` components: Espressif, Apache 2.0.
  **cJSON**: MIT. They are downloaded at build time, not stored here.
- **Fonts**: Goldman and Russo One, SIL Open Font License 1.1; the licence
  texts are in `tools/font_gen/`. The 6×9 one is `6x9` from X11 misc-fixed,
  public domain.
- **Display** (`components/display/`): own code, written from the NV3030B
  datasheet (V0.6). The datasheet does not give the analogue and gamma values a
  specific panel needs; those values in the init sequence come from
  [Sang.IoT.NV3030B](https://github.com/sangyuxiaowu/NV3030B), Apache 2.0. The
  notice is next to the table in `display.c`, and the licence in
  `components/display/LICENSE-NV3030B-init.txt`. The datasheet is not included:
  it is manufacturer documentation with no permission to redistribute.
