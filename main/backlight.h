#pragma once

#include <stdint.h>

// Display backlight brightness, in percent. The levels in use are settings
// (app_config); this one is only for the boot splash, before they are loaded.
#define BACKLIGHT_FULL 100

// Takes over the BL pin with a PWM channel. Call after lcd_init(), which
// holds that pin low as a plain GPIO. Starts dark.
void backlight_init(void);

// 0 turns it off, 100 is full brightness.
void backlight_set(uint8_t percent);
