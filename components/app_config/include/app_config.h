#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Language of the text on the screen. The setup page has its own selector,
// per browser, and only starts from this one.
typedef enum {
    APP_LANG_EN,
    APP_LANG_ES,
    APP_LANG_COUNT,
} app_lang_t;

// Device settings, held in NVS.
//
// This is the single source of truth for everything the user can change from
// the web interface. Nothing personal is compiled in: a board whose NVS is
// empty has no network, and starts in setup mode.
//
// Stored as one NVS blob, so a save either lands whole or leaves the previous
// settings. The blob is the raw struct: new fields go at the END, where an
// older blob simply lacks them and they keep their defaults. Removing or
// reordering a field breaks every stored copy unless BLOB_VERSION in
// app_config.c goes up with it.
typedef struct {
    char wifi_ssid[33];     // 32 bytes plus terminator, per the 802.11 limit
    char wifi_pass[65];

    // With the radio off the device is a standalone meter: no station, no
    // access point, nothing transmitted. It is also the largest power saving
    // available, since the radio runs without modem sleep.
    bool wifi_enabled;

    char mqtt_uri[128];     // "mqtt://host:port" or "mqtts://host:port"
    char mqtt_user[65];
    char mqtt_pass[65];
    bool ha_discovery;

    // Root of the Home Assistant discovery tree. "homeassistant" unless HA was
    // set up with another one; on a change, co2_net_goodbye() clears the old
    // config messages before the restart.
    char ha_prefix[32];

    // Every topic hangs off this, e.g. "home/livingroom/co2". Empty means the
    // default "co2monitor/<mac>", which is unique per board without any setup.
    char mqtt_topic[64];

    // Publish readings retained, so a new subscriber gets the last value at
    // once instead of waiting for the next chart bar.
    bool mqtt_retain;

    char device_name[48];   // shown, never used as an identifier; empty means
                            // "derive one from the MAC"

    // Height above sea level in metres. The SCD30 needs it because CO2
    // measurement by NDIR depends on air pressure, and altitude dominates the
    // weather variation. See docs/escala-color-co2.md.
    uint16_t altitude_m;

    // Degrees Celsius times 100, always positive: the SCD30 reads high because
    // of its own self-heating and this value is subtracted from the reading.
    // See docs/temperatura-autocalentamiento.md.
    uint16_t temp_offset_c100;

    // Backlight, in percent. Applied at once, without a reboot.
    uint8_t backlight_full;
    uint8_t backlight_dim;     // 0 turns the screen off

    uint8_t language;          // app_lang_t; applied at once, like the brightness

    // History chart. A bar lasts chart_bar_s seconds, and that is also how
    // often readings are published, since each bar goes out as one average.
    // Only the values in APP_CHART_BARS and APP_CHART_BAR_S are accepted.
    uint16_t chart_bars;
    uint16_t chart_bar_s;

    // The blue status LED. Off keeps it dark in normal operation, for a meter
    // in a bedroom; the setup access point still shows on it. Applied at once.
    bool led_enabled;
} app_config_t;

// The chart is 200 px wide and its bars must all have the same width, so the
// count has to divide 200. The largest one sizes the buffer.
#define APP_CHART_BARS    { 20, 25, 40, 50, 100 }
#define APP_CHART_BARS_MAX 100

// Multiples of the 2 s sensor interval.
#define APP_CHART_BAR_S   { 20, 30, 60, 120, 300 }

// "en", "es": the codes the setup page uses. NULL for an unknown value, and
// APP_LANG_COUNT for an unknown code.
const char *app_config_lang_code(uint8_t lang);
uint8_t     app_config_lang_from_code(const char *code);

// Limits the setup page enforces and app_config_load() restores. The normal
// brightness has a floor so a mistake cannot leave the screen unreadable.
#define APP_BACKLIGHT_FULL_MIN 5
#define APP_ALTITUDE_MAX       9000   // metres, above Everest

// True if the value is one of the listed ones.
bool app_config_chart_bars_valid(uint16_t bars);
bool app_config_chart_bar_s_valid(uint16_t seconds);

// Fills cfg with the built-in defaults. Never fails.
void app_config_defaults(app_config_t *cfg);

// Starts from the defaults and overwrites every field NVS holds. A missing key
// is not an error: a board that was never configured comes back with defaults.
// A stored value out of range is replaced by its default, so callers can use
// every field as it comes.
esp_err_t app_config_load(app_config_t *cfg);

esp_err_t app_config_save(const app_config_t *cfg);

// Wipes the settings namespace. The next load returns the defaults.
esp_err_t app_config_erase(void);

// True once there is enough to join a network: an SSID. Says nothing about
// whether the user wants to join one; check wifi_enabled for that. Without it
// the device starts its own access point for setup.
bool app_config_is_provisioned(const app_config_t *cfg);

// A one-shot request to come back up in provisioning mode, set by the long
// press on the joystick. It survives the reboot in NVS rather than switching
// the radio live, which would mean tearing down the station interface and the
// MQTT client from inside the display loop.
esp_err_t app_config_request_ap(void);

// Reads the request and clears it, so a board cannot get stuck in setup mode.
bool app_config_take_ap_request(void);

// TLS material for mqtts://, in PEM. Kept apart from app_config_t because each
// one can run to a few kilobytes and app_config_t lives on the stack of the
// HTTP handlers.
typedef enum {
    // The broker's CA, for a broker whose certificate is not signed by a
    // public authority (a self-signed Mosquitto, for instance).
    APP_PEM_CA,

    // The device's own certificate and private key, for brokers that
    // authenticate clients by certificate instead of by password (Azure Event
    // Grid, AWS IoT). The key is stored in the clear, like the passwords: NVS
    // encryption is not enabled.
    APP_PEM_CLIENT_CERT,
    APP_PEM_CLIENT_KEY,
} app_pem_t;

// With terminator. The NVS limit for a string; enough for an RSA 4096 key.
#define APP_CONFIG_PEM_MAX 4000

// Returns a heap copy of the stored PEM, or NULL when there is none. The
// caller frees it.
char *app_config_load_pem(app_pem_t which);

// An empty string or NULL removes it.
esp_err_t app_config_save_pem(app_pem_t which, const char *pem);

// The last forced recalibration of the SCD30, kept so the page can show it and
// undo it. The sensor stores the correction itself but offers no way to read
// it back or to return to the factory curve; knowing what it read and what it
// was told is what allows the opposite correction later.
typedef struct {
    uint16_t before_ppm;   // what the sensor read, averaged over two minutes
    uint16_t ref_ppm;      // the reference it was calibrated to
    uint32_t at;           // Unix time from the browser; 0 when unknown, since
                           // the device has no clock of its own
} app_frc_t;

// Returns false when no calibration is recorded.
bool app_config_load_frc(app_frc_t *frc);

// NULL removes the record, as after an undo.
esp_err_t app_config_save_frc(const app_frc_t *frc);
