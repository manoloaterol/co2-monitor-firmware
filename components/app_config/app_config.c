#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "cfg";

#define NS              "co2cfg"

// The settings, all of app_config_t in one entry. NVS replaces a blob as a
// whole: the new copy is written in full before the old one is dropped, so a
// failed write or a power cut leaves the previous settings rather than a mix
// of old and new. Stored behind a version byte, see load_blob().
#define KEY_BLOB        "cfg"
#define BLOB_VERSION    1

// Keys kept apart from the blob: the TLS material, which is too large for
// app_config_t, the one-shot setup request, and the record of the last CO2
// calibration, which is not a setting.
#define KEY_MQTT_CA     "mqtt_ca"
#define KEY_MQTT_CERT   "mqtt_cert"
#define KEY_MQTT_KEY    "mqtt_key"
#define KEY_FORCE_AP    "force_ap"
#define KEY_FRC         "frc"

// The layout before the blob: one key per field. Only read, to migrate a board
// set up with an older firmware, and left in place, so going back to such a
// firmware finds the settings as they were when this one first started.
// NVS keys are limited to 15 characters, so some are shorter than the field
// they hold.
#define KEY_WIFI_SSID   "wifi_ssid"
#define KEY_WIFI_PASS   "wifi_pass"
#define KEY_MQTT_URI    "mqtt_uri"
#define KEY_MQTT_USER   "mqtt_user"
#define KEY_MQTT_PASS   "mqtt_pass"
#define KEY_HA_DISC     "ha_disc"
#define KEY_MQTT_TOPIC  "mqtt_topic"
#define KEY_MQTT_RETAIN "mqtt_retain"
#define KEY_DEV_NAME    "dev_name"
#define KEY_ALTITUDE    "altitude"
#define KEY_TEMP_OFF    "temp_off"
#define KEY_WIFI_EN     "wifi_en"
#define KEY_SAVED       "saved"
#define KEY_HA_PREFIX   "ha_prefix"
#define KEY_BL_FULL     "bl_full"
#define KEY_BL_DIM      "bl_dim"
#define KEY_CHART_BARS  "chart_bars"
#define KEY_CHART_BAR_S "chart_bar_s"
#define KEY_LANG        "lang"

static const char *const s_lang_codes[APP_LANG_COUNT] = { "en", "es" };

const char *app_config_lang_code(uint8_t lang)
{
    return lang < APP_LANG_COUNT ? s_lang_codes[lang] : NULL;
}

uint8_t app_config_lang_from_code(const char *code)
{
    for (uint8_t i = 0; code != NULL && i < APP_LANG_COUNT; i++) {
        if (strcmp(code, s_lang_codes[i]) == 0) {
            return i;
        }
    }
    return APP_LANG_COUNT;
}

static const uint16_t s_chart_bars[]  = APP_CHART_BARS;
static const uint16_t s_chart_bar_s[] = APP_CHART_BAR_S;

static bool in_list(uint16_t v, const uint16_t *list, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (list[i] == v) {
            return true;
        }
    }
    return false;
}

bool app_config_chart_bars_valid(uint16_t bars)
{
    return in_list(bars, s_chart_bars, sizeof(s_chart_bars) / sizeof(s_chart_bars[0]));
}

bool app_config_chart_bar_s_valid(uint16_t seconds)
{
    return in_list(seconds, s_chart_bar_s, sizeof(s_chart_bar_s) / sizeof(s_chart_bar_s[0]));
}

void app_config_defaults(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    // Discovery on by default: it costs three retained messages and is what
    // makes the device usable out of the box for the intended audience.
    cfg->ha_discovery = true;

    // The radio is on unless the user says otherwise: a meter nobody can reach
    // is a surprising thing to find out of the box.
    cfg->wifi_enabled = true;

    snprintf(cfg->ha_prefix, sizeof(cfg->ha_prefix), "homeassistant");

    // What the device shipped with before these became settings. 40 bars of
    // 20 s give a quarter of an hour of history.
    cfg->backlight_full = 100;
    cfg->backlight_dim  = 3;
    cfg->chart_bars     = 40;
    cfg->chart_bar_s    = 20;
    cfg->language       = APP_LANG_EN;
    cfg->led_enabled    = true;

    // Altitude and offset stay at zero, which means "no compensation". Making
    // up a value would be worse than not compensating: a wrong altitude biases
    // every reading in a way nobody would think to question.
}

// Reads one string key, leaving the destination untouched if it is not there.
static void load_str(nvs_handle_t h, const char *key, char *dst, size_t len)
{
    size_t n = len;
    esp_err_t err = nvs_get_str(h, key, dst, &n);

    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "%s: %s", key, esp_err_to_name(err));
    }
}

static void load_u8(nvs_handle_t h, const char *key, uint8_t *dst)
{
    uint8_t v = 0;
    if (nvs_get_u8(h, key, &v) == ESP_OK) {
        *dst = v;
    }
}

static void load_u16(nvs_handle_t h, const char *key, uint16_t *dst)
{
    uint16_t v = 0;
    if (nvs_get_u16(h, key, &v) == ESP_OK) {
        *dst = v;
    }
}

