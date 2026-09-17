#pragma once

/* WIFI */
#define WIFI_RETRY_DELAY_S              60

/* NTP */
#define NTP_MAX_ATTEMPTS                20
#define NTP_RETRY_DELAY_S               10

/* OTA */
#define OTA_URL                         "https://192.168.11.15:8070/heating-controller.bin"

/* LOGGING */
#define LOG_UDP_IP                      "192.168.11.15"
#define LOG_UDP_PORT                    1340

/* HTTPD */
#define HTTPD_PORT                      80

/* MQTT (metrics to hc-data broker; user/pass in credentials.h) */
#define MQTT_BROKER_URI                 "mqtt://192.168.11.16:1883"
#define MQTT_TOPIC_PREFIX               "heating"
#define MQTT_OUTBOX_LIMIT_BYTES         24576

/* SENSORS */
#define GPIO_DS18B20_0                  GPIO_NUM_4
#define MAX_DEVICES                     8
#define DS18B20_RESOLUTION              DS18B20_RESOLUTION_12_BIT
#define TEMP_SENSOR_SCAN_RETRY_S        30
#define SAMPLE_PERIOD_S                 60
#define INVALID_TEMPERATURE_INDICATOR   (-273)
