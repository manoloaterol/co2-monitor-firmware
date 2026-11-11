idf_component_register(SRCS "display.c" "fonts.c"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES esp_driver_spi esp_driver_gpio)
