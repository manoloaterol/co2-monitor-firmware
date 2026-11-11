idf_component_register(SRCS "co2_net.c"
                            "ha_discovery.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_event
                                     esp_wifi
                                     esp_netif
                                     esp_timer
                                     fw_info
                                     mbedtls
                                     nvs_flash
                                     espressif__mqtt
                                     espressif__mdns)
