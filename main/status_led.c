#include <stdbool.h>
#include <stdint.h>

#include "status_led.h"

#include "co2_net.h"

#include "driver/gpio.h"
#include "esp_timer.h"

// Blue LED on GPIO 7.
//
// Active low: D3 sits between +3V3 and the pin through R11, so the pin sinks
// the current. Driving it high puts both ends at 3V3 and the LED goes out.
#define LED_GPIO     GPIO_NUM_7
#define LED_ON       0
#define LED_OFF      1

// Half period of the blink, in milliseconds. Slow enough to read as "waiting"
// rather than as a fault.
#define BLINK_MS     500

// Set at boot and then only by the display loop, which is also the one
// reading it.
static bool s_enabled = true;

void status_led_set_enabled(bool on)
{
    s_enabled = on;
}

void status_led_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(LED_GPIO, LED_OFF);
}

void status_led_update(void)
{
    // Derived from the clock, so the blink keeps its rate however long a loop
    // pass takes.
    bool blink = (esp_timer_get_time() / (BLINK_MS * 1000LL)) % 2 == 0;
    bool lit;

    // Steady means "there is a link": either someone is using the setup access
    // point, or the device is on the home network and, if a broker is set,
    // connected to it. Blinking means the radio is up but waiting for the
    // other end, be it the access point or the broker.
    switch (co2_net_state()) {
    case CO2_NET_CONNECTED:
        lit = s_enabled && (co2_net_is_connected() || !co2_net_broker_set() || blink);
        break;

    case CO2_NET_CONNECTING:
        lit = blink && s_enabled;
        break;

    case CO2_NET_AP_CLIENT:
        lit = true;
        break;

    case CO2_NET_AP_IDLE:
        lit = blink;
        break;

    case CO2_NET_OFF:
    default:
        lit = false;
        break;
    }

    gpio_set_level(LED_GPIO, lit ? LED_ON : LED_OFF);
}
