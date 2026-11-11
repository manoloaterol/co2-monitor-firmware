#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mqtt_client.h"

#include "co2_net.h"

// Topics hang off a per-device base so a plain MQTT consumer sees a tidy tree
// and two boards never collide:
//   <base>/co2  .../temperature  .../humidity  .../state  .../status  .../info
#define TOPIC_CO2         "co2"
#define TOPIC_TEMPERATURE "temperature"
#define TOPIC_HUMIDITY    "humidity"
#define TOPIC_STATE       "state"
#define TOPIC_STATUS      "status"
#define TOPIC_INFO        "info"

#define STATUS_ONLINE  "online"
#define STATUS_OFFLINE "offline"

// Long enough for "<base>/temperature" with the longest base allowed.
#define TOPIC_BASE_MAX 64   // with terminator
#define TOPIC_LEN      96

// Shared by the two translation units.
extern co2_net_config_t g_cfg;

// "co2monitor_a4cf12": MQTT client id and the stem of every unique_id.
extern char g_device_id[32];

// "co2monitor/a4cf12", or the base the user chose: every topic hangs off this.
extern char g_topic_base[TOPIC_BASE_MAX];

// "CO2 Monitor a4cf12": only ever shown, never used as an identifier, so it is
// safe to rename the device in Home Assistant without breaking anything.
extern char g_device_name[48];

// "homeassistant", or the root the user chose for discovery.
extern char g_ha_prefix[32];

// Copies src into dst as the inside of a JSON string. dst needs twice the
// length of src plus one to never truncate.
void net_json_escape(char *dst, size_t len, const char *src);

// Builds "<base>/<leaf>" into out.
void net_topic(char *out, size_t len, const char *leaf);

// Hands a message to the MQTT task instead of writing to the socket from the
// caller. Logs, and drops the message, when the outbox is full.
void net_enqueue(const char *topic, const char *payload, int qos, int retain);

// Publishes the three Home Assistant discovery messages, retained. Called once
// per broker connection: HA recreates the entities after its own restart.
void ha_discovery_publish(esp_mqtt_client_handle_t client);

// Deletes those three messages from the broker, under the current g_ha_prefix,
// so Home Assistant drops the entities.
void ha_discovery_clear(void);
