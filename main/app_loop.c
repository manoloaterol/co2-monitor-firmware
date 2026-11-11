#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "app_loop.h"

#include "backlight.h"
#include "buttons.h"
#include "screens.h"
#include "status_led.h"
#include "web_config.h"
#include "sensor_data.h"

#include "app_config.h"
#include "co2_net.h"
#include "fw_info.h"
#include "scd30_cfg.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "display.h"

#include "scd30_i2c.h"
#include "sensirion_i2c.h"
#include "sensirion_i2c_hal.h"
#include "sensirion_common.h"

#define SCD30_POLL_MS 250   // how often the sensor is asked whether data is ready

// Without a usable reading for this long the screen says so. Five missed
// measurements: long enough to ride out a single glitch on the bus, short
// enough that a loose cable shows up before anyone trusts a stale value.
#define SENSOR_STALE_US (5LL * SCD30_INTERVAL_S * 1000000)

// Names the error in the log, so a report says what failed without anyone
// looking codes up. The HAL's own codes are negative, the driver's positive.
static const char *sensor_err_str(int16_t err)
{
    switch (err) {
    case SENSIRION_HAL_ERR_NACK:    return "no ACK: sensor not answering (wiring, power)";
    case SENSIRION_HAL_ERR_TIMEOUT: return "I2C timeout: bus held low or clock stretched too long";
    case SENSIRION_HAL_ERR_OTHER:   return "transfer failed, sensor answers again: a glitch on the bus";
    case CRC_ERROR:                 return "CRC mismatch: corrupted data on the bus";
    default:                        return "I2C error";
    }
}

static uint8_t screen_idx = 0;
static bool    dimmed     = false;

// Written by the web server's task, read here. Each value is a single byte,
// so a torn read cannot happen; the flag only asks for them to be applied.
// Placeholders: main.c passes the stored settings before the loop starts.
static volatile uint8_t bl_full = BACKLIGHT_FULL;
static volatile uint8_t bl_dim  = BACKLIGHT_FULL;
static volatile uint8_t ui_lang;
static volatile bool    led_on = true;
static volatile bool    display_changed;

void app_loop_set_display(uint8_t full, uint8_t dim, uint8_t lang, bool led)
{
    bl_full         = full;
    bl_dim          = dim;
    ui_lang         = lang;
    led_on          = led;
    display_changed = true;
}

// See app_loop_set_setup_mode().
static bool setup_mode;
static bool setup_can_leave;

void app_loop_set_setup_mode(bool can_leave)
{
    setup_mode      = true;
    setup_can_leave = can_leave;
}

// Long enough to read the notice, short enough not to feel like a hang.
#define RESTART_NOTICE_MS 1500

// Shows what is about to happen and restarts. Setup mode itself is decided at
// boot, from the flag app_config_request_ap() leaves in NVS: the flag is
// cleared on every boot, so a restart without it lands in normal mode.
// The settings do not change on this path, hence the NULL for the goodbye; it
// runs while the notice is up, so it only lengthens the wait if it is slow.
static void restart_to(bool setup)
{
    backlight_set(bl_full);   // the notice must be readable even if dimmed
    screens_show_restart(setup);

    int64_t shown_at = esp_timer_get_time();
    co2_net_goodbye(NULL);

    int64_t left_ms = RESTART_NOTICE_MS - (esp_timer_get_time() - shown_at) / 1000;
    if (left_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(left_ms));
    }
    esp_restart();
}

static void set_dimmed(bool on)
{
    dimmed = on;
    backlight_set(on ? bl_dim : bl_full);
    ESP_LOGI("UI", "backlight %s", on ? "dimmed" : "full");
}

// Readings are published once per chart bar, as its average, rather than one
// by one: Home Assistant then shows the same curve as the screen, and the radio
// wakes up a tenth as often. Only usable readings count, so an average is never
// dragged down by a zero.
typedef struct {
    uint32_t ppm;
    float    temperature;
    float    humidity;
    uint16_t n;
} pub_acc_t;

static pub_acc_t pub;

static void publish_add(uint16_t ppm, float temperature, float humidity)
{
    pub.ppm         += ppm;
    pub.temperature += temperature;
    pub.humidity    += humidity;
    pub.n++;
}

static void publish_flush(void)
{
    if (pub.n != 0) {
        co2_net_publish((uint16_t)(pub.ppm / pub.n),
                        pub.temperature / pub.n, pub.humidity / pub.n);
    }
    pub = (pub_acc_t){0};
}

// Tracks whether readings keep arriving, and logs a fault once per episode
// instead of once per failed poll, which would be four lines a second.
static int64_t  last_ok_us;
static bool     sensor_fault;
static uint32_t sensor_errors;   // failed transactions in this episode

