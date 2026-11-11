#pragma once

#include <stdint.h>

// The three versions that describe a device: this firmware, the board it runs
// on and the firmware inside the SCD30.
//
// The firmware version comes from version.txt through the top-level
// CMakeLists.txt: "1.3.0" for a build of the commit tagged v1.3.0 with a clean
// tree, "1.3.0-dev+abc1234" (".dirty" appended with uncommitted changes) for
// anything else.

// As the build stamped it, up to 31 characters.
const char *fw_info_version(void);

// CONFIG_CO2_HW_REVISION, set in menuconfig.
const char *fw_info_hw_revision(void);

// Recorded once the sensor has answered. Until then, or if it never does,
// fw_info_sensor() returns an empty string. Setting it again is harmless.
void        fw_info_set_sensor(uint8_t major, uint8_t minor);
const char *fw_info_sensor(void);

// Why the chip last started, as a short lowercase word: "power_on",
// "software" for a restart the firmware asked for, "panic", "task_wdt",
// "brownout"... An unexpected restart is otherwise invisible once the
// device is back, since nothing else survives it.
const char *fw_info_reset_reason(void);
