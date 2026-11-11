#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "web_config.h"
#include "web_private.h"

#include "app_config.h"
#include "co2_net.h"
#include "fw_info.h"
#include "scd30_cfg.h"

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "web";

// The page is embedded in the binary rather than living in a filesystem
// partition: it is a single file, it has to match the firmware that serves it,
// and this way an OTA update can never leave the two out of step.
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

#define SCAN_MAX      20
// The form is small except for the TLS material: a CA, a client certificate and
// its key, each up to APP_CONFIG_PEM_MAX. Anything larger is not ours.
#define BODY_MAX    (3 * APP_CONFIG_PEM_MAX + 2048)

static httpd_handle_t s_server;
static bool           s_captive;

static void (*s_on_display)(uint8_t full, uint8_t dim, uint8_t lang, bool led);

// Written by the display loop, read by the HTTP task. A float pair and a
// timestamp cannot be copied atomically, hence the lock.
static portMUX_TYPE s_reading_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    uint16_t ppm;
    float    temperature;
    float    humidity;
    int64_t  at_us;      // 0 until the first reading
} s_reading;

// The last CAL_READINGS readings, for the CO2 calibration. Sensirion asks
// for at least two minutes of continuous measurement in stable air before a
// forced recalibration; at one reading every 2 s that is 60 of them. A gap in
// the readings starts the count again, since it means the sensor or the bus
// stopped for a while.
#define CAL_READINGS   60
#define CAL_GAP_US     (10 * 1000000LL)
#define CAL_SPREAD_MAX 40      // ppm between the lowest and highest reading

// The largest correction accepted. A sensor that has drifted further than
// this is rare; a reference taken in the wrong air is not, and it is the far
// more likely cause of a difference this large.
#define CAL_DELTA_MAX  150

// A wide calibration is the deliberate exception: a sensor that comes out of
// the box far off, set against outdoor air or a calibrated meter next to it.
// There is no limit on the correction then, only the sensor's own range, and
// the readings may vary more, since a 2-minute average of noisy readings is
// still a sound figure next to a correction of hundreds of ppm.
#define CAL_SPREAD_WIDE_MAX 150

// Protected by s_reading_lock, like the reading itself.
static struct {
    uint16_t ppm[CAL_READINGS];
    uint16_t n;          // how many of them hold a reading, up to CAL_READINGS
    uint16_t next;       // where the next one goes
    int64_t  since_us;   // start of the current unbroken run
} s_cal;

static void cal_reset_locked(void)
{
    s_cal.n    = 0;
    s_cal.next = 0;
}

typedef struct {
    bool     ready;      // a full window, and steady enough
    bool     ready_wide; // a full window, steady enough for a wide calibration
    uint32_t run_s;      // how long the readings have been coming unbroken
    uint16_t spread;
    uint16_t avg;
} cal_stats_t;

static cal_stats_t cal_stats(void)
{
    cal_stats_t st = { 0 };
    int64_t now = esp_timer_get_time();

    portENTER_CRITICAL(&s_reading_lock);
    uint32_t sum = 0;
    uint16_t lo = UINT16_MAX, hi = 0;
    uint16_t n = s_cal.n;
    for (uint16_t i = 0; i < n; i++) {
        uint16_t v = s_cal.ppm[i];
        sum += v;
        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
    }
    int64_t since = s_cal.since_us;
    portEXIT_CRITICAL(&s_reading_lock);

    if (n == 0) {
        return st;
    }
    st.run_s  = (uint32_t)((now - since) / 1000000);
    st.spread = hi - lo;
    st.avg    = (uint16_t)((sum + n / 2) / n);
    st.ready      = (n == CAL_READINGS) && st.spread <= CAL_SPREAD_MAX;
    st.ready_wide = (n == CAL_READINGS) && st.spread <= CAL_SPREAD_WIDE_MAX;
    return st;
}

void web_config_set_reading(uint16_t ppm, float temperature, float humidity)
{
    int64_t now = esp_timer_get_time();

    portENTER_CRITICAL(&s_reading_lock);
    if (s_cal.n == 0 || now - s_reading.at_us > CAL_GAP_US) {
        cal_reset_locked();
        s_cal.since_us = now;
    }
    s_cal.ppm[s_cal.next] = ppm;
    s_cal.next = (s_cal.next + 1) % CAL_READINGS;
    if (s_cal.n < CAL_READINGS) {
        s_cal.n++;
    }

    s_reading.ppm         = ppm;
    s_reading.temperature = temperature;
    s_reading.humidity    = humidity;
    s_reading.at_us       = now;
    portEXIT_CRITICAL(&s_reading_lock);
}

static volatile bool s_sensor_ok = true;

void web_config_set_sensor_ok(bool ok)
{
    s_sensor_ok = ok;
    if (!ok) {
        portENTER_CRITICAL(&s_reading_lock);
        cal_reset_locked();
        portEXIT_CRITICAL(&s_reading_lock);
    }
}

void web_config_on_display(void (*fn)(uint8_t full, uint8_t dim, uint8_t lang, bool led))
{
    s_on_display = fn;
}

