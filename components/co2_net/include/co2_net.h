#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Wi-Fi station plus MQTT publishing, with optional Home Assistant discovery.
//
// Discovery is not an alternative to plain MQTT: readings always go to flat
// topics, and discovery only adds a retained config message that points Home
// Assistant at those same topics. A plain MQTT consumer ignores it.
//
// By default the topics carry the last three bytes of the MAC, so several
// boards can share a broker without colliding. The whole base can be replaced:
//
//   <base>/co2  /temperature  /humidity   one plain value each
//   <base>/state                          the three together, as JSON
//   <base>/status                         online / offline, retained, last will
//   <base>/info                           firmware, address, signal, retained
typedef struct {
    const char *wifi_ssid;
    const char *wifi_pass;

    const char *mqtt_uri;       // "mqtt://192.168.1.10:1883" or "mqtts://..."
    const char *mqtt_user;      // NULL for an anonymous broker
    const char *mqtt_pass;
    const char *mqtt_ca;        // PEM for an mqtts:// broker with a private
                                // CA; NULL checks against the public roots
    const char *mqtt_client_cert;   // PEM, for brokers that authenticate the
    const char *mqtt_client_key;    // client by certificate; both or neither

    const char *topic_prefix;   // e.g. "co2monitor"; NULL defaults to that.
                                // The MAC suffix is appended automatically.
    const char *topic_base;     // replaces "<prefix>/<mac>" when set and
                                // valid; NULL or empty keeps the default
    bool        retain;         // publish readings retained
    const char *device_name;    // shown in Home Assistant; NULL or empty falls
                                // back to "CO2 Monitor <mac>"
    bool        ha_discovery;   // publish the Home Assistant config messages
    const char *ha_prefix;      // discovery root; NULL, empty or not valid
                                // falls back to "homeassistant"
    uint16_t    chart_bar_s;    // seconds between readings, one per chart
                                // bar; reported in <base>/info
} co2_net_config_t;

// Brings up Wi-Fi and MQTT. Returns as soon as the attempt is under way: the
// link comes up in the background and publishing is a no-op until it does.
// cfg is copied, so the caller need not keep it alive.
esp_err_t co2_net_start(const co2_net_config_t *cfg);

// Queues the reading for the MQTT task and returns; the caller never writes to
// the socket itself. It can still wait for the client's lock while the MQTT
// task is sending. Does nothing while the broker is unreachable.
void co2_net_publish(uint16_t ppm, float temperature, float humidity);

bool co2_net_is_connected(void);

// True when a broker is configured, so a broker that is not connected is
// something to wait for rather than the normal state.
bool co2_net_broker_set(void);

// Leaves the broker cleanly before a restart: retained messages the next boot
// will not overwrite are deleted, the status turns "offline", and a real
// DISCONNECT keeps the broker from firing the last will later. next is the
// configuration the device restarts with, of which only topic_base, retain,
// ha_discovery and ha_prefix are read; NULL means the same as now. Blocks for
// up to a few seconds and does nothing without a broker connection. Nothing
// is published after it, so it is only called right before esp_restart().
void co2_net_goodbye(const co2_net_config_t *next);

// The station's address and the signal of the access point it is joined to.
// Returns false, leaving both untouched, while there is no address.
bool co2_net_link_info(char *ip, size_t ip_len, int *rssi);

// The station's TX power cap in dBm: 15 normally, 20 while the signal is weak
// or joining keeps failing.
int co2_net_tx_power_dbm(void);

// What the radio is doing, for the status LED. Separated from
// co2_net_is_connected(), which is about the broker rather than the link.
typedef enum {
    CO2_NET_OFF,          // radio not started
    CO2_NET_CONNECTING,   // joining a network, no address yet
    CO2_NET_CONNECTED,    // on the network
    CO2_NET_AP_IDLE,      // serving the setup access point, nobody joined
    CO2_NET_AP_CLIENT,    // serving it with at least one client
} co2_net_state_t;

co2_net_state_t co2_net_state(void);

// "co2-monitor-a4cf12", the mDNS name the device answers to on the network,
// without the ".local" suffix. Derived from the MAC and never from the device
// name, so renaming the device does not break a bookmark. Valid once
// co2_net_start() has returned.
const char *co2_net_hostname(void);

//==============================================================================
// Provisioning mode
//
// A separate entry point: it brings up an open access point and does not touch
// MQTT. The radio runs in APSTA rather than plain AP because scanning for
// networks needs the station interface, and the setup page has to offer a list
// of networks to choose from.
//==============================================================================

// Starts the provisioning access point. Its name carries the MAC suffix so
// several devices within range stay distinguishable.
esp_err_t co2_net_start_ap(const co2_net_config_t *cfg);

// "CO2-Monitor-a4cf12". Valid once co2_net_start_ap() has returned.
const char *co2_net_ap_ssid(void);

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    bool    secure;
} co2_net_scan_result_t;

// Scans and writes at most max entries into out, strongest first, skipping
// hidden networks. Returns how many were written, or an esp_err_t negated.
// Blocks for a couple of seconds, so it is called from the HTTP handler and
// never from the display loop.
int co2_net_scan(co2_net_scan_result_t *out, int max);

// The base every topic hangs off, as in use: the configured one, or
// "co2monitor/a4cf12" when none was set or it was not valid.
// Valid once co2_net_start() or co2_net_start_ap() has returned.
const char *co2_net_topic_base(void);

// "co2monitor/a4cf12", what an empty topic base falls back to.
const char *co2_net_default_topic_base(void);

// True if s can be used as a topic base: no wildcards, no empty levels, and
// short enough for every topic built on it. Empty is not valid here; callers
// treat it as "use the default" before asking.
bool co2_net_topic_base_valid(const char *s);

// Unique per board, derived from the Wi-Fi MAC, e.g. "co2monitor_a4cf12".
// Valid once co2_net_start() has returned.
const char *co2_net_device_id(void);