// The page validates everything it saves, so this only catches values written
// by an older firmware or by hand. Each one falls back to its default rather
// than being clamped: a clamp would produce a value nobody chose either.
static void sanitize(app_config_t *cfg)
{
    app_config_t def;
    app_config_defaults(&def);

    // A blob is copied in as raw bytes, so nothing else guarantees that the
    // strings end inside their buffers.
    #define TERMINATE(field) ((field)[sizeof(field) - 1] = '\0')
    TERMINATE(cfg->wifi_ssid);
    TERMINATE(cfg->wifi_pass);
    TERMINATE(cfg->mqtt_uri);
    TERMINATE(cfg->mqtt_user);
    TERMINATE(cfg->mqtt_pass);
    TERMINATE(cfg->ha_prefix);
    TERMINATE(cfg->mqtt_topic);
    TERMINATE(cfg->device_name);
    #undef TERMINATE

    if (cfg->backlight_full < APP_BACKLIGHT_FULL_MIN || cfg->backlight_full > 100) {
        cfg->backlight_full = def.backlight_full;
    }
    if (cfg->backlight_dim > 100) {
        cfg->backlight_dim = def.backlight_dim;
    }
    if (!app_config_chart_bars_valid(cfg->chart_bars)) {
        cfg->chart_bars = def.chart_bars;
    }
    if (!app_config_chart_bar_s_valid(cfg->chart_bar_s)) {
        cfg->chart_bar_s = def.chart_bar_s;
    }
    if (cfg->language >= APP_LANG_COUNT) {
        cfg->language = def.language;
    }
    if (cfg->altitude_m > APP_ALTITUDE_MAX) {
        cfg->altitude_m = def.altitude_m;
    }
}

// One byte of version ahead of the struct. Fields are only ever appended to
// app_config_t, so a shorter blob from an older firmware is read as far as it
// goes and the defaults fill the rest, and a longer one from a newer firmware
// gives up only the fields this one does not know. Anything else, a field
// removed or reordered, has to raise BLOB_VERSION, and an unknown version is
// ignored rather than misread.
static esp_err_t load_blob(nvs_handle_t h, app_config_t *cfg)
{
    size_t len = 0;

    esp_err_t err = nvs_get_blob(h, KEY_BLOB, NULL, &len);
    if (err != ESP_OK) {
        return err;
    }
    if (len < 1) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buf = malloc(len);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_blob(h, KEY_BLOB, buf, &len);
    if (err == ESP_OK && buf[0] != BLOB_VERSION) {
        ESP_LOGW(TAG, "settings version %u unknown, using defaults", buf[0]);
        err = ESP_ERR_NOT_SUPPORTED;
    }
    if (err == ESP_OK) {
        size_t n = len - 1;
        memcpy(cfg, buf + 1, n < sizeof(*cfg) ? n : sizeof(*cfg));
    }

    free(buf);
    return err;
}

// True if a firmware from before the blob left settings behind. The "saved"
// flag came late, so an SSID counts as well, as it did then.
static bool legacy_saved(nvs_handle_t h)
{
    uint8_t v   = 0;
    size_t  len = 0;

    return (nvs_get_u8(h, KEY_SAVED, &v) == ESP_OK && v != 0)
        || (nvs_get_str(h, KEY_WIFI_SSID, NULL, &len) == ESP_OK && len > 1);
}

static void load_legacy(nvs_handle_t h, app_config_t *cfg)
{
    load_str(h, KEY_WIFI_SSID, cfg->wifi_ssid,   sizeof(cfg->wifi_ssid));
    load_str(h, KEY_WIFI_PASS, cfg->wifi_pass,   sizeof(cfg->wifi_pass));
    load_str(h, KEY_MQTT_URI,  cfg->mqtt_uri,    sizeof(cfg->mqtt_uri));
    load_str(h, KEY_MQTT_USER, cfg->mqtt_user,   sizeof(cfg->mqtt_user));
    load_str(h, KEY_MQTT_PASS, cfg->mqtt_pass,   sizeof(cfg->mqtt_pass));
    load_str(h, KEY_DEV_NAME,  cfg->device_name, sizeof(cfg->device_name));
    load_str(h, KEY_MQTT_TOPIC, cfg->mqtt_topic, sizeof(cfg->mqtt_topic));

    load_u16(h, KEY_ALTITUDE, &cfg->altitude_m);
    load_u16(h, KEY_TEMP_OFF, &cfg->temp_offset_c100);
    load_str(h, KEY_HA_PREFIX, cfg->ha_prefix, sizeof(cfg->ha_prefix));
    load_u8(h,  KEY_BL_FULL,   &cfg->backlight_full);
    load_u8(h,  KEY_BL_DIM,    &cfg->backlight_dim);
    load_u16(h, KEY_CHART_BARS,  &cfg->chart_bars);
    load_u16(h, KEY_CHART_BAR_S, &cfg->chart_bar_s);
    load_u8(h,  KEY_LANG,        &cfg->language);

    uint8_t flag = 0;
    if (nvs_get_u8(h, KEY_HA_DISC, &flag) == ESP_OK) {
        cfg->ha_discovery = (flag != 0);
    }
    if (nvs_get_u8(h, KEY_WIFI_EN, &flag) == ESP_OK) {
        cfg->wifi_enabled = (flag != 0);
    }
    if (nvs_get_u8(h, KEY_MQTT_RETAIN, &flag) == ESP_OK) {
        cfg->mqtt_retain = (flag != 0);
    }
}