//==============================================================================
// Request guard
//
// There is no login: anyone on the network can use the page, which is the
// intended trade-off for a meter at home. What must not work is a web page on
// the internet doing it through a visitor's browser:
//
//   cross-site requests  A form or a "simple" fetch from another site can POST
//                        here without the browser asking first. A custom
//                        header rules that out: it makes the browser send a
//                        CORS preflight, which this server never answers.
//   DNS rebinding        A site can point its own name at this device's
//                        address and then read and write the API as same-
//                        origin. Its requests still carry its own name in
//                        Host, so only addresses and this device's names are
//                        served.
//==============================================================================

// "192.168.1.20", "co2-monitor-a4cf12", "co2-monitor-a4cf12.local" or the
// same name under whatever suffix the router's DNS adds.
static bool host_is_ours(const char *host)
{
    const char *name = co2_net_hostname();
    size_t hlen = strcspn(host, ":");   // the port, if any, does not matter
    size_t nlen = strlen(name);

    bool literal = hlen > 0;
    for (size_t i = 0; i < hlen; i++) {
        if (!(host[i] == '.' || (host[i] >= '0' && host[i] <= '9'))) {
            literal = false;
            break;
        }
    }
    if (literal) {
        return true;
    }
    return nlen > 0 && hlen >= nlen && strncasecmp(host, name, nlen) == 0
        && (hlen == nlen || host[nlen] == '.');
}

bool web_request_allowed(httpd_req_t *req, bool changes_state)
{
    char host[96];

    // No Host header at all is a hand-written request, not a browser, and a
    // browser is the only thing rebinding can abuse.
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK
        && !host_is_ours(host)) {
        ESP_LOGW(TAG, "refused request for host \"%s\"", host);
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "unknown host");
        return false;
    }
    if (changes_state && httpd_req_get_hdr_value_len(req, WEB_GUARD_HEADER) == 0) {
        ESP_LOGW(TAG, "refused %s without " WEB_GUARD_HEADER, req->uri);
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "missing " WEB_GUARD_HEADER " header");
        return false;
    }
    return true;
}

//==============================================================================
// Handlers
//==============================================================================

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start - 1);
}

// The last calibration, as the page shows it and needs it to offer an undo.
static void add_frc(cJSON *root, const app_frc_t *frc)
{
    cJSON *o = cJSON_AddObjectToObject(root, "frc");
    if (o != NULL) {
        cJSON_AddNumberToObject(o, "before", frc->before_ppm);
        cJSON_AddNumberToObject(o, "reference", frc->ref_ppm);
        cJSON_AddNumberToObject(o, "at", frc->at);
    }
}

// Passwords and the client key are never sent back. The page shows a
// placeholder and only submits one when the user actually types it, so a
// stored secret cannot be read off the form by whoever reaches the page.
static esp_err_t config_get(httpd_req_t *req)
{
    if (!web_request_allowed(req, false)) {
        return ESP_FAIL;
    }

    app_config_t cfg;
    app_config_load(&cfg);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }

    cJSON_AddBoolToObject(root,   "wifi_enabled", cfg.wifi_enabled);
    cJSON_AddStringToObject(root, "wifi_ssid",   cfg.wifi_ssid);
    cJSON_AddBoolToObject(root,   "wifi_pass_set", cfg.wifi_pass[0] != '\0');
    cJSON_AddStringToObject(root, "mqtt_uri",    cfg.mqtt_uri);
    cJSON_AddStringToObject(root, "mqtt_user",   cfg.mqtt_user);
    cJSON_AddBoolToObject(root,   "mqtt_pass_set", cfg.mqtt_pass[0] != '\0');
    cJSON_AddBoolToObject(root,   "ha_discovery", cfg.ha_discovery);
    cJSON_AddStringToObject(root, "mqtt_topic",  cfg.mqtt_topic);
    cJSON_AddStringToObject(root, "mqtt_topic_default", co2_net_default_topic_base());
    cJSON_AddBoolToObject(root,   "mqtt_retain", cfg.mqtt_retain);
    cJSON_AddStringToObject(root, "fw_version",  fw_info_version());
    cJSON_AddStringToObject(root, "hw_revision", fw_info_hw_revision());
    cJSON_AddStringToObject(root, "sensor_fw",   fw_info_sensor());

    // Unlike the passwords, certificates are public by definition: they are
    // handed to the other end in every handshake. Sending them back lets the
    // page show which ones are stored. The key is the secret half.
    char *pem = app_config_load_pem(APP_PEM_CA);
    cJSON_AddStringToObject(root, "mqtt_ca", pem != NULL ? pem : "");
    free(pem);

    pem = app_config_load_pem(APP_PEM_CLIENT_CERT);
    cJSON_AddStringToObject(root, "mqtt_client_cert", pem != NULL ? pem : "");
    free(pem);

    pem = app_config_load_pem(APP_PEM_CLIENT_KEY);
    cJSON_AddBoolToObject(root, "mqtt_client_key_set", pem != NULL);
    free(pem);

    cJSON_AddStringToObject(root, "ha_prefix",   cfg.ha_prefix);
    cJSON_AddNumberToObject(root, "backlight_full", cfg.backlight_full);
    cJSON_AddNumberToObject(root, "backlight_dim",  cfg.backlight_dim);
    cJSON_AddNumberToObject(root, "chart_bars",     cfg.chart_bars);
    cJSON_AddNumberToObject(root, "chart_bar_s",    cfg.chart_bar_s);
    cJSON_AddStringToObject(root, "language",       app_config_lang_code(cfg.language));
    cJSON_AddBoolToObject(root,   "led_enabled",    cfg.led_enabled);

    cJSON_AddStringToObject(root, "device_name", cfg.device_name);
    cJSON_AddNumberToObject(root, "altitude_m",  cfg.altitude_m);
    cJSON_AddNumberToObject(root, "temp_offset_c100", cfg.temp_offset_c100);

    // What the sensor itself reports, which is the only proof the write stuck.
    // It can differ from the values above if a write failed.
    uint16_t sensor_alt = 0, sensor_off = 0;
    scd30_cfg_applied(&sensor_alt, &sensor_off);
    cJSON_AddNumberToObject(root, "sensor_altitude_m",       sensor_alt);
    cJSON_AddNumberToObject(root, "sensor_temp_offset_c100", sensor_off);
    cJSON_AddBoolToObject(root,   "sensor_asc",              scd30_cfg_asc_active());

    app_frc_t frc;
    if (app_config_load_frc(&frc)) {
        add_frc(root, &frc);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == NULL) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, out);
    free(out);
    return err;
}

