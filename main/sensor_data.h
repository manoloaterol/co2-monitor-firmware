#pragma once

#include <stdint.h>

// Upper bound of what the header can render, consistent with CO2_HDR_DIGITS.
// It also keeps the chart axis within its label_digits.
#define CO2_PPM_MAX 9999

// Seconds between SCD30 measurements, which is also how often a reading
// reaches the chart. See main.c for why it is not raised.
#define SCD30_INTERVAL_S 2

// Latest sensor reading, as the screens consume it.
typedef struct {
    uint16_t ppm;
    float    temperature;
    float    humidity;
} sensor_data_t;
