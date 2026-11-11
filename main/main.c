#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "app_loop.h"
#include "backlight.h"
#include "screens.h"
#include "sensor_data.h"
#include "status_led.h"

#include "app_config.h"
#include "co2_net.h"
#include "fw_info.h"
#include "scd30_cfg.h"
#include "web_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"

#include "display.h"

#include "scd30_i2c.h"
#include "sensirion_common.h"
#include "sensirion_i2c_hal.h"

// How long the boot screen stays up once it shows every version, so they can
// actually be read. The network starts in the meantime, so the wait only costs
// whatever that does not already take.
#define SPLASH_HOLD_MS 3000

// Internal DMA-capable RAM is the scarce resource here: the frame buffer lives
// in PSRAM, so every display update needs internal memory that Wi-Fi also wants.
static void log_heap(const char *when)
{
    ESP_LOGI("MEM", "%-18s internal %6u B (largest block %6u B), DMA %6u B",
             when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
}

// The CPU drops to 80 MHz whenever no driver holds a lock, and the drivers in
// use (Wi-Fi, SPI, I2C) take one while they work. 80 MHz is the floor on
// purpose: below it the CPU runs from the crystal, APB stops being 80 MHz and
// the PLL can go off, which would upset the backlight PWM and the USB console.
// No light sleep: the main loop wakes every LOOP_PERIOD_MS and the USB console
// would drop.
static void pm_start(void)
{
    esp_pm_config_t pm = {
        .max_freq_mhz       = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz       = 80,
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        ESP_LOGW("PM", "frequency scaling not enabled: %s", esp_err_to_name(err));
    }
}

void app_main(void)
{
    pm_start();

    status_led_init();

    lcd_init();
    backlight_init();
    backlight_set(BACKLIGHT_FULL);

    screens_logo(NULL);

    log_heap("after display");

    // Static rather than on the stack: app_main never returns, since the
    // loop runs inside it, so anything in its frame costs stack for good.
    static app_config_t cfg;

    // A broken NVS is no reason to stop measuring: the defaults keep the
    // meter useful, and the setup page can still write new settings.
    esp_err_t cfg_err = app_config_load(&cfg);
    if (cfg_err != ESP_OK) {
        ESP_LOGE("CFG", "cannot read the settings, using defaults: %s",
                 esp_err_to_name(cfg_err));
        app_config_defaults(&cfg);
    }

    ESP_LOGI("CFG", "altitude %u m, temperature offset %.2f C",
             cfg.altitude_m, cfg.temp_offset_c100 / 100.0f);

    sensirion_i2c_hal_init();
    init_driver(SCD30_I2C_ADDR_61);

    // The SCD30 needs up to 2 s after power-up before it answers.
    sensirion_i2c_hal_sleep_usec(2000000);

    // Nothing here waits for the first measurement. The loop polls for it and
    // raises the sensor warning if it never comes, whereas waiting here would
    // hang the boot for good on a sensor that is idle, such as a new one.
    uint8_t major = 0;
    uint8_t minor = 0;
    int16_t error = NO_ERROR;
    error = scd30_read_firmware_version(&major, &minor);
    if (error != NO_ERROR) {
        ESP_LOGE("SCD30", "Error reading firmware version: %d", error);
    } else {
        ESP_LOGI("SCD30", "firmware %u.%u", major, minor);
        fw_info_set_sensor(major, minor);
    }
    ESP_LOGI("FW", "firmware %s, hardware %s", fw_info_version(), fw_info_hw_revision());
    screens_logo(fw_info_sensor());
    int64_t splash_at = esp_timer_get_time();

    // 2 s, the SCD30 default. Raising it (e.g. to 30 s) cuts power draw and
    // self-heating a lot, but Sensirion warns that changing the interval
    // degrades CO2 accuracy and forces a recalibration afterwards (FRC or ASC),
    // as well as measuring the temperature offset again.
    scd30_set_measurement_interval(SCD30_INTERVAL_S);

    sensirion_i2c_hal_sleep_usec(20000u);

    error = scd30_start_periodic_measurement(0);
    if (error != NO_ERROR) {
        ESP_LOGE("SCD30", "Error starting measurement: %d", error);
    }

    scd30_cfg_apply(cfg.altitude_m, cfg.temp_offset_c100);

    // app_config_load() has already replaced any value out of range.
    screens_chart_setup(cfg.chart_bars, cfg.chart_bar_s / SCD30_INTERVAL_S);
    ESP_LOGI("CFG", "chart %u bars of %u s", cfg.chart_bars, cfg.chart_bar_s);

    app_loop_set_display(cfg.backlight_full, cfg.backlight_dim, cfg.language,
                         cfg.led_enabled);

    // Also set here, or the LED would follow the default for the loop's
    // first pass, before the loop applies the settings above.
    status_led_set_enabled(cfg.led_enabled);

    log_heap("after sensor");

    // Started after the display on purpose: Wi-Fi takes a large bite out of
    // internal DMA-capable RAM when it associates, and the SPI bus needs that
    // same memory to push the frame buffer out of PSRAM.
    // co2_net keeps its own copies, so these are freed once it has started.
    char *ca   = app_config_load_pem(APP_PEM_CA);
    char *cert = app_config_load_pem(APP_PEM_CLIENT_CERT);
    char *key  = app_config_load_pem(APP_PEM_CLIENT_KEY);

    co2_net_config_t net = {
        .wifi_ssid    = cfg.wifi_ssid,
        .wifi_pass    = cfg.wifi_pass,
        .mqtt_uri     = cfg.mqtt_uri,
        .mqtt_user    = cfg.mqtt_user,
        .mqtt_pass    = cfg.mqtt_pass,
        .mqtt_ca      = ca,
        .mqtt_client_cert = cert,
        .mqtt_client_key  = key,
        .topic_prefix = "co2monitor",
        .topic_base   = cfg.mqtt_topic,
        .retain       = cfg.mqtt_retain,
        .device_name  = cfg.device_name,
        .ha_discovery = cfg.ha_discovery,
        .ha_prefix    = cfg.ha_prefix,
        .chart_bar_s  = cfg.chart_bar_s,
    };
    // Read unconditionally, never inside a short-circuit: it clears the flag,
    // and a flag that survives would trap the board in setup mode.
    bool ap_requested = app_config_take_ap_request();

    esp_err_t net_err = ESP_OK;

    // Registered before the server starts, so no save can come in before it.
    // The brightness, the screen language and the LED are the settings the
    // page changes without a reboot.
    web_config_on_display(app_loop_set_display);

    // Precedence matters. The long press wins over everything, because with the
    // radio disabled it is the only way back in and a device would otherwise be
    // unreachable without a cable. Then comes an explicit "no radio", which has
    // to hold even when no network is configured, or disabling Wi-Fi would not
    // disable anything. Only then the first-run access point.
    if (ap_requested || (cfg.wifi_enabled && !app_config_is_provisioned(&cfg))) {
        net_err = co2_net_start_ap(&net);
        if (net_err == ESP_OK) {
            screens_set_setup(co2_net_ap_ssid());
            web_config_start(true);
        }

        // Entered with the long press on a device that has a normal mode:
        // either a network to join or the radio deliberately off. On a first
        // boot there is nothing to go back to, so setup waits for as long as
        // it takes.
        app_loop_set_setup_mode(ap_requested
                                && (app_config_is_provisioned(&cfg) || !cfg.wifi_enabled));
    } else if (!cfg.wifi_enabled) {
        // Nothing is brought up at all: no station, no access point, no web
        // server. The meter still measures and draws, and the LED stays dark.
        ESP_LOGI("NET", "Wi-Fi disabled, running standalone");
    } else {
        net_err = co2_net_start(&net);
        if (net_err == ESP_OK) {
            // Served on the home network too, so settings can be changed
            // without dropping the device off the network first.
            web_config_start(false);
        }
    }
    if (net_err != ESP_OK) {
        ESP_LOGE("NET", "network: %s", esp_err_to_name(net_err));
    }
    free(ca);
    free(cert);
    free(key);

    log_heap("after wifi start");

    int64_t left_ms = SPLASH_HOLD_MS - (esp_timer_get_time() - splash_at) / 1000;
    if (left_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(left_ms));
    }

    app_loop();
}
