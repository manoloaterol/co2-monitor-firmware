#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net_private.h"

#include "fw_info.h"

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "mqtt_client.h"

static const char *TAG = "net";

// Station TX power cap, in 0.25 dBm units. A meter indoors near its access
// point does not need the full 20 dBm, and every transmission draws less at
// 15. One far from it does, so the cap follows the signal: up to full power
// when the access point is heard weakly, back down once it is heard well again.
// The gap between the two thresholds keeps it from flapping on a signal that
// hovers around one of them. They are the page's "fair" and "good" limits.
#define WIFI_TX_LOW_QDBM   60    // 15 dBm
#define WIFI_TX_HIGH_QDBM  80    // 20 dBm, the PHY's maximum
#define WIFI_TX_RAISE_RSSI (-70)
#define WIFI_TX_LOWER_RSSI (-60)
#define WIFI_TX_CHECK_US   (10LL * 1000000)

// Failed attempts in a row after which the next ones go out at full power: a
// network at the edge of range may never be joined at 15 dBm, and then there
// is no signal reading to go by.
#define WIFI_TX_RAISE_RETRIES 3

// How often <base>/info is refreshed. It is retained, so this only decides how
// stale the signal and uptime a new subscriber sees can get.
#define INFO_PERIOD_US (5LL * 60 * 1000000)

co2_net_config_t g_cfg;
char             g_device_id[32];
char             g_topic_base[TOPIC_BASE_MAX];
char             g_device_name[48];
char             g_ha_prefix[32];

// The strings in g_cfg point into these copies, so the caller may pass
// literals, stack buffers or values it later frees.
static char s_ssid[33];     // 32 bytes plus terminator, per the 802.11 limit
static char s_pass[65];
static char s_uri[128];
static char s_user[65];
static char s_mqtt_pass[65];
static char s_prefix[33];
static char s_topic[TOPIC_BASE_MAX];
static char s_name[48];
static char s_ha_prefix[32];

// Heap copies: each runs to a kilobyte or more, too much for static buffers
// that almost every board leaves empty.
static char *s_ca;
static char *s_cert;
static char *s_key;

// "co2monitor/a4cf12", kept even when the user chose another base, so the
// setup page can show what an empty field falls back to.
static char s_topic_default[TOPIC_BASE_MAX];

// "CO2-Monitor-a4cf12" is 18 characters; kept below the 32 of wifi_config_t so
// the copy into it can never truncate.
static char s_ap_ssid[24];

// "co2-monitor-a4cf12". Hyphens and lower case because it has to be a valid
// DNS label, which the MQTT identifier with its underscore is not.
static char s_hostname[32];

static void mdns_start(void);

static co2_net_state_t s_state = CO2_NET_OFF;
static bool            s_ap_mode;
static int             s_ap_clients;
static bool s_core_ready;

static esp_mqtt_client_handle_t s_client;
static volatile bool            s_mqtt_up;
static int64_t                  s_info_at;   // esp_timer time of the last info

// Set by co2_net_goodbye(): nothing new is queued once the device is on its
// way out, or a reading could land on a topic that was just cleared.
static volatile bool            s_leaving;

// Retries are unbounded on purpose: this is a fixed appliance on a home
// network, and giving up would need someone to power cycle it. They are spaced
// out instead: 1 s, doubling up to a minute. Reconnecting straight from the
// disconnect event turned a missing access point into a tight loop of scans
// and log lines, while a minute is still short next to a router that takes a
// couple of them to reboot.
#define WIFI_RETRY_FIRST_MS 1000
#define WIFI_RETRY_MAX_MS   60000

static int                s_retries;
static esp_timer_handle_t s_retry_timer;

static esp_timer_handle_t s_tx_timer;
static int8_t             s_tx_qdbm = WIFI_TX_LOW_QDBM;
static int                s_rssi_avg;   // smoothed, in dBm; 0 until the first sample

static void tx_power_set(int8_t qdbm, const char *why)
{
    if (qdbm == s_tx_qdbm) {
        return;
    }
    esp_err_t err = esp_wifi_set_max_tx_power(qdbm);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TX power: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "TX power %d -> %d dBm (%s)", s_tx_qdbm / 4, qdbm / 4, why);
    s_tx_qdbm = qdbm;
}

