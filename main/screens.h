#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sensor_data.h"

// While provisioning, an extra screen is prepended telling the user which
// network to join. Passing NULL leaves setup mode. The pointer is kept, not
// copied, so it must outlive the call.
void screens_set_setup(const char *ap_ssid);

// Boot screen: logo, name and the three versions. Drawn once at power-up with
// the sensor still unknown (NULL), and again once it has answered or failed
// to: "" means it did not answer.
void screens_logo(const char *sensor_fw);

// Paints, right away, the notice shown just before a restart that enters or
// leaves setup mode.
void screens_show_restart(bool entering_setup);

// Language of the text on the screen, an app_lang_t. English until set.
void screens_set_language(uint8_t lang);

// While set, the screens show dashes instead of readings plus a warning box
// over the chart, which keeps its history.
void screens_set_sensor_fault(bool fault);

// Screens the joystick cycles through, in order.
uint8_t     screens_count(void);
const char *screens_name(uint8_t idx);
void        screens_draw(uint8_t idx, const sensor_data_t *d);

// Sizes the history chart: how many bars it shows and how many readings each
// one averages. Call once, before the first reading; bars must be one of
// APP_CHART_BARS.
void screens_chart_setup(uint16_t bars, uint16_t avg_n);

// Feeds the history chart, which is unique and shared by the screens. Returns
// true when the reading closed a bar.
bool screens_add_reading(uint16_t ppm);