static void sensor_error(int16_t err, const char *what)
{
    if (sensor_errors++ == 0) {
        ESP_LOGW("SCD30", "%s failed: %d, %s", what, err, sensor_err_str(err));
    }
}

// Returns true when the fault state changed, so the caller repaints.
static bool sensor_check(bool reading_ok)
{
    int64_t now = esp_timer_get_time();

    if (reading_ok) {
        last_ok_us = now;
        if (sensor_errors != 0) {
            ESP_LOGI("SCD30", "reading again after %lu failed transactions",
                     (unsigned long)sensor_errors);
            sensor_errors = 0;
        }
    }

    bool fault = (now - last_ok_us) > SENSOR_STALE_US;
    if (fault == sensor_fault) {
        return false;
    }
    sensor_fault = fault;
    if (fault) {
        ESP_LOGW("SCD30", "no reading for %lld s, showing the warning",
                 SENSOR_STALE_US / 1000000);
    } else {
        ESP_LOGI("SCD30", "sensor back, warning cleared");
    }
    screens_set_sensor_fault(fault);
    web_config_set_sensor_ok(!fault);
    return true;
}

void app_loop(void)
{
    sensor_data_t data = {0};
    bool     redraw    = true;   // draw right away, without waiting for the first reading
    int64_t  last_poll_us = 0;   // 0 forces a poll on the first pass

    btn_init();
    last_ok_us = esp_timer_get_time();

    // This loop runs on the main task, whose stack is the stock 3.5 KB and
    // also holds app_main's frame. Reported once, after a minute of readings,
    // drawing and publishing, so a change that eats the margin shows up.
    int64_t stack_report_at = esp_timer_get_time() + 60 * 1000000LL;

    // Last moment the setup access point had someone on it, or setup started.
    int64_t setup_busy_at = esp_timer_get_time();

    while (1)
    {
        uint16_t data_ready = 0;
        int16_t  error      = NO_ERROR;

        // The loop spins at LOOP_PERIOD_MS because of the button, but the sensor
        // does not need to be asked that often: with a 30 s interval that would
        // be 600 I2C transactions per measurement. We poll it every
        // SCD30_POLL_MS. Measured in time rather than in passes, since a
        // pass lasts longer than LOOP_PERIOD_MS whenever the bus is slow.
        //
        // Polling instead of blocking on scd30_await_data_ready() is what keeps
        // the button alive: that wait lasts up to a whole interval.
        int64_t now_us = esp_timer_get_time();
        if (last_poll_us == 0 || now_us - last_poll_us >= SCD30_POLL_MS * 1000LL)
        {
            last_poll_us = now_us;
            error = scd30_get_data_ready(&data_ready);
        }
        bool reading_ok = false;

        if (error != NO_ERROR)
        {
            sensor_error(error, "data ready");
        }
        else if (data_ready)
        {
            float co2_ppm = 0.0;
            float temperature = 0.0;
            float humidity = 0.0;

            error = scd30_read_measurement_data(&co2_ppm, &temperature, &humidity);
            if (error != NO_ERROR)
            {
                sensor_error(error, "read measurement");
            }
            else
            {
                // Printed as floats: casting a NaN or a negative value to an
                // integer here would be the UB the clamp below avoids.
                ESP_LOGI("SCD30", "CO2: %4.0f ppm, Temperature: %.2f C, Humidity: %.2f %%",
                         co2_ppm, temperature, humidity);

                // clamp once, so header and chart use the same value. The
                // !(x > 0) also catches NaN, whose conversion to an integer
                // would be UB
                data.ppm = !(co2_ppm > 0.0f)       ? 0
                         : (co2_ppm > CO2_PPM_MAX) ? CO2_PPM_MAX
                                                   : (uint16_t)co2_ppm;
                data.temperature = temperature;
                data.humidity    = humidity;

                // The already clamped value is published, never the raw float:
                // casting a NaN to an integer would be UB, which is the same
                // reason the clamp above exists. data.ppm == 0 means the
                // reading was not usable, so it is left out, and so is a NaN
                // temperature or humidity, which would poison the average.
                if (data.ppm != 0 && isfinite(temperature) && isfinite(humidity)) {
                    publish_add(data.ppm, temperature, humidity);
                }
                // An unusable reading stays out of the chart as well: a zero
                // would drag the bar's average down, and a bar of zeros would
                // pin the baseline to 0 for the whole window.
                reading_ok = (data.ppm != 0);

                // The version is read once at boot, and a sensor that was
                // still waking up then leaves it empty for good. The sensor
                // answering now is the moment to ask again.
                if (reading_ok && fw_info_sensor()[0] == '\0') {
                    uint8_t major = 0;
                    uint8_t minor = 0;
                    if (scd30_read_firmware_version(&major, &minor) == NO_ERROR) {
                        ESP_LOGI("SCD30", "firmware %u.%u, read late", major, minor);
                        fw_info_set_sensor(major, minor);
                    }
                }

                if (reading_ok) {
                    web_config_set_reading(data.ppm, temperature, humidity);
                    if (screens_add_reading(data.ppm)) {
                        publish_flush();
                    }
                }

                // A reading arrived, so the sensor, the I2C bus and this loop
                // are alive. CONNECTING means the radio is still trying, which
                // is exactly the state a bad update would be stuck in.
                if (data.ppm != 0 && co2_net_state() != CO2_NET_CONNECTING) {
                    ota_mark_healthy();
                }

                redraw = true;
            }
        }

        // While the warning is up the last values are not shown: they are
        // exactly what a user might mistake for a live reading. Blanked on
        // every pass, not only when the warning appears, because an unusable
        // reading can still carry a temperature and humidity.
        if (sensor_check(reading_ok)) {
            redraw = true;
        }
        if (sensor_fault) {
            data.ppm         = 0;
            data.temperature = NAN;
            data.humidity    = NAN;
        }

        status_led_update();

        // Here rather than in the HTTP task, which asks for it: this loop owns
        // the sensor's bus, and this point sits between two of its own
        // transactions.
        scd30_cfg_service();

        // A long press by accident, or setup left halfway, must not keep the
        // meter off the network for good. Only a client on the access point
        // counts as activity: the page is only reachable through it.
        if (setup_mode && setup_can_leave) {
            int64_t now = esp_timer_get_time();
            if (co2_net_state() == CO2_NET_AP_CLIENT) {
                setup_busy_at = now;
            } else if (now - setup_busy_at >= SETUP_IDLE_MIN * 60 * 1000000LL) {
                ESP_LOGW("UI", "nobody joined setup in %d min, back to normal mode",
                         SETUP_IDLE_MIN);
                restart_to(false);
            }
        }

        if (display_changed) {
            display_changed = false;
            backlight_set(dimmed ? bl_dim : bl_full);
            screens_set_language(ui_lang);
            status_led_set_enabled(led_on);
            redraw = true;
        }

        switch (btn_push_poll())
        {
        case PUSH_LONG:
            // Held down rather than clicked: entering setup drops the network,
            // so it should not be one stray press away. In setup mode the same
            // press is the way back, when there is somewhere to go back to.
            if (setup_mode && setup_can_leave) {
                ESP_LOGW("UI", "leaving setup mode, rebooting");
                restart_to(false);
            } else if (setup_mode) {
                ESP_LOGI("UI", "nothing configured yet, staying in setup mode");
            } else {
                ESP_LOGW("UI", "setup requested, rebooting");
                if (app_config_request_ap() != ESP_OK) {
                    // Rebooting anyway gives a visible response to the press;
                    // the device just comes back in normal mode.
                    ESP_LOGE("UI", "cannot store the setup request");
                }
                restart_to(true);
            }
            break;

        case PUSH_SHORT:
            set_dimmed(!dimmed);
            break;

        default:
            break;
        }

        btn_event_t ev = btn_take();

        // While dimmed, left and right only restore the brightness: the
        // screen is hard to read that way, so a page change would go unseen.
        if (dimmed && (ev == BTN_LEFT || ev == BTN_RIGHT)) {
            set_dimmed(false);
            ev = BTN_NONE;
        }

        switch (ev)
        {
        case BTN_RIGHT:
        case BTN_LEFT:
            screen_idx = ev == BTN_RIGHT
                       ? (screen_idx + 1) % screens_count()
                       : (screen_idx + screens_count() - 1) % screens_count();
            ESP_LOGI("UI", "screen %u/%u: %s", screen_idx + 1,
                     (unsigned)screens_count(), screens_name(screen_idx));
            redraw = true;
            break;

        // BTN_PUSH is ignored: the centre button is handled by
        // btn_push_poll(). It stays wired to the interrupt only so that it
        // shares the debounce window with the other two ways.
        default:
            break;
        }

        if (redraw)
        {
            lcd_clear();
            screens_draw(screen_idx, &data);
            lcd_flush();
            redraw = false;
        }

        if (stack_report_at != 0 && esp_timer_get_time() >= stack_report_at) {
            stack_report_at = 0;
            ESP_LOGI("MEM", "main task stack: %u B never used",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }

        vTaskDelay(pdMS_TO_TICKS(LOOP_PERIOD_MS));
    }
}