esp_err_t app_config_load(app_config_t *cfg)
{
    nvs_handle_t h;

    app_config_defaults(cfg);

    // Safe to call more than once; Wi-Fi needs NVS up as well.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no settings stored yet, using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    bool migrate = false;
    err = load_blob(h, cfg);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        migrate = legacy_saved(h);
        if (migrate) {
            load_legacy(h, cfg);
        }
        err = ESP_OK;
    } else if (err != ESP_OK) {
        // Whatever load_blob() copied is not to be trusted.
        ESP_LOGE(TAG, "settings unreadable: %s", esp_err_to_name(err));
        app_config_defaults(cfg);
    }
    nvs_close(h);

    sanitize(cfg);

    // Once only: from then on the blob is found and the old keys are ignored.
    // A failure is not fatal, the old keys are still there for the next boot.
    if (migrate) {
        esp_err_t e = app_config_save(cfg);
        if (e == ESP_OK) {
            ESP_LOGI(TAG, "settings moved to the single-entry format");
        } else {
            ESP_LOGW(TAG, "cannot move the settings yet: %s", esp_err_to_name(e));
        }
    }
    return err;
}

esp_err_t app_config_save(const app_config_t *cfg)
{
    nvs_handle_t h;

    // Static: this runs on the HTTP handler's stack, which is short, and on
    // one task at a time, the web server or the boot.
    static uint8_t buf[1 + sizeof(app_config_t)];

    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    buf[0] = BLOB_VERSION;
    memcpy(buf + 1, cfg, sizeof(*cfg));

    // The only write: either all of it lands or none of it does.
    err = nvs_set_blob(h, KEY_BLOB, buf, sizeof(buf));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}

esp_err_t app_config_erase(void)
{
    nvs_handle_t h;

    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}

esp_err_t app_config_request_ap(void)
{
    nvs_handle_t h;

    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(h, KEY_FORCE_AP, 1);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}

bool app_config_take_ap_request(void)
{
    nvs_handle_t h;
    uint8_t v = 0;

    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }

    if (nvs_get_u8(h, KEY_FORCE_AP, &v) == ESP_OK && v != 0) {
        // Cleared straight away: if setup is abandoned halfway, the next boot
        // goes back to normal operation instead of waiting forever. A flag
        // that cannot be cleared would bring every boot back to setup mode,
        // so it is at least reported.
        esp_err_t err = nvs_erase_key(h, KEY_FORCE_AP);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "cannot clear the setup request: %s", esp_err_to_name(err));
        }
        nvs_close(h);
        return true;
    }

    nvs_close(h);
    return false;
}

bool app_config_is_provisioned(const app_config_t *cfg)
{
    // The password is not checked: open networks are legitimate.
    return cfg->wifi_ssid[0] != '\0';
}

static const char *pem_key(app_pem_t which)
{
    switch (which) {
    case APP_PEM_CA:          return KEY_MQTT_CA;
    case APP_PEM_CLIENT_CERT: return KEY_MQTT_CERT;
    case APP_PEM_CLIENT_KEY:  return KEY_MQTT_KEY;
    }
    return NULL;
}

char *app_config_load_pem(app_pem_t which)
{
    nvs_handle_t h;
    size_t len = 0;
    const char *key = pem_key(which);

    if (key == NULL || nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return NULL;
    }

    char *pem = NULL;
    if (nvs_get_str(h, key, NULL, &len) == ESP_OK && len > 1) {
        pem = malloc(len);
        if (pem != NULL && nvs_get_str(h, key, pem, &len) != ESP_OK) {
            free(pem);
            pem = NULL;
        }
    }

    nvs_close(h);
    return pem;
}

esp_err_t app_config_save_pem(app_pem_t which, const char *pem)
{
    nvs_handle_t h;
    const char *key = pem_key(which);

    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pem != NULL && strlen(pem) >= APP_CONFIG_PEM_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    if (pem == NULL || pem[0] == '\0') {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        err = nvs_set_str(h, key, pem);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}

bool app_config_load_frc(app_frc_t *frc)
{
    nvs_handle_t h;

    if (frc == NULL || nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    // Only a record of exactly this size is trusted: it is small enough that
    // a change of layout would simply get a new key.
    size_t len = sizeof(*frc);
    bool ok = nvs_get_blob(h, KEY_FRC, frc, &len) == ESP_OK && len == sizeof(*frc);
    nvs_close(h);
    return ok;
}

esp_err_t app_config_save_frc(const app_frc_t *frc)
{
    nvs_handle_t h;

    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    if (frc == NULL) {
        err = nvs_erase_key(h, KEY_FRC);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        err = nvs_set_blob(h, KEY_FRC, frc, sizeof(*frc));
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}