// Polled by the page every few seconds, so it only reads what is already in
// memory: no NVS, no radio.
static esp_err_t status_get(httpd_req_t *req)
{
    if (!web_request_allowed(req, false)) {
        return ESP_FAIL;
    }

    portENTER_CRITICAL(&s_reading_lock);
    typeof(s_reading) r = s_reading;
    portEXIT_CRITICAL(&s_reading_lock);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }

    int64_t now = esp_timer_get_time();
    if (r.at_us != 0 && r.ppm != 0) {
        cJSON_AddNumberToObject(root, "co2", r.ppm);
        // Rounded the same way as the MQTT topics, so the two agree.
        // In double: a float rounded to 23.4 would print as 23.399999618530273.
        cJSON_AddNumberToObject(root, "temperature", round(r.temperature * 10.0) / 10.0);
        cJSON_AddNumberToObject(root, "humidity",    round(r.humidity));
        cJSON_AddNumberToObject(root, "reading_age_s", (double)((now - r.at_us) / 1000000));
    }

    char ip[16];
    int  rssi = 0;
    if (co2_net_link_info(ip, sizeof(ip), &rssi)) {
        cJSON_AddStringToObject(root, "ip", ip);
        cJSON_AddNumberToObject(root, "rssi", rssi);
        cJSON_AddNumberToObject(root, "tx_dbm", co2_net_tx_power_dbm());
    }
    cJSON_AddBoolToObject(root, "sensor_ok", s_sensor_ok);
    cJSON_AddBoolToObject(root, "setup_mode", s_captive);
    cJSON_AddBoolToObject(root, "mqtt_connected", co2_net_is_connected());
    cJSON_AddStringToObject(root, "hostname", co2_net_hostname());
    cJSON_AddNumberToObject(root, "uptime_s", (double)(now / 1000000));

    cal_stats_t cal = cal_stats();
    cJSON *c = cJSON_AddObjectToObject(root, "cal");
    if (c != NULL) {
        cJSON_AddBoolToObject(c,   "ready",  cal.ready);
        cJSON_AddNumberToObject(c, "run_s",  cal.run_s);
        cJSON_AddNumberToObject(c, "spread", cal.spread);
        cJSON_AddNumberToObject(c, "avg",    cal.avg);
        cJSON_AddNumberToObject(c, "spread_max", CAL_SPREAD_MAX);
        cJSON_AddNumberToObject(c, "delta_max",  CAL_DELTA_MAX);
        cJSON_AddBoolToObject(c,   "ready_wide", cal.ready_wide);
        cJSON_AddNumberToObject(c, "spread_wide_max", CAL_SPREAD_WIDE_MAX);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == NULL) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, out);
    free(out);
    return err;
}

// Copies a JSON string field into dst, leaving dst alone when the field is
// absent. That is what lets the page omit a password it does not want changed.
static void take_str(const cJSON *root, const char *key, char *dst, size_t len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);

    if (cJSON_IsString(item) && item->valuestring != NULL) {
        snprintf(dst, len, "%s", item->valuestring);
    }
}

// A number field that is present must be one of the accepted values, or the
// whole request is refused: silently clamping a brightness or a bar count
// would store something the user never chose.
static bool number_ok(const cJSON *root, const char *key, bool (*ok)(uint16_t))
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);

    if (item == NULL) {
        return true;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > UINT16_MAX
        || item->valuedouble != (double)(uint16_t)item->valuedouble) {
        return false;
    }
    return ok((uint16_t)item->valuedouble);
}

static bool bl_full_ok(uint16_t v)  { return v >= APP_BACKLIGHT_FULL_MIN && v <= 100; }
static bool bl_dim_ok(uint16_t v)   { return v <= 100; }
static bool altitude_ok(uint16_t v) { return v <= APP_ALTITUDE_MAX; }
static bool any_u16_ok(uint16_t v)  { (void)v; return true; }

// Longest string each field holds, without the terminator.
#define FIELD_MAX(member) (sizeof(((app_config_t *)0)->member) - 1)