// Smoothed over about a minute, so a person walking past or a microwave oven
// does not move the power on its own.
static void tx_check_cb(void *arg)
{
    (void)arg;
    wifi_ap_record_t ap;

    if (s_state != CO2_NET_CONNECTED || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return;
    }
    s_rssi_avg = (s_rssi_avg == 0) ? ap.rssi : (s_rssi_avg * 3 + ap.rssi) / 4;

    if (s_rssi_avg < WIFI_TX_RAISE_RSSI) {
        tx_power_set(WIFI_TX_HIGH_QDBM, "weak signal");
    } else if (s_rssi_avg > WIFI_TX_LOWER_RSSI) {
        tx_power_set(WIFI_TX_LOW_QDBM, "good signal");
    }
}

static void retry_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static int retry_delay_ms(int attempt)
{
    int ms = WIFI_RETRY_FIRST_MS;

    while (attempt-- > 0 && ms < WIFI_RETRY_MAX_MS) {
        ms *= 2;
    }
    return ms < WIFI_RETRY_MAX_MS ? ms : WIFI_RETRY_MAX_MS;
}

void net_topic(char *out, size_t len, const char *leaf)
{
    snprintf(out, len, "%s/%s", g_topic_base, leaf);
}

// Copies src into dst as the inside of a JSON string: quotes and backslashes
// escaped, control characters (which the setup page refuses anyway) turned
// into spaces. dst needs twice the length of src plus one to never truncate.
// Without it a device named Salon "2" would make every payload invalid JSON,
// and Home Assistant drops those without a word.
void net_json_escape(char *dst, size_t len, const char *src)
{
    size_t o = 0;

    for (; *src != '\0' && o + 2 < len; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else {
            dst[o++] = c < 0x20 ? ' ' : (char)c;
        }
    }
    dst[o] = '\0';
}

co2_net_state_t co2_net_state(void)
{
    return s_state;
}

bool co2_net_is_connected(void)
{
    return s_mqtt_up;
}

int co2_net_tx_power_dbm(void)
{
    return s_tx_qdbm / 4;
}

bool co2_net_broker_set(void)
{
    return g_cfg.mqtt_uri != NULL && g_cfg.mqtt_uri[0] != '\0';
}

bool co2_net_link_info(char *ip, size_t ip_len, int *rssi)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;

    if (sta == NULL || esp_netif_get_ip_info(sta, &info) != ESP_OK || info.ip.addr == 0) {
        return false;
    }
    snprintf(ip, ip_len, IPSTR, IP2STR(&info.ip));

    wifi_ap_record_t ap;
    *rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
    return true;
}

const char *co2_net_device_id(void)
{
    return g_device_id;
}

const char *co2_net_topic_base(void)
{
    return g_topic_base;
}

const char *co2_net_default_topic_base(void)
{
    return s_topic_default;
}

bool co2_net_topic_base_valid(const char *s)
{
    size_t len = s != NULL ? strlen(s) : 0;

    // A base that fills the buffer would truncate on copy and move the topics.
    if (len == 0 || len >= TOPIC_BASE_MAX) {
        return false;
    }

    // Wildcards are illegal in a published topic, and an empty level ("a//b",
    // a leading or trailing slash) is legal but almost always a typo that
    // splits the tree in two. A leading '$' is reserved for the broker.
    if (s[0] == '/' || s[len - 1] == '/' || s[0] == '$' || strstr(s, "//") != NULL) {
        return false;
    }
    // Quotes and backslashes are legal in a topic but would have to be
    // escaped wherever the topic goes into JSON (the discovery payloads), and
    // nobody needs them in a topic name.
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '+' || c == '#' || c == '"' || c == '\\' || c < 0x20 || c == 0x7F) {
            return false;
        }
    }
    return true;
}

// The base a configured value ends up as: itself when valid, the MAC default
// when it is empty or not valid. The page validates too, so the second case is
// a stale or hand-written value, and the default beats a broken topic.
static const char *effective_base(const char *wanted)
{
    return (wanted != NULL && co2_net_topic_base_valid(wanted)) ? wanted : s_topic_default;
}

