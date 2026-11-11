#include <stdio.h>
#include <string.h>

#include "net_private.h"

#include "fw_info.h"

#include "esp_log.h"
#include "mqtt_client.h"

static const char *TAG = "ha";

// Home Assistant discovery.
//
// One retained config message per entity, under the topic tree HA watches. The
// state_topic of each one points back at the plain topics this firmware would
// publish anyway, so discovery adds entities without changing the data path: a
// consumer that is not Home Assistant simply never reads these messages.
//
// The three entities share a "dev" object with the same identifier, which is
// what makes HA group them under a single device instead of three loose
// sensors. That grouping is also what the HomeKit bridge exports later.
//
// The keys are the documented short forms (stat_t, dev_cla...), which keep the
// payload well under the buffer below and cost nothing in readability once you
// know them.
// The worst case, with a 63-character topic base and a 47-character name that
// doubles when escaped, is about 620 bytes, and about 780 for the CO2 entity
// with its chart attribute.
#define PAYLOAD_LEN 896
#define TOPIC_LEN_HA 128

// Where Home Assistant looks for the config of one entity.
static void config_topic(char *out, size_t len, const char *leaf)
{
    snprintf(out, len, "%s/sensor/%s_%s/config", g_ha_prefix, g_device_id, leaf);
}

static void publish_entity(esp_mqtt_client_handle_t client,
                           const char *leaf,          // topic leaf and uniq_id suffix
                           const char *name,
                           const char *device_class,
                           const char *unit,
                           bool        chart_attr)
{
    char topic[TOPIC_LEN_HA];
    char state_topic[TOPIC_LEN];
    char avty_topic[TOPIC_LEN];
    char attr[TOPIC_LEN + 128] = "";
    char payload[PAYLOAD_LEN];
    char dev_name[2 * sizeof(g_device_name)];
    char hw[64];

    // The topics need no escaping: co2_net_topic_base_valid() refuses quotes
    // and backslashes in them.
    net_json_escape(dev_name, sizeof(dev_name), g_device_name);
    net_json_escape(hw, sizeof(hw), fw_info_hw_revision());

    net_topic(state_topic, sizeof(state_topic), leaf);
    net_topic(avty_topic,  sizeof(avty_topic),  TOPIC_STATUS);

    // The chart period, as an attribute a dashboard card can follow. The
    // template keeps only that field: the rest of info moves every few
    // minutes and would fill the recorder with attribute versions.
    if (chart_attr) {
        char info_topic[TOPIC_LEN];
        net_topic(info_topic, sizeof(info_topic), TOPIC_INFO);
        snprintf(attr, sizeof(attr),
                 "\"json_attr_t\":\"%s\","
                 "\"json_attr_tpl\":\"{{ {'chart_bar_s': value_json.chart_bar_s} | tojson }}\",",
                 info_topic);
    }

    config_topic(topic, sizeof(topic), leaf);

    int n = snprintf(payload, sizeof(payload),
        "{"
          "\"name\":\"%s\","
          "\"uniq_id\":\"%s_%s\","
          "\"stat_t\":\"%s\","
          "\"avty_t\":\"%s\","
          "\"pl_avail\":\"" STATUS_ONLINE "\","
          "\"pl_not_avail\":\"" STATUS_OFFLINE "\","
          "\"dev_cla\":\"%s\","
          "\"stat_cla\":\"measurement\","
          "\"unit_of_meas\":\"%s\","
          "%s"
          "\"dev\":{"
            "\"ids\":[\"%s\"],"
            "\"name\":\"%s\","
            "\"mdl\":\"ESP32-S2 + SCD30\","
            "\"mf\":\"MAT\","
            "\"sw\":\"%s\","
            "\"hw\":\"%s\","
            // "cu" is configuration_url: Home Assistant turns it into a link
            // to the device's own settings page. The mDNS name is used rather
            // than the address so it survives a new DHCP lease.
            "\"cu\":\"http://%s.local\""
          "}"
        "}",
        name, g_device_id, leaf, state_topic, avty_topic,
        device_class, unit, attr, g_device_id, dev_name, fw_info_version(),
        hw, co2_net_hostname());

    if (n < 0 || n >= (int)sizeof(payload)) {
        ESP_LOGE(TAG, "discovery payload for %s does not fit", leaf);
        return;
    }

    // Retained, so Home Assistant finds the entities when it starts after us.
    // QoS 1: unlike a reading, this message is not repeated later.
    esp_mqtt_client_publish(client, topic, payload, n, 1, 1);
    ESP_LOGI(TAG, "discovery published for %s", leaf);
}

// device_class is what earns the right icon, the right history graph and,
// through the HomeKit bridge, the right accessory type.
static const struct {
    const char *leaf;
    const char *name;
    const char *device_class;
    const char *unit;
    bool        chart_attr;   // carries the chart period as an attribute
} ENTITIES[] = {
    { TOPIC_CO2,         "CO2",         "carbon_dioxide", "ppm",      true  },
    { TOPIC_TEMPERATURE, "Temperature", "temperature",    "\\u00b0C", false },
    { TOPIC_HUMIDITY,    "Humidity",    "humidity",       "%",        false },
};

void ha_discovery_publish(esp_mqtt_client_handle_t client)
{
    for (size_t i = 0; i < sizeof(ENTITIES) / sizeof(ENTITIES[0]); i++) {
        publish_entity(client, ENTITIES[i].leaf, ENTITIES[i].name,
                       ENTITIES[i].device_class, ENTITIES[i].unit,
                       ENTITIES[i].chart_attr);
    }
}

// An empty retained message is how MQTT deletes a retained one, and Home
// Assistant removes the entity when its config goes. Queued rather than sent
// here, with QoS 1 so the caller can wait for the broker to confirm.
void ha_discovery_clear(void)
{
    char topic[TOPIC_LEN_HA];

    for (size_t i = 0; i < sizeof(ENTITIES) / sizeof(ENTITIES[0]); i++) {
        config_topic(topic, sizeof(topic), ENTITIES[i].leaf);
        net_enqueue(topic, "", 1, 1);
    }
    ESP_LOGI(TAG, "discovery cleared under %s", g_ha_prefix);
}
