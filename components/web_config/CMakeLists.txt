idf_component_register(SRCS "web_config.c"
                            "dns_server.c"
                            "ota.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_http_server
                                     esp_wifi
                                     esp_netif
                                     espressif__cjson
                                     app_config
                                     co2_net
                                     scd30_cfg
                                     app_update
                                     esp_timer
                                     fw_info
                       EMBED_TXTFILES "www/index.html")