// Empty for an open network, 8 to 63 characters for a passphrase, or exactly
// 64 hex digits for a raw key: the only forms WPA2 accepts, and anything else
// makes esp_wifi_set_config() fail at boot.
static bool wifi_pass_ok(const char *p)
{
    size_t n = strlen(p);

    if (n == 0 || (n >= 8 && n <= 63)) {
        return true;
    }
    if (n != 64) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

// "mqtts://user@Broker.example:8883/x" gives "Broker.example". Compared
// without case, since host names have none.
static bool same_broker_host(const char *a, const char *b)
{
    const char *ha[2] = { a, b };
    size_t      len[2];

    for (int i = 0; i < 2; i++) {
        const char *s = strstr(ha[i], "://");
        s = (s != NULL) ? s + 3 : ha[i];
        const char *at = strchr(s, '@');
        if (at != NULL && at < s + strcspn(s, "/")) {
            s = at + 1;
        }
        ha[i]  = s;
        len[i] = strcspn(s, ":/");
    }
    return len[0] == len[1] && strncasecmp(ha[0], ha[1], len[0]) == 0;
}

// Whether saving this PEM would change what is stored. The page sends the
// certificates back on every save, so most saves carry them unchanged.
static bool pem_differs(app_pem_t which, const char *pem)
{
    char *stored = app_config_load_pem(which);
    bool  differs = strcmp(stored != NULL ? stored : "", pem != NULL ? pem : "") != 0;
    free(stored);
    return differs;
}

// Everything except the brightness, the screen language and the LED is read
// once at boot, so any other change needs a reboot to take effect. Compared
// field by field rather than with memcmp, which would also compare the bytes
// after each string terminator.
static bool needs_restart(const app_config_t *a, const app_config_t *b)
{
    return strcmp(a->wifi_ssid, b->wifi_ssid) != 0
        || strcmp(a->wifi_pass, b->wifi_pass) != 0
        || a->wifi_enabled != b->wifi_enabled
        || strcmp(a->mqtt_uri, b->mqtt_uri) != 0
        || strcmp(a->mqtt_user, b->mqtt_user) != 0
        || strcmp(a->mqtt_pass, b->mqtt_pass) != 0
        || a->ha_discovery != b->ha_discovery
        || strcmp(a->ha_prefix, b->ha_prefix) != 0
        || strcmp(a->mqtt_topic, b->mqtt_topic) != 0
        || a->mqtt_retain != b->mqtt_retain
        || strcmp(a->device_name, b->device_name) != 0
        || a->altitude_m != b->altitude_m
        || a->temp_offset_c100 != b->temp_offset_c100
        || a->chart_bars != b->chart_bars
        || a->chart_bar_s != b->chart_bar_s;
}

static void take_u16(const cJSON *root, const char *key, uint16_t *dst)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);

    if (cJSON_IsNumber(item)) {
        double v = item->valuedouble;
        if (v < 0) {
            v = 0;
        }
        if (v > UINT16_MAX) {
            v = UINT16_MAX;
        }
        *dst = (uint16_t)v;
    }
}

static void take_u8(const cJSON *root, const char *key, uint8_t *dst)
{
    uint16_t v = *dst;
    take_u16(root, key, &v);
    *dst = v > UINT8_MAX ? UINT8_MAX : (uint8_t)v;
}

