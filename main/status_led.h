#pragma once

#include <stdbool.h>

void status_led_init(void);

// Off keeps the LED dark except while the setup access point is up, which is
// short-lived and asked for on purpose. Takes effect on the next update.
void status_led_set_enabled(bool on);

// Drives the LED from the current network state. Call once per loop pass; the
// blink follows the clock, not the number of calls.
void status_led_update(void);
