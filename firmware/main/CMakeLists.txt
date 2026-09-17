idf_component_register(
    SRCS "main.c"
         "device.c"
         "sensor.c"
         "metrics.c"
         "events.c"
         "history.c"
         "api.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi esp_timer esp_app_format nvs_flash json esp_http_server esp_driver_gpio mqtt
                  esp32-owb esp32-ds18b20
    EMBED_TXTFILES ${project_dir}/certs/ota_server_cert_15.pem ${project_dir}/web/manifest.webmanifest
    EMBED_FILES ${project_dir}/web/apple-touch-icon.png)

home_idf_login_page(${COMPONENT_LIB} NAME "h-controller" ACCENT "#e8604c")
home_idf_app_page(${COMPONENT_LIB} SRC ${project_dir}/web/app.html NAME "h-controller")
