#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Altitude and temperature compensations of the SCD30, and its CO2
// calibration.
//
// The compensations live in the sensor's own non-volatile memory, not in ours,
// so what it holds and what we asked for can drift apart. Everything here
// exists to make that difference visible instead of having to trust the write
// went through.
void scd30_cfg_apply(uint16_t altitude_m, uint16_t temp_offset_c100);

// What the sensor reported after the last apply, which is the only honest
// answer to "did it get stored". Cached at boot on purpose: reading it live
// would mean driving the I2C bus from whichever task asks, while the display
// loop is polling the sensor on the same bus.
void scd30_cfg_applied(uint16_t *altitude_m, uint16_t *temp_offset_c100);

// Whether the sensor's automatic self-calibration was on at boot. This
// firmware never changes it; it is reported because ASC and a forced
// recalibration overwrite each other.
bool scd30_cfg_asc_active(void);

// Forced recalibration: tells the sensor that the air around it holds ref_ppm
// of CO2 (400 to 2000), and it shifts its calibration to match. Persistent in
// the sensor. Callable from any task: the work itself is done by whichever
// task calls scd30_cfg_service(), the one that owns the bus, and this waits
// for it for up to a few seconds.
//
// ESP_OK once the sensor accepted it, ESP_FAIL if it refused,
// ESP_ERR_INVALID_STATE if another one is under way, and ESP_ERR_TIMEOUT if
// no answer came in time, in which case it may or may not have been applied.
esp_err_t scd30_cfg_frc(uint16_t ref_ppm);

// Runs a pending recalibration, if there is one. Call on every pass of the
// task that polls the sensor, between two transactions of its own.
void scd30_cfg_service(void);
