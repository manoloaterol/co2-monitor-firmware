#pragma once

#include <stdint.h>

// RGB565 color for a CO2 value, interpolated over a ramp anchored to the
// indoor air quality thresholds. See docs/escala-color-co2.md for where each
// control point comes from.
uint16_t co2_color(uint32_t ppm);