// Same rules as a topic base, since it is the first level of a topic.
static const char *effective_ha_prefix(const char *wanted)
{
    return (wanted != NULL && co2_net_topic_base_valid(wanted)) ? wanted : "homeassistant";
}

static void copy_cfg(const co2_net_config_t *cfg)
{
    #define COPY(dst, src) do {                                \
        if ((src) != NULL) {                                   \
            snprintf((dst), sizeof(dst), "%s", (src));         \
        } else {                                               \
            (dst)[0] = '\0';                                   \
        }                                                      \
    } while (0)

    COPY(s_ssid,      cfg->wifi_ssid);
    COPY(s_pass,      cfg->wifi_pass);
    COPY(s_uri,       cfg->mqtt_uri);
    COPY(s_user,      cfg->mqtt_user);
    COPY(s_mqtt_pass, cfg->mqtt_pass);
    COPY(s_prefix,    cfg->topic_prefix != NULL ? cfg->topic_prefix : "co2monitor");
    COPY(s_topic,     cfg->topic_base);
    COPY(s_name,      cfg->device_name);
    COPY(s_ha_prefix, cfg->ha_prefix);

    #undef COPY

    g_cfg.wifi_ssid    = s_ssid;
    g_cfg.wifi_pass    = s_pass;
    g_cfg.mqtt_uri     = s_uri;
    g_cfg.mqtt_user    = s_user[0]      != '\0' ? s_user      : NULL;
    g_cfg.mqtt_pass    = s_mqtt_pass[0] != '\0' ? s_mqtt_pass : NULL;
    g_cfg.topic_prefix = s_prefix;
    g_cfg.topic_base   = s_topic[0] != '\0' ? s_topic : NULL;
    g_cfg.retain       = cfg->retain;
    g_cfg.device_name  = s_name[0] != '\0' ? s_name : NULL;
    g_cfg.ha_discovery = cfg->ha_discovery;
    g_cfg.ha_prefix    = s_ha_prefix[0] != '\0' ? s_ha_prefix : NULL;
    g_cfg.chart_bar_s  = cfg->chart_bar_s;

    if (cfg->mqtt_ca != NULL && cfg->mqtt_ca[0] != '\0') {
        s_ca = strdup(cfg->mqtt_ca);
    }

    // Half a pair is useless: the handshake needs the key to prove it owns the
    // certificate. Dropped rather than passed on, so the broker sees a client
    // without a certificate instead of esp-tls failing obscurely.
    if (cfg->mqtt_client_cert != NULL && cfg->mqtt_client_cert[0] != '\0'
        && cfg->mqtt_client_key != NULL && cfg->mqtt_client_key[0] != '\0') {
        s_cert = strdup(cfg->mqtt_client_cert);
        s_key  = strdup(cfg->mqtt_client_key);
    }

    g_cfg.mqtt_ca          = s_ca;
    g_cfg.mqtt_client_cert = s_cert;
    g_cfg.mqtt_client_key  = s_key;
}

//==============================================================================
// MQTT
//==============================================================================

// Hands a message to the MQTT task, which sends it from its own loop.
// esp_mqtt_client_publish() would write to the socket right here instead, in
// the caller's task, and with a stalled link that blocks for the whole network
// timeout: the display loop would freeze for seconds per message. Queuing still
// takes the client's lock, which the MQTT task holds while it sends, so a
// stalled link can still delay the caller, but the caller no longer does the
// sending. QoS 0 needs
// store=true to be queued at all; the outbox limit bounds what a stalled link
// can pile up.
void net_enqueue(const char *topic, const char *payload, int qos, int retain)
{
    if (esp_mqtt_client_enqueue(s_client, topic, payload, 0, qos, retain, true) < 0) {
        ESP_LOGW(TAG, "not queued, outbox full: %s", topic);
    }
}