// Answers 400 and returns false when a field that is present cannot be used.
// Checked before anything is written, so a bad value never half-applies.
static bool fields_valid(httpd_req_t *req, const cJSON *root)
{
    // Refused rather than cut short: a truncated password or broker address
    // fails later, at connect time, with nothing to say why.
    static const struct {
        const char *field;
        size_t      max;
    } lengths[] = {
        { "wifi_ssid",   FIELD_MAX(wifi_ssid) },
        { "wifi_pass",   FIELD_MAX(wifi_pass) },
        { "mqtt_uri",    FIELD_MAX(mqtt_uri) },
        { "mqtt_user",   FIELD_MAX(mqtt_user) },
        { "mqtt_pass",   FIELD_MAX(mqtt_pass) },
        { "device_name", FIELD_MAX(device_name) },
        { "mqtt_topic",  FIELD_MAX(mqtt_topic) },
        { "ha_prefix",   FIELD_MAX(ha_prefix) },
    };
    char msg[64];

    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, lengths[i].field);
        if (item == NULL) {
            continue;
        }
        if (!cJSON_IsString(item)) {
            snprintf(msg, sizeof(msg), "%s: must be a string", lengths[i].field);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
            return false;
        }
        if (strlen(item->valuestring) > lengths[i].max) {
            snprintf(msg, sizeof(msg), "%s: at most %u characters",
                     lengths[i].field, (unsigned)lengths[i].max);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
            return false;
        }
    }

    // Shown in Home Assistant and in network browsers; a line break or a tab
    // in it is never intended.
    const cJSON *dname = cJSON_GetObjectItemCaseSensitive(root, "device_name");
    if (cJSON_IsString(dname)) {
        for (const char *c = dname->valuestring; *c != '\0'; c++) {
            if ((unsigned char)*c < 0x20 || *c == 0x7F) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "name: no control characters");
                return false;
            }
        }
    }

    const cJSON *wpass = cJSON_GetObjectItemCaseSensitive(root, "wifi_pass");
    if (cJSON_IsString(wpass) && !wifi_pass_ok(wpass->valuestring)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Wi-Fi password: 8 to 63 characters, or 64 hex digits");
        return false;
    }

    // The offset is added to what is stored, so it only has to be a sane
    // number here; the sum is clamped when applied. Checked before the cast
    // to long, which would be undefined for a huge or infinite value.
    const cJSON *delta = cJSON_GetObjectItemCaseSensitive(root, "temp_offset_delta_c100");
    if (delta != NULL && !(cJSON_IsNumber(delta) && isfinite(delta->valuedouble)
                           && fabs(delta->valuedouble) <= 10000)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "temperature correction: at most 100 degrees either way");
        return false;
    }

    const cJSON *topic = cJSON_GetObjectItemCaseSensitive(root, "mqtt_topic");
    if (cJSON_IsString(topic) && topic->valuestring[0] != '\0'
        && !co2_net_topic_base_valid(topic->valuestring)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "topic base: no + or #, no empty levels, under 64 characters");
        return false;
    }

    const cJSON *hap = cJSON_GetObjectItemCaseSensitive(root, "ha_prefix");
    if (cJSON_IsString(hap) && hap->valuestring[0] != '\0'
        && (strlen(hap->valuestring) >= sizeof(((app_config_t *)0)->ha_prefix)
            || !co2_net_topic_base_valid(hap->valuestring))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "discovery prefix: no + or #, no empty levels, under 32 characters");
        return false;
    }

    const cJSON *lang = cJSON_GetObjectItemCaseSensitive(root, "language");
    if (lang != NULL && !(cJSON_IsString(lang)
                          && app_config_lang_from_code(lang->valuestring) < APP_LANG_COUNT)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "language: en or es");
        return false;
    }

    static const struct {
        const char *field;
        bool      (*ok)(uint16_t);
        const char *what;
    } numbers[] = {
        { "backlight_full", bl_full_ok,                   "brightness: 5 to 100" },
        { "backlight_dim",  bl_dim_ok,                    "dimmed brightness: 0 to 100" },
        { "chart_bars",     app_config_chart_bars_valid,  "chart bars: not an accepted count" },
        { "chart_bar_s",    app_config_chart_bar_s_valid, "bar duration: not an accepted value" },
        { "altitude_m",     altitude_ok,                  "altitude: 0 to 9000 m" },
        { "temp_offset_c100", any_u16_ok,                 "temperature offset: 0 to 65535" },
    };
    for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
        if (!number_ok(root, numbers[i].field, numbers[i].ok)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, numbers[i].what);
            return false;
        }
    }

    static const struct {
        const char *field;
        const char *marker;   // what a PEM of the right kind contains
        const char *what;
    } pems[] = {
        { "mqtt_ca",          "-----BEGIN CERTIFICATE-----", "CA certificate" },
        { "mqtt_client_cert", "-----BEGIN CERTIFICATE-----", "client certificate" },
        { "mqtt_client_key",  "PRIVATE KEY-----",            "client key" },
    };
    char err[64];

    for (size_t i = 0; i < sizeof(pems) / sizeof(pems[0]); i++) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, pems[i].field);
        if (!cJSON_IsString(item) || item->valuestring[0] == '\0') {
            continue;
        }
        if (strlen(item->valuestring) >= APP_CONFIG_PEM_MAX) {
            snprintf(err, sizeof(err), "%s too long", pems[i].what);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
            return false;
        }
        if (strstr(item->valuestring, pems[i].marker) == NULL) {
            snprintf(err, sizeof(err), "%s must be PEM", pems[i].what);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
            return false;
        }
    }

    // There is nowhere to type a passphrase, and asking for one at every boot
    // would defeat an unattended device.
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(root, "mqtt_client_key");
    if (cJSON_IsString(key) && strstr(key->valuestring, "ENCRYPTED") != NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "client key must not be encrypted");
        return false;
    }

    // A certificate needs its key. The key may already be stored, since the
    // page only sends it when retyped.
    const cJSON *cert = cJSON_GetObjectItemCaseSensitive(root, "mqtt_client_cert");
    if (cJSON_IsString(cert) && cert->valuestring[0] != '\0'
        && !(cJSON_IsString(key) && key->valuestring[0] != '\0')) {
        char *stored = app_config_load_pem(APP_PEM_CLIENT_KEY);
        bool  have   = (stored != NULL);
        free(stored);
        if (!have) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "client certificate needs its private key");
            return false;
        }
    }
    return true;
}

