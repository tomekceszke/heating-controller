#pragma once

#include "driver/gpio.h"

/* DEVICE (hostname, label and app URL are per board, see config/devices.h and device.c) */

/* SENSORS (1-Wire, esp32-owb RMT driver; the IDF 6 driver swap is tracked in docs/IDF6_MIGRATION.md) */
#define GPIO_DS18B20_0                  GPIO_NUM_4
#define MAX_DEVICES                     8       // max sensors on the bus (7 on heating-controller-2)
#define DS18B20_RESOLUTION              DS18B20_RESOLUTION_12_BIT
#define TEMP_SENSOR_SCAN_RETRY_S        30      // rescan interval while no sensor answers
#define SAMPLE_PERIOD_S                 60      // seconds between sensor reads (one row per sensor per minute)
#define SENSOR_FAIL_READS               3       // consecutive bad reads before the sensor counts as lost
#define INVALID_TEMPERATURE_INDICATOR   (-273)  // sentinel in /sensor responses, kept for the ulanzi display

/* MQTT (metrics to the hc-data broker; user/pass in credentials.h) */
#define MQTT_BROKER_URI                 "mqtt://192.168.11.16:1883"
#define MQTT_TOPIC_PREFIX               "heating"
#define MQTT_OUTBOX_LIMIT_BYTES         24576   // ~40 min of buffered readings for 7 sensors
#define METRICS_QUEUE_LEN               32      // handover to the metrics task; the outbox does the real buffering
#define MQTT_STATE_PERIOD_S             60      // retained heating/<mac>/state refresh; matches SAMPLE_PERIOD_S

/* EVENTS / HISTORY (RAM only; PostgreSQL on hc-data keeps the real history) */
#define EVENTS_RING_SIZE                50
#define HISTORY_PERIOD_S                60
#define HISTORY_SAMPLES                 1440    // 24 h

/* OTA */
#define OTA_FILE                        "heating-controller.bin"
#define OTA_URL                         "https://192.168.11.15:8070/" OTA_FILE

/* LOGGING */
#define LOG_UDP_IP                      "192.168.11.15"
#define LOG_UDP_PORT                    1340

/* HTTPD */
#define HTTPD_PORT                      80

/* HEALTH */
#define HEALTH_VERIFY_TIMEOUT_S         300     // new image: sensor task alive + WiFi within this, else rollback
#define HEALTH_MAX_UNVERIFIED_BOOTS     3
#define HEALTH_STATS_LOG_PERIOD_S       900

/* NOTIFY */
#define NOTIFY_ERROR_COOLDOWN_S         3600
