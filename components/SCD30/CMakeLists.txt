idf_component_register(SRCS "scd30_i2c.c" "sensirion_common.c" "sensirion_i2c_hal.c" "sensirion_i2c.c"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES esp_driver_i2c)