static esp_err_t config_post(httpd_req_t *req)
{
    if (!web_request_allowed(req, true)) {
        return ESP_FAIL;
    }

    if (req->content_len >= BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
        return ESP_FAIL;
    }

    // On the heap: the handler task's stack is a few kilobytes and the body
    // can come close to that with a certificate in it.
    char *body = malloc(req->content_len + 1);
    if (body == NULL) {
        return httpd_resp_send_500(req);
    }

    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            free(body);
            return ESP_FAIL;
        }
        got += n;
    }
    body[got] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "malformed JSON");
        return ESP_FAIL;
    }

    if (!fields_valid(req, root)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    // Loaded first so that any field the page leaves out keeps its value.
    app_config_t cfg;
    app_config_load(&cfg);
    app_config_t before = cfg;

    take_str(root, "wifi_ssid",   cfg.wifi_ssid,   sizeof(cfg.wifi_ssid));
    take_str(root, "wifi_pass",   cfg.wifi_pass,   sizeof(cfg.wifi_pass));
    take_str(root, "mqtt_uri",    cfg.mqtt_uri,    sizeof(cfg.mqtt_uri));
    take_str(root, "mqtt_user",   cfg.mqtt_user,   sizeof(cfg.mqtt_user));
    take_str(root, "mqtt_pass",   cfg.mqtt_pass,   sizeof(cfg.mqtt_pass));
    take_str(root, "device_name", cfg.device_name, sizeof(cfg.device_name));
    take_str(root, "mqtt_topic",  cfg.mqtt_topic,  sizeof(cfg.mqtt_topic));
    take_u16(root, "altitude_m",       &cfg.altitude_m);
    take_u16(root, "temp_offset_c100", &cfg.temp_offset_c100);
    take_str(root, "ha_prefix",   cfg.ha_prefix,   sizeof(cfg.ha_prefix));
    take_u8(root,  "backlight_full", &cfg.backlight_full);
    take_u8(root,  "backlight_dim",  &cfg.backlight_dim);
    take_u16(root, "chart_bars",     &cfg.chart_bars);
    take_u16(root, "chart_bar_s",    &cfg.chart_bar_s);

    const cJSON *lang = cJSON_GetObjectItemCaseSensitive(root, "language");
    if (cJSON_IsString(lang)) {
        cfg.language = app_config_lang_from_code(lang->valuestring);   // checked above
    }

    // Empty means the default, which is spelled out rather than stored blank
    // so the page always shows what is in use.
    if (cfg.ha_prefix[0] == '\0') {
        snprintf(cfg.ha_prefix, sizeof(cfg.ha_prefix), "homeassistant");
    }

    // The offset is cumulative: what you measure against a reference is the
    // residual error of an already corrected reading, so it has to be added to
    // what is stored, never written in its place. Doing the sum here is what
    // makes the classic double-correction mistake impossible from the page.
    const cJSON *delta = cJSON_GetObjectItemCaseSensitive(root, "temp_offset_delta_c100");
    if (cJSON_IsNumber(delta)) {
        long v = (long)cfg.temp_offset_c100 + (long)delta->valuedouble;

        // The sensor only knows how to read high, so a negative total means the
        // correction overshot and the right answer is none at all.
        if (v < 0) {
            v = 0;
        }
        if (v > UINT16_MAX) {
            v = UINT16_MAX;
        }
        cfg.temp_offset_c100 = (uint16_t)v;
    }

    const cJSON *disc = cJSON_GetObjectItemCaseSensitive(root, "ha_discovery");
    if (cJSON_IsBool(disc)) {
        cfg.ha_discovery = cJSON_IsTrue(disc);
    }

    const cJSON *wen = cJSON_GetObjectItemCaseSensitive(root, "wifi_enabled");
    if (cJSON_IsBool(wen)) {
        cfg.wifi_enabled = cJSON_IsTrue(wen);
    }

    const cJSON *ret = cJSON_GetObjectItemCaseSensitive(root, "mqtt_retain");
    if (cJSON_IsBool(ret)) {
        cfg.mqtt_retain = cJSON_IsTrue(ret);
    }

    const cJSON *led = cJSON_GetObjectItemCaseSensitive(root, "led_enabled");
    if (cJSON_IsBool(led)) {
        cfg.led_enabled = cJSON_IsTrue(led);
    }

    // The stored password belongs to the broker it was typed for. Pointing
    // the device at another host without typing a new one would hand the old
    // one to whoever runs that host, which is also the easiest thing for
    // anybody on the network to do to a device without a login.
    if (!same_broker_host(before.mqtt_uri, cfg.mqtt_uri)
        && !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "mqtt_pass"))
        && cfg.mqtt_pass[0] != '\0') {
        ESP_LOGW(TAG, "broker host changed, stored MQTT password dropped");
        cfg.mqtt_pass[0] = '\0';
    }

    // Blank means "keep it" for a password, so clearing one needs its own
    // flag. It matters when moving to a broker that authenticates by
    // certificate and would be handed a stale password otherwise.
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "mqtt_pass_clear"))) {
        cfg.mqtt_pass[0] = '\0';
    }

    esp_err_t err = ESP_OK;
    bool pem_changed = false;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "mqtt_ca");
    if (cJSON_IsString(item) && pem_differs(APP_PEM_CA, item->valuestring)) {
        pem_changed = true;
        err = app_config_save_pem(APP_PEM_CA, item->valuestring);
    }

    // Emptying the certificate drops the key with it: a key without its
    // certificate is of no use and is the one thing worth not leaving behind.
    bool cert_cleared = false;
    item = cJSON_GetObjectItemCaseSensitive(root, "mqtt_client_cert");
    if (err == ESP_OK && cJSON_IsString(item)
        && pem_differs(APP_PEM_CLIENT_CERT, item->valuestring)) {
        pem_changed = true;
        err = app_config_save_pem(APP_PEM_CLIENT_CERT, item->valuestring);
        cert_cleared = (item->valuestring[0] == '\0');
        if (err == ESP_OK && cert_cleared) {
            err = app_config_save_pem(APP_PEM_CLIENT_KEY, NULL);
        }
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "mqtt_client_key");
    if (err == ESP_OK && !cert_cleared
        && cJSON_IsString(item) && item->valuestring[0] != '\0'
        && pem_differs(APP_PEM_CLIENT_KEY, item->valuestring)) {
        pem_changed = true;
        err = app_config_save_pem(APP_PEM_CLIENT_KEY, item->valuestring);
    }

    cJSON_Delete(root);

    if (err == ESP_OK) {
        err = app_config_save(&cfg);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot save");
        return ESP_FAIL;
    }

    if (s_on_display != NULL) {
        s_on_display(cfg.backlight_full, cfg.backlight_dim, cfg.language, cfg.led_enabled);
    }

    // The page reboots the device only when told to, so a brightness change
    // does not cost the chart its history.
    // In setup mode the device runs its own access point until it restarts,
    // whatever changed: without the restart it would never rejoin the network.
    bool restart = s_captive || pem_changed || needs_restart(&before, &cfg);
    ESP_LOGI(TAG, "settings saved%s, stack: %u B never used",
             restart ? ", restart needed" : "",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, restart ? "{\"ok\":true,\"restart\":true}"
                                           : "{\"ok\":true,\"restart\":false}");
}

