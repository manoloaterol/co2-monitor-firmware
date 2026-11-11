#include <stdint.h>

#include "buttons.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"


//==============================================================================
// 3-way joystick (Korean Hroparts K1-1519SA-01): left, right and push. The
// common pin (T) goes to +3V3 through R14 of 1k, so actuating a way drives its
// GPIO high: rising edge.
//
// R14 sits in series with the common pin, it is not a pull-down towards the
// GPIOs: at rest the three inputs are left floating. The internal pull-down is
// mandatory here, without it there would be spurious interrupts.
//
// Note: on the ESP32-S2, GPIO43/44 are U0TXD/U0RXD. They do not get in the way
// here because the console runs over USB CDC (CONFIG_ESP_CONSOLE_UART_NUM=-1 in
// sdkconfig); should the console ever go back to UART0, these two pins would
// clash.
//==============================================================================
#define BTN_LEFT_GPIO   GPIO_NUM_42
#define BTN_RIGHT_GPIO  GPIO_NUM_43
#define BTN_PUSH_GPIO   GPIO_NUM_44
#define BTN_DEBOUNCE_US 200000   // 200 ms: discards the contact bounces

static volatile btn_event_t btn_event   = BTN_NONE;
static volatile int64_t     btn_last_us = 0;

// Guards btn_event between the ISR, which sets it, and btn_take(), which reads
// and clears it: an interrupt between those two steps would lose its press.
static portMUX_TYPE btn_lock = portMUX_INITIALIZER_UNLOCKED;

// Debouncing is shared by the three ways, not one per way: the contacts of
// this kind of joystick also bounce against each other when released. The price
// is that two distinct presses less than BTN_DEBOUNCE_US apart merge into one,
// which is irrelevant for moving between screens.
static void IRAM_ATTR btn_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    if (now - btn_last_us < BTN_DEBOUNCE_US) {
        return;
    }
    btn_last_us = now;

    portENTER_CRITICAL_ISR(&btn_lock);
    btn_event = (btn_event_t)(uintptr_t)arg;
    portEXIT_CRITICAL_ISR(&btn_lock);
}

void btn_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BTN_LEFT_GPIO)
                      | (1ULL << BTN_RIGHT_GPIO)
                      | (1ULL << BTN_PUSH_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    gpio_config(&cfg);

    // it is not an error if another module already installed the ISR service
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE("BTN", "gpio_install_isr_service: %s", esp_err_to_name(err));
        return;
    }

    gpio_isr_handler_add(BTN_LEFT_GPIO,  btn_isr, (void *)(uintptr_t)BTN_LEFT);
    gpio_isr_handler_add(BTN_RIGHT_GPIO, btn_isr, (void *)(uintptr_t)BTN_RIGHT);
    gpio_isr_handler_add(BTN_PUSH_GPIO,  btn_isr, (void *)(uintptr_t)BTN_PUSH);
}

// returns the pending event once and consumes it
btn_event_t btn_take(void)
{
    portENTER_CRITICAL(&btn_lock);
    btn_event_t e = btn_event;
    btn_event = BTN_NONE;
    portEXIT_CRITICAL(&btn_lock);
    return e;
}
//==============================================================================

// Timed with esp_timer rather than by counting calls: a loop pass lasts longer
// than LOOP_PERIOD_MS whenever the sensor bus is slow, and counting passes would
// then stretch the 3 s long press to many times that.
btn_push_t btn_push_poll(void)
{
    static int64_t pressed_at;    // when the contact closed; 0 while idle
    static int64_t released_at;   // when it opened again; 0 while closed
    static bool    fired;         // the long press was already reported

    int64_t now = esp_timer_get_time();

    if (gpio_get_level(BTN_PUSH_GPIO) != 0) {
        released_at = 0;
        if (pressed_at == 0) {
            pressed_at = now;
        }
        if (fired) {
            return PUSH_NONE;   // one shot per press, however long it is held
        }
        if (now - pressed_at >= BTN_LONG_MS * 1000LL) {
            fired = true;
            return PUSH_LONG;
        }
        return PUSH_NONE;
    }

    if (pressed_at == 0) {
        return PUSH_NONE;   // idle
    }

    if (released_at == 0) {
        released_at = now;
    }
    if (now - released_at < BTN_RELEASE_MS * 1000LL) {
        return PUSH_NONE;   // may still be a bounce
    }

    btn_push_t e = fired ? PUSH_NONE : PUSH_SHORT;
    pressed_at  = 0;
    released_at = 0;
    fired       = false;
    return e;
}