// Retained, so whoever looks at the broker can tell which board is which and
// what it runs without reaching its web page. Refreshed now and then because
// the signal and the uptime move; the rest only changes with a reboot.
static void publish_info(int qos)
{
    char topic[TOPIC_LEN];
    // 320 bytes at most: the longest version, hardware revision and hostname,
    // and an uptime of ten digits.
    char payload[352];
    char ip[16] = "";
    int  rssi   = 0;
    uint8_t mac[6] = { 0 };
    char hw[64];

    net_json_escape(hw, sizeof(hw), fw_info_hw_revision());

    co2_net_link_info(ip, sizeof(ip), &rssi);
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    int n = snprintf(payload, sizeof(payload),
        "{"
          "\"fw\":\"%s\","
          "\"hw\":\"%s\","
          "\"sensor_fw\":\"%s\","
          "\"ip\":\"%s\","
          "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
          "\"hostname\":\"%s.local\","
          "\"rssi\":%d,"
          "\"tx_dbm\":%d,"
          "\"uptime_s\":%lld,"
          "\"reset\":\"%s\","
          "\"chart_bar_s\":%u"
        "}",
        fw_info_version(), hw, fw_info_sensor(), ip,
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        s_hostname, rssi, co2_net_tx_power_dbm(), esp_timer_get_time() / 1000000,
        fw_info_reset_reason(), (unsigned)g_cfg.chart_bar_s);
    if (n < 0 || n >= (int)sizeof(payload)) {
        return;
    }

    net_topic(topic, sizeof(topic), TOPIC_INFO);
    net_enqueue(topic, payload, qos, 1);
    s_info_at = esp_timer_get_time();
}

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        char topic[TOPIC_LEN];

        s_mqtt_up = true;
        ESP_LOGI(TAG, "MQTT connected to %s", g_cfg.mqtt_uri);

        // Counterpart of the last will: the broker publishes "offline" on our
        // behalf if we drop, so we claim "online" as soon as we are back.
        net_topic(topic, sizeof(topic), TOPIC_STATUS);
        esp_mqtt_client_publish(s_client, topic, STATUS_ONLINE, 0, 1, 1);

        publish_info(1);

        // Republished on every connection because Home Assistant forgets the
        // entities when its own MQTT integration restarts.
        if (g_cfg.ha_discovery) {
            ha_discovery_publish(s_client);
        }
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_up = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        break;

    case MQTT_EVENT_ERROR: {
        // The detail is what tells a TLS problem (a CA that does not match, a
        // wrong client certificate) from a broker saying no, which a bare
        // "MQTT error" never did.
        const esp_mqtt_event_handle_t ev = event_data;
        const esp_mqtt_error_codes_t *e  = ev->error_handle;

        if (e != NULL && e->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "MQTT transport error: %s, TLS stack 0x%x, cert flags 0x%x, errno %d",
                     esp_err_to_name(e->esp_tls_last_esp_err), e->esp_tls_stack_err,
                     e->esp_tls_cert_verify_flags, e->esp_transport_sock_errno);
        } else if (e != NULL && e->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGE(TAG, "MQTT connection refused by the broker, code %d",
                     (int)e->connect_return_code);
        } else {
            ESP_LOGE(TAG, "MQTT error");
        }
        break;
    }

    default:
        break;
    }
}

static esp_err_t mqtt_start(void)
{
    char will_topic[TOPIC_LEN];

    net_topic(will_topic, sizeof(will_topic), TOPIC_STATUS);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri            = g_cfg.mqtt_uri,
        .credentials.username          = g_cfg.mqtt_user,
        .credentials.authentication.password = g_cfg.mqtt_pass,
        .credentials.client_id         = g_device_id,

        // Only read for mqtts://. A CA the user supplied is for a broker with
        // its own certificate authority; otherwise the certificate is checked
        // against the bundled public roots, which covers the cloud brokers and
        // anything with a Let's Encrypt certificate. There is deliberately no
        // way to skip verification: TLS without it only hides the password
        // from a passive listener, not from whoever answers in the broker's
        // place.
        .broker.verification.certificate       = s_ca,
        .broker.verification.crt_bundle_attach = s_ca == NULL ? esp_crt_bundle_attach : NULL,

        // Mutual TLS, for brokers that take a certificate instead of a
        // password. The username still goes out: Azure Event Grid, for one,
        // matches it against the client's authentication name.
        .credentials.authentication.certificate = s_cert,
        .credentials.authentication.key         = s_key,

        // The will is what makes an unplugged device show as unavailable in
        // Home Assistant instead of freezing on its last reading.
        .session.last_will.topic       = will_topic,
        .session.last_will.msg         = STATUS_OFFLINE,
        .session.last_will.msg_len     = strlen(STATUS_OFFLINE),
        .session.last_will.qos         = 1,
        .session.last_will.retain      = 1,

        // Readings are queued (see net_enqueue()), so a link that stalls while
        // it still looks connected would otherwise grow the outbox without
        // bound.
        // Several bars of readings plus the discovery burst fit well inside.
        .outbox.limit                  = 8192,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL));
    return esp_mqtt_client_start(s_client);
}