static esp_err_t scan_get(httpd_req_t *req)
{
    if (!web_request_allowed(req, false)) {
        return ESP_FAIL;
    }

    co2_net_scan_result_t *found = calloc(SCAN_MAX, sizeof(*found));
    if (found == NULL) {
        return httpd_resp_send_500(req);
    }

    int n = co2_net_scan(found, SCAN_MAX);
    if (n < 0) {
        n = 0;   // an empty list is a better answer than an error page here
    }

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; arr != NULL && i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        if (o == NULL) {
            break;
        }
        cJSON_AddStringToObject(o, "ssid", found[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", found[i].rssi);
        cJSON_AddBoolToObject(o,   "secure", found[i].secure);
        cJSON_AddItemToArray(arr, o);
    }
    free(found);

    if (arr == NULL) {
        return httpd_resp_send_500(req);
    }

    char *out = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (out == NULL) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, out);
    free(out);
    return err;
}

// Long enough for the response to reach the browser before the link drops.
#define RESTART_DELAY_MS 1200

// Every restart the page asks for goes through here: after saving, after an
// update, or on its own. The goodbye compares what runs with what is stored,
// which is what the next boot will use, so a save that moved the topics gets
// the old ones cleared and anything else just says "offline".
static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));

    // Static: this task has a small stack and the settings take a good part.
    static app_config_t next;
    if (app_config_load(&next) == ESP_OK) {
        co2_net_config_t net = {
            .topic_base   = next.mqtt_topic,
            .retain       = next.mqtt_retain,
            .ha_discovery = next.ha_discovery,
            .ha_prefix    = next.ha_prefix,
        };
        co2_net_goodbye(&net);
    } else {
        co2_net_goodbye(NULL);
    }

    ESP_LOGI(TAG, "restarting, stack: %u B never used",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    esp_restart();
}

void web_restart_soon(void)
{
    // Without the task there is no way to delay the restart; doing it at once
    // still beats promising a restart that never comes.
    if (xTaskCreate(restart_task, "restart", 3072, NULL, 5, NULL) != pdPASS) {
        esp_restart();
    }
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    if (!web_request_allowed(req, true)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, "{\"ok\":true}");

    web_restart_soon();
    return err;
}

//==============================================================================
// CO2 calibration
//==============================================================================

// Refusals carry a code rather than a sentence, so the page can say it in the
// viewer's language. The page checks the same conditions first; these are
// for a request that arrives anyway, or a state that changed in between.
static esp_err_t frc_refuse(httpd_req_t *req, const char *status, const char *code)
{
    char out[48];

    snprintf(out, sizeof(out), "{\"error\":\"%s\"}", code);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    return ESP_FAIL;
}

