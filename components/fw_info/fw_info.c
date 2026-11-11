#include <stdio.h>

#include "fw_info.h"

#include "esp_app_desc.h"
#include "esp_system.h"
#include "sdkconfig.h"

static char s_sensor[8];   // "255.255" plus terminator

const char *fw_info_version(void)
{
    return esp_app_get_description()->version;
}

const char *fw_info_hw_revision(void)
{
    return CONFIG_CO2_HW_REVISION;
}

void fw_info_set_sensor(uint8_t major, uint8_t minor)
{
    snprintf(s_sensor, sizeof(s_sensor), "%u.%u", major, minor);
}

const char *fw_info_sensor(void)
{
    return s_sensor;
}

const char *fw_info_reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power_on";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    case ESP_RST_USB:       return "usb";
    case ESP_RST_JTAG:      return "jtag";
    default:                return "unknown";
    }
}