void co2_net_publish(uint16_t ppm, float temperature, float humidity)
{
    char topic[TOPIC_LEN];
    char payload[80];
    int  retain = g_cfg.retain ? 1 : 0;

    // A NaN would print as "nan", which is neither a number for the plain
    // topics nor valid JSON for the state topic.
    if (!s_mqtt_up || s_leaving || !isfinite(temperature) || !isfinite(humidity)) {
        return;
    }

    // QoS 0: a new average follows with the next chart bar, so a lost one is
    // not worth a retransmission.
    snprintf(payload, sizeof(payload), "%u", (unsigned)ppm);
    net_topic(topic, sizeof(topic), TOPIC_CO2);
    net_enqueue(topic, payload, 0, retain);

    snprintf(payload, sizeof(payload), "%.1f", temperature);
    net_topic(topic, sizeof(topic), TOPIC_TEMPERATURE);
    net_enqueue(topic, payload, 0, retain);

    snprintf(payload, sizeof(payload), "%.0f", humidity);
    net_topic(topic, sizeof(topic), TOPIC_HUMIDITY);
    net_enqueue(topic, payload, 0, retain);

    // The same reading as one message, for consumers that take a JSON object
    // per sample (Telegraf, Node-RED, ioBroker) and would otherwise have to
    // stitch three topics back together. Same rounding as the plain topics.
    snprintf(payload, sizeof(payload),
             "{\"co2\":%u,\"temperature\":%.1f,\"humidity\":%.0f}",
             (unsigned)ppm, temperature, humidity);
    net_topic(topic, sizeof(topic), TOPIC_STATE);
    net_enqueue(topic, payload, 0, retain);

    if (esp_timer_get_time() - s_info_at >= INFO_PERIOD_US) {
        publish_info(0);
    }
}

// Worst cases for a goodbye. The outbox drains one message per turn of the
// MQTT task, each quick once the broker acknowledges it. The disconnect
// request is only seen after the task's read poll, which lasts up to a second.
#define GOODBYE_DRAIN_MS      2000
#define GOODBYE_DISCONNECT_MS 1500
#define GOODBYE_POLL_MS       50

static bool outbox_empty(void)
{
    return esp_mqtt_client_get_outbox_size(s_client) == 0;
}

static bool mqtt_down(void)
{
    return !s_mqtt_up;
}

static bool wait_for(bool (*done)(void), int timeout_ms)
{
    for (int t = 0; !done(); t += GOODBYE_POLL_MS) {
        if (t >= timeout_ms) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(GOODBYE_POLL_MS));
    }
    return true;
}

// Empty retained payloads, which is how MQTT deletes a retained message.
static void clear_readings(void)
{
    static const char *const leaves[] = {
        TOPIC_CO2, TOPIC_TEMPERATURE, TOPIC_HUMIDITY, TOPIC_STATE,
    };
    char topic[TOPIC_LEN];

    for (size_t i = 0; i < sizeof(leaves) / sizeof(leaves[0]); i++) {
        net_topic(topic, sizeof(topic), leaves[i]);
        net_enqueue(topic, "", 1, 1);
    }
}

