idf_component_register(
        SRCS "ds18b20.c"
        REQUIRES "driver"
        PRIV_REQUIRES "esp32-owb" "esp_timer"
        INCLUDE_DIRS "include"
)


