#include "backlight.h"

#include "driver/ledc.h"
#include "esp_log.h"

#include "display.h"

// The BL pin is active high. lcd_init() holds it low, and the LEDC output
// replaces that GPIO level once the channel is set up.
//
// 5 kHz is well above visible flicker and 10 bits give a 0.1 % step, fine
// enough for the dim level. The timer runs from APB, which stays at 80 MHz
// under frequency scaling as long as the CPU minimum is not below 80 MHz (see
// main.c), so the PWM frequency does not drift when the CPU slows down.
#define BL_MODE     LEDC_LOW_SPEED_MODE
#define BL_TIMER    LEDC_TIMER_0
#define BL_CHANNEL  LEDC_CHANNEL_0
#define BL_FREQ_HZ  5000
#define BL_RES      LEDC_TIMER_10_BIT
#define BL_DUTY_MAX ((1u << BL_RES) - 1)

void backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = BL_MODE,
        .timer_num       = BL_TIMER,
        .duty_resolution = BL_RES,
        .freq_hz         = BL_FREQ_HZ,
        .clk_cfg         = LEDC_USE_APB_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t ch = {
        .gpio_num   = LCD_PIN_BL,
        .speed_mode = BL_MODE,
        .channel    = BL_CHANNEL,
        .timer_sel  = BL_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
}

void backlight_set(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    ledc_set_duty(BL_MODE, BL_CHANNEL, BL_DUTY_MAX * percent / 100);
    ledc_update_duty(BL_MODE, BL_CHANNEL);
}
