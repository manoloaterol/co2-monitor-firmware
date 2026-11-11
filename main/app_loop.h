#pragma once

#include <stdbool.h>
#include <stdint.h>

// Period of the main loop, which sets the button latency. The timers that
// matter (sensor polling, the long press, the LED blink) run on esp_timer, so
// a pass that takes longer does not stretch them.
#define LOOP_PERIOD_MS 50

// Minutes the setup access point waits for someone to join before a device
// that has a normal mode to return to goes back to it.
#define SETUP_IDLE_MIN 5

// Main loop: polls the sensor, handles the joystick and repaints the screen.
// Does not return.
void app_loop(void);

// Tells the loop the device is running its setup access point. can_leave is
// true when there is a normal mode to go back to, that is, when setup was
// entered with the long press rather than because nothing is configured.
// Then a second long press, or SETUP_IDLE_MIN minutes without anyone joining
// the access point, restarts into normal mode. Call before app_loop().
void app_loop_set_setup_mode(bool can_leave);

// Brightness levels, in percent, for the normal and the dimmed screen, the
// screen language (app_lang_t) and whether the status LED is in use. Safe to
// call from another task, such as the web server's: the loop applies them on
// its next pass.
void app_loop_set_display(uint8_t full, uint8_t dim, uint8_t lang, bool led);