void co2_net_goodbye(const co2_net_config_t *next)
{
    char topic[TOPIC_LEN];

    if (s_client == NULL || !s_mqtt_up) {
        return;   // setup mode, radio off, or no broker to tell
    }
    s_leaving = true;

    // No next configuration means the same one again.
    const char *base   = next != NULL ? effective_base(next->topic_base)     : g_topic_base;
    const char *prefix = next != NULL ? effective_ha_prefix(next->ha_prefix) : g_ha_prefix;
    bool retain        = next != NULL ? next->retain       : g_cfg.retain;
    bool discovery     = next != NULL ? next->ha_discovery : g_cfg.ha_discovery;
    bool moved         = strcmp(base, g_topic_base) != 0;

    // Whatever the next boot will not publish again has to go now, while the
    // old topics are still known. The next boot republishes discovery under
    // the new prefix, and pointing at the new base, by itself.
    if (g_cfg.ha_discovery && (!discovery || strcmp(prefix, g_ha_prefix) != 0)) {
        ha_discovery_clear();
    }

    // Only when readings are retained now: clearing sends an empty message to
    // every live subscriber too, which a consumer parsing numbers would choke
    // on for nothing. Retained readings left over from an earlier setting are
    // not tracked, and stay.
    if (g_cfg.retain && (moved || !retain)) {
        clear_readings();
    }

    // Moving away, the old base is emptied instead: an "offline" left there
    // would describe a device that no longer publishes under it.
    net_topic(topic, sizeof(topic), TOPIC_STATUS);
    if (moved) {
        net_enqueue(topic, "", 1, 1);
        net_topic(topic, sizeof(topic), TOPIC_INFO);
        net_enqueue(topic, "", 1, 1);
    } else {
        net_enqueue(topic, STATUS_OFFLINE, 1, 1);
    }

    bool drained = wait_for(outbox_empty, GOODBYE_DRAIN_MS);

    // A clean DISCONNECT, or the broker would publish the last will once the
    // link times out: "offline" again, and on a status topic just cleared.
    esp_mqtt_client_disconnect(s_client);
    bool closed = wait_for(mqtt_down, GOODBYE_DISCONNECT_MS);

    if (drained && closed) {
        ESP_LOGI(TAG, "left the broker%s", moved ? ", old topics cleared" : "");
    } else {
        ESP_LOGW(TAG, "left the broker without confirmation (%s)",
                 drained ? "disconnect" : "outbox");
    }
}

//==============================================================================
// Wi-Fi
//==============================================================================

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    // In setup mode the radio runs APSTA for scanning, so the station events
    // still arrive. Connecting there would mean joining with empty credentials.
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (!s_ap_mode) {
            esp_wifi_connect();
        }
        return;
    }

    if (base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        s_ap_clients++;
        s_state = CO2_NET_AP_CLIENT;
        ESP_LOGI(TAG, "client joined the setup AP (%d)", s_ap_clients);
        return;
    }

    if (base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_ap_clients > 0) {
            s_ap_clients--;
        }
        s_state = s_ap_clients > 0 ? CO2_NET_AP_CLIENT : CO2_NET_AP_IDLE;
        ESP_LOGI(TAG, "client left the setup AP (%d)", s_ap_clients);
        return;
    }

    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_ap_mode) {
            return;
        }
        const wifi_event_sta_disconnected_t *ev = event_data;
        int delay_ms = retry_delay_ms(s_retries++);

        s_mqtt_up  = false;
        s_state    = CO2_NET_CONNECTING;
        s_rssi_avg = 0;   // the next link may be to another access point

        if (s_retries >= WIFI_TX_RAISE_RETRIES) {
            tx_power_set(WIFI_TX_HIGH_QDBM, "cannot connect");
        }

        // The reason tells a wrong password (202, or 15 while the handshake
        // times out) from a network out of range (201) or a dropped link (200).
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason %u, retry %d in %d s",
                 ev->reason, s_retries, delay_ms / 1000);

        // ESP_ERR_INVALID_STATE means a retry is already pending, which is
        // what we want. Any other failure falls back to trying at once rather
        // than never again.
        esp_err_t err = s_retry_timer != NULL
                      ? esp_timer_start_once(s_retry_timer, delay_ms * 1000LL)
                      : ESP_ERR_INVALID_ARG;
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            esp_wifi_connect();
        }
        return;
    }

    if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;

        s_retries = 0;
        s_state   = CO2_NET_CONNECTED;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&ev->ip_info.ip));

        mdns_start();

        // MQTT is started on the first address and only reconnects afterwards:
        // esp-mqtt retries on its own once the client exists.
        if (s_client == NULL) {
            esp_err_t err = mqtt_start();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "MQTT not started: %s", esp_err_to_name(err));
            }
        }
    }
}

