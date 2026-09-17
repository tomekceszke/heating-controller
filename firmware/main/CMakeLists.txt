idf_component_register(SRCS "main.c"
        "wifi.c"
        "log_dispatch.c"
        "log_udp.c"
        "ota.c"
        "ntp.c"
        "api.c"
        "sensor.c"
        "metrics.c"
        PRIV_REQUIRES esp_wifi
        esp_http_client
        esp_http_server
        esp_https_ota
        nvs_flash
        esp_driver_gpio
        mqtt
        esp32-owb
        esp32-ds18b20
        EMBED_TXTFILES ${project_dir}/certs/ota_server_cert_15.pem
)