// {"reference": 430, "at": <Unix time>} calibrates to that reference.
// {"undo": true, "at": ...} applies the opposite of the last recorded
// correction to the air as it is now, which works in any stable air: the
// reference then follows from the current reading, not from the air.
// {"reference": 430, "wide": true, ...} lifts the limit on the correction,
// for a sensor that is far off from the start; see CAL_SPREAD_WIDE_MAX.
static esp_err_t frc_post(httpd_req_t *req)
{
    if (!web_request_allowed(req, true)) {
        return ESP_FAIL;
    }

    char body[128];
    if (req->content_len >= sizeof(body)) {
        return frc_refuse(req, "400 Bad Request", "malformed");
    }
    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            return ESP_FAIL;
        }
        got += n;
    }
    body[got] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return frc_refuse(req, "400 Bad Request", "malformed");
    }
    bool         undo = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "undo"));
    bool         wide = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "wide"));
    const cJSON *ref  = cJSON_GetObjectItemCaseSensitive(root, "reference");
    const cJSON *at   = cJSON_GetObjectItemCaseSensitive(root, "at");
    double ref_v = cJSON_IsNumber(ref) ? ref->valuedouble : -1;
    double at_v  = cJSON_IsNumber(at)  ? at->valuedouble  : 0;
    cJSON_Delete(root);

    if (!s_sensor_ok) {
        return frc_refuse(req, "409 Conflict", "sensor");
    }

    // The pressure compensation is part of every reading, so a calibration
    // made under one altitude is off under another. The altitude reaches the
    // sensor at boot: a new one saved since then is not in effect yet.
    app_config_t cfg;
    app_config_load(&cfg);
    uint16_t sensor_alt = 0;
    scd30_cfg_applied(&sensor_alt, NULL);
    if (sensor_alt != cfg.altitude_m) {
        return frc_refuse(req, "409 Conflict", "altitude");
    }

    // An undo always asks for the normal steadiness: it is the way back
    // from a mistake, and a noisy average would make a second one.
    if (undo) {
        wide = false;
    }

    cal_stats_t st = cal_stats();
    if (!(wide ? st.ready_wide : st.ready)) {
        return frc_refuse(req, "409 Conflict", "unstable");
    }

    app_frc_t last;
    long target;
    if (undo) {
        if (!app_config_load_frc(&last)) {
            return frc_refuse(req, "409 Conflict", "nothing_to_undo");
        }
        target = (long)st.avg - ((long)last.ref_ppm - (long)last.before_ppm);
    } else {
        if (ref_v != (double)(long)ref_v) {
            return frc_refuse(req, "400 Bad Request", "range");
        }
        target = (long)ref_v;
        if (!wide && labs(target - (long)st.avg) > CAL_DELTA_MAX) {
            return frc_refuse(req, "400 Bad Request", "too_far");
        }
    }
    // The range the sensor accepts.
    if (target < 400 || target > 2000) {
        return frc_refuse(req, "400 Bad Request", "range");
    }

    ESP_LOGI(TAG, "%s: sensor reads %u ppm (spread %u), reference %ld ppm",
             undo ? "undoing the last calibration"
                  : wide ? "calibrating (wide)" : "calibrating",
             st.avg, st.spread, target);

    esp_err_t err = scd30_cfg_frc((uint16_t)target);
    if (err == ESP_ERR_TIMEOUT) {
        return frc_refuse(req, "504 Gateway Timeout", "timeout");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return frc_refuse(req, "409 Conflict", "busy");
    }
    if (err != ESP_OK) {
        return frc_refuse(req, "500 Internal Server Error", "refused");
    }

    // The readings from before no longer describe the sensor as it is, so the
    // wait for stable air starts again before a second calibration.
    portENTER_CRITICAL(&s_reading_lock);
    cal_reset_locked();
    portEXIT_CRITICAL(&s_reading_lock);

    app_frc_t rec = {
        .before_ppm = st.avg,
        .ref_ppm    = (uint16_t)target,
        .at         = (at_v > 0 && at_v < 4294967296.0) ? (uint32_t)at_v : 0,
    };
    // Applied in the sensor even if this fails, and said so: the record only
    // serves the undo, and a failed write here must not look like a failed
    // calibration.
    err = app_config_save_frc(undo ? NULL : &rec);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "calibration applied but not recorded: %s", esp_err_to_name(err));
    }

    cJSON *out = cJSON_CreateObject();
    if (out == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddBoolToObject(out, "recorded", err == ESP_OK);
    cJSON_AddNumberToObject(out, "before", st.avg);
    cJSON_AddNumberToObject(out, "reference", target);
    if (!undo && err == ESP_OK) {
        add_frc(out, &rec);
    }
    char *txt = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (txt == NULL) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t sent = httpd_resp_sendstr(req, txt);
    free(txt);
    return sent;
}

// Everything a phone probes for when it joins a network. Answering these with a
// redirect is what pops the setup page up on its own instead of leaving the
// user to discover an IP address.
static esp_err_t captive_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

//==============================================================================

esp_err_t web_config_start(bool captive)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 10;   // the routes below plus the captive catch-all
    // The default 4 KB is tight: saving keeps two app_config_t on the stack
    // while cJSON and NVS work on top. config_post() logs what is left.
    cfg.stack_size       = 6144;
    // The captive portal needs the catch-all below, which only matches with
    // wildcards enabled.
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = page_get },
        { .uri = "/api/config",  .method = HTTP_GET,  .handler = config_get },
        { .uri = "/api/config",  .method = HTTP_POST, .handler = config_post },
        { .uri = "/api/scan",    .method = HTTP_GET,  .handler = scan_get },
        { .uri = "/api/status",  .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/reboot",  .method = HTTP_POST, .handler = reboot_post },
        { .uri = "/api/ota",     .method = HTTP_POST, .handler = ota_post },
        { .uri = "/api/frc",     .method = HTTP_POST, .handler = frc_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t e = httpd_register_uri_handler(s_server, &routes[i]);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "route %s: %s", routes[i].uri, esp_err_to_name(e));
        }
    }

    s_captive = captive;
    if (captive) {
        // Registered last so the explicit routes above win: the matcher takes
        // the first handler that matches.
        static const httpd_uri_t catch_all = {
            .uri = "/*", .method = HTTP_GET, .handler = captive_redirect,
        };
        httpd_register_uri_handler(s_server, &catch_all);

        esp_netif_ip_info_t ip = { 0 };
        esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (ap != NULL) {
            esp_netif_get_ip_info(ap, &ip);
        }
        dns_server_start(ip.ip.addr);
    }

    ESP_LOGI(TAG, "web interface up%s", captive ? " (captive portal)" : "");
    return ESP_OK;
}

esp_err_t web_config_stop(void)
{
    if (s_captive) {
        dns_server_stop();
        s_captive = false;
    }
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    return ESP_OK;
}