// Everything both modes need, and that may only happen once per boot.
static esp_err_t net_core_init(const co2_net_config_t *cfg)
{
    uint8_t mac[6] = { 0 };

    if (s_core_ready) {
        return ESP_OK;
    }

    copy_cfg(cfg);

    // Wi-Fi keeps its calibration data in NVS, so this has to succeed before
    // esp_wifi_init(). A truncated or outdated partition is recoverable.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Read before esp_wifi_init() because it works off the efuse, and it gives
    // every board a stable identity without storing anything.
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));

    char suffix[8];
    snprintf(suffix, sizeof(suffix), "%02x%02x%02x", mac[3], mac[4], mac[5]);

    // The identifier and the default topics are built from the MAC, never
    // from the name: renaming a device in Home Assistant must not move its
    // topics, or the history and any external subscriber would break. A base
    // chosen by the user replaces the topics but never the identifier, which
    // is also the MQTT client id and must stay unique per board.
    snprintf(g_device_id,     sizeof(g_device_id),     "%s_%s", g_cfg.topic_prefix, suffix);
    snprintf(s_topic_default, sizeof(s_topic_default), "%s/%s", g_cfg.topic_prefix, suffix);

    const char *base = effective_base(g_cfg.topic_base);
    if (g_cfg.topic_base != NULL && base != g_cfg.topic_base) {
        ESP_LOGW(TAG, "topic base \"%s\" not valid, using %s",
                 g_cfg.topic_base, s_topic_default);
    }
    snprintf(g_topic_base, sizeof(g_topic_base), "%s", base);
    snprintf(g_ha_prefix,  sizeof(g_ha_prefix),  "%s", effective_ha_prefix(g_cfg.ha_prefix));

    snprintf(s_ap_ssid,     sizeof(s_ap_ssid),     "CO2-Monitor-%s", suffix);
    snprintf(s_hostname,    sizeof(s_hostname),    "co2-monitor-%s", suffix);

    if (g_cfg.device_name != NULL) {
        snprintf(g_device_name, sizeof(g_device_name), "%s", g_cfg.device_name);
    } else {
        snprintf(g_device_name, sizeof(g_device_name), "CO2 Monitor %s", suffix);
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    // Registered here rather than per mode: the access point events matter too.
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler, NULL, NULL));

    s_core_ready = true;
    return ESP_OK;
}

const char *co2_net_ap_ssid(void)
{
    return s_ap_ssid;
}

const char *co2_net_hostname(void)
{
    return s_hostname;
}

// Started once an address exists, because the responder has to bind to it.
//
// The host name comes from the MAC so a bookmark keeps working after a rename,
// while the instance name, which is what shows up when browsing the network,
// carries the name the user chose. Stable address, friendly label.
static void mdns_start(void)
{
    static bool started;

    if (started) {
        return;
    }

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init: %s", esp_err_to_name(err));
        return;
    }

    mdns_hostname_set(s_hostname);
    mdns_instance_name_set(g_device_name);

    // Advertising the web interface makes the device show up in the network
    // browsers that list _http._tcp, not just resolve by name.
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    started = true;
    ESP_LOGI(TAG, "reachable at http://%s.local", s_hostname);
}

