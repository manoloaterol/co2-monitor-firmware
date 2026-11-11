#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Setup web interface.
//
// The same page is served in both modes:
//
//   provisioning  the device runs its own access point and a captive DNS, so a
//                 phone that joins it opens the page by itself
//   normal        the device is on the home network and the page is reachable
//                 at its address, to change settings without losing the link
//
// Settings are read and written through app_config, so this module owns no
// state of its own.
esp_err_t web_config_start(bool captive);

esp_err_t web_config_stop(void);

// The latest reading, for the live status on the page. Called from the display
// loop on every measurement; the web server only ever reads a copy.
void web_config_set_reading(uint16_t ppm, float temperature, float humidity);

// Whether readings are arriving, as the display loop judges it. The page shows
// the same warning as the screen.
void web_config_set_sensor_ok(bool ok);

// Called after a save with the brightness levels, in percent, the screen
// language (app_lang_t) and whether the status LED is in use. They are the
// settings applied without a reboot, and the display and the LED belong to the
// application, not to this component.
void web_config_on_display(void (*fn)(uint8_t full, uint8_t dim, uint8_t lang, bool led));

// Confirms the running image so the bootloader stops treating it as on
// probation. Called once the device has proved it works, which here means a
// real sensor reading and a radio that is not stuck trying to join a network:
// the failure OTA must protect against is an update that cannot get back on
// the network, because that is the one needing a cable to undo.
void ota_mark_healthy(void);
