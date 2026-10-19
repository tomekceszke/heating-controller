#pragma once

#include "driver/gpio.h"

/* DEVICE (hostname, label and app URL are per board, see config/devices.h and device.c) */

/* SENSORS (1-Wire, esp32-owb RMT driver; the IDF 6 driver swap is tracked in docs/IDF6_MIGRATION.md) */
#define GPIO_DS18B20_0                  GPIO_NUM_4
#define MAX_DEVICES                     16      // max sensors on the bus (7 on sensors-controller-2, more planned 2026-10-19)
#define DS18B20_RESOLUTION              DS18B20_RESOLUTION_12_BIT
#define TEMP_SENSOR_SCAN_RETRY_S        30      // rescan interval while no sensor answers
#define READ_PERIOD_S                   5       // seconds between sensor reads: a burner run of ~3 min is ~36 reads
#define SEND_DELTA_C                    0.2f    // a reading is sent on this change from the last one sent (hi_deadband)
#define SEND_MAX_GAP_S                  55      // ...or this long after it: with 5 s reads, a row at least every 60 s
#define SENSOR_LOST_AFTER_S             180     // failing this long in a row: the sensor counts as lost
#define SENSOR_FAIL_READS               (SENSOR_LOST_AFTER_S / READ_PERIOD_S)
#define SENSOR_ALIVE_S                  30      // a cycle at least this often: the sensor task is alive (hi_health)
#define INVALID_TEMPERATURE_INDICATOR   (-273)  // sentinel in /sensor responses, kept for the ulanzi display

/* MQTT (metrics to the hc-data broker; user/pass in credentials.h) */
#define MQTT_BROKER_URI                 "mqtt://192.168.11.16:1883"
#define MQTT_TOPIC_PREFIX               "sensors"
#define MQTT_OUTBOX_LIMIT_BYTES         32768   // ~50 min of 7 / ~25 min of 16 quiet sensors (a row a minute), less while they change
#define METRICS_QUEUE_LEN               32      // handover to the metrics task; the outbox does the real buffering
#define MQTT_STATE_PERIOD_S             60      // retained sensors/<mac>/state refresh

/* EVENTS / HISTORY (RAM only; PostgreSQL on hc-data keeps the real history) */
#define EVENTS_RING_SIZE                50
#define HISTORY_PERIOD_S                60
#define HISTORY_SAMPLES                 1440    // 24 h

/* OTA */
#define OTA_FILE                        "sensors-controller.bin"
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