static esp_err_t wifi_start(void)
{
    esp_netif_create_default_wifi_sta();

    const esp_timer_create_args_t retry = {
        .callback = retry_cb,
        .name     = "wifi_retry",
    };
    esp_err_t err = esp_timer_create(&retry, &s_retry_timer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "retry timer: %s, retrying without a pause", esp_err_to_name(err));
    }

    wifi_config_t wc = { 0 };
    snprintf((char *)wc.sta.ssid,     sizeof(wc.sta.ssid),     "%s", g_cfg.wifi_ssid);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", g_cfg.wifi_pass);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    // Modem sleep: the radio wakes for each DTIM beacon and otherwise stays
    // off, which is most of the draw when it idles. The cost is latency on
    // incoming traffic (MQTT, the web page), up to a beacon interval, and
    // nothing here needs it faster than that.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(WIFI_TX_LOW_QDBM));

    const esp_timer_create_args_t tx = {
        .callback = tx_check_cb,
        .name     = "wifi_tx",
    };
    err = esp_timer_create(&tx, &s_tx_timer);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(s_tx_timer, WIFI_TX_CHECK_US);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TX power stays at %d dBm: %s", WIFI_TX_LOW_QDBM / 4,
                 esp_err_to_name(err));
    }
    return ESP_OK;
}

esp_err_t co2_net_start(const co2_net_config_t *cfg)
{
    if (cfg == NULL || cfg->wifi_ssid == NULL || cfg->mqtt_uri == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_ERROR_CHECK(net_core_init(cfg));

    ESP_LOGI(TAG, "device %s, topics %s/#, broker %s%s",
             g_device_id, g_topic_base, g_cfg.mqtt_uri,
             s_cert != NULL ? " (client certificate)" : "");

    s_state = CO2_NET_CONNECTING;
    return wifi_start();
}

esp_err_t co2_net_start_ap(const co2_net_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_ERROR_CHECK(net_core_init(cfg));

    s_ap_mode = true;
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();   // needed for scanning, never connected

    wifi_config_t wc = { 0 };
    snprintf((char *)wc.ap.ssid, sizeof(wc.ap.ssid), "%s", s_ap_ssid);
    wc.ap.ssid_len       = strlen(s_ap_ssid);
    wc.ap.channel        = 1;
    wc.ap.max_connection = 4;

    // Open on purpose. A password would have to be printed on the enclosure or
    // derived from the MAC, and neither protects anything: whoever is in radio
    // range during setup can read it. The window is short and the page holds
    // nothing until the user types it.
    wc.ap.authmode = WIFI_AUTH_OPEN;

    // APSTA rather than AP: scanning for networks needs the station interface,
    // and the setup page has to offer a list to choose from.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    esp_err_t err = esp_wifi_start();
    if (err == ESP_OK) {
        s_state = CO2_NET_AP_IDLE;
        ESP_LOGI(TAG, "provisioning AP \"%s\" up on 192.168.4.1", s_ap_ssid);
    }
    return err;
}

int co2_net_scan(co2_net_scan_result_t *out, int max)
{
    // Errors go back negated: esp_err_t codes are positive, and the caller
    // tells a count from an error by the sign.
    if (out == NULL || max <= 0) {
        return -ESP_ERR_INVALID_ARG;
    }

    // Blocking scan: the caller is an HTTP handler with its own task, and the
    // asynchronous form would need an event and a buffer to hand over.
    wifi_scan_config_t scan = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
        return -err;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        return 0;
    }

    wifi_ap_record_t *recs = calloc(found, sizeof(*recs));
    if (recs == NULL) {
        esp_wifi_clear_ap_list();
        return -ESP_ERR_NO_MEM;
    }

    err = esp_wifi_scan_get_ap_records(&found, recs);
    if (err != ESP_OK) {
        free(recs);
        return -err;
    }

    // esp_wifi returns them sorted by RSSI, strongest first. Duplicates are
    // common when a network has several access points, and showing the same
    // name four times helps nobody.
    int n = 0;
    for (uint16_t i = 0; i < found && n < max; i++) {
        if (recs[i].ssid[0] == '\0') {
            continue;
        }

        bool dup = false;
        for (int j = 0; j < n; j++) {
            if (strcmp(out[j].ssid, (const char *)recs[i].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }

        snprintf(out[n].ssid, sizeof(out[n].ssid), "%s", (const char *)recs[i].ssid);
        out[n].rssi   = recs[i].rssi;
        out[n].secure = (recs[i].authmode != WIFI_AUTH_OPEN);
        n++;
    }

    free(recs);
    return n;
}
