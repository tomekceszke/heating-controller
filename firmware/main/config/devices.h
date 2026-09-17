#pragma once

/*
 * One binary runs on both boards, so everything board-specific is picked at runtime by STA MAC
 * (see device.c). Sensor metadata mirrors the PostgreSQL `sensor` table on hc-data
 * (server/migrate/out/sensor.csv); the database stays the source of truth for the views, the
 * firmware needs the labels offline. Sensor ids are primary keys in `temperature_raw` with
 * history since 2023 and must never change.
 */

/* Direction on the circuit, from the DB column of the same name (POWER / BACK). */
typedef enum {
    SENSOR_DIR_NONE = 0,
    SENSOR_DIR_SUPPLY,
    SENSOR_DIR_RETURN,
} sensor_dir_t;

typedef struct {
    const char *id;                 // 16 lowercase hex chars, ROM bytes 7->0
    int display_order;              // same ordering as the DB, drives the UI
    const char *label;              // shown in lists and events
    const char *group;              // circuit the sensor belongs to, pairs a supply with its return
    const char *location;           // OUTDOOR / HEATER / GENERAL / HALLWAY / KITCHEN
    const char *system_type;        // MAIN / RADIATOR / FLOOR
    sensor_dir_t direction;
} sensor_meta_t;

/* All sensors known on either board (10 as of 2026-09-11). */
#define SENSOR_META_TABLE {                                                                                   \
    {"370000001287ce28", 10, "Heater supply",            "Heater",           "HEATER",  "MAIN",     SENSOR_DIR_SUPPLY},\
    {"5900000008f0a828", 11, "Heater return",            "Heater",           "HEATER",  "MAIN",     SENSOR_DIR_RETURN},\
    {"b700000014e83928", 21, "Radiators return",         "Radiators",        "GENERAL", "RADIATOR", SENSOR_DIR_RETURN},\
    {"2800000008efc028", 22, "Kitchen radiators supply", "Kitchen radiators","KITCHEN", "RADIATOR", SENSOR_DIR_SUPPLY},\
    {"39000000150fc228", 23, "Kitchen radiators return", "Kitchen radiators","KITCHEN", "RADIATOR", SENSOR_DIR_RETURN},\
    {"7100000008ece828", 31, "Floor supply",             "Floor",            "GENERAL", "FLOOR",    SENSOR_DIR_SUPPLY},\
    {"a60416586fb5ff28", 32, "Floor return",             "Floor",            "GENERAL", "FLOOR",    SENSOR_DIR_RETURN},\
    {"77041469f284ff28", 33, "Hallway floor supply",     "Hallway floor",    "HALLWAY", "FLOOR",    SENSOR_DIR_SUPPLY},\
    {"f10316555c4bff28", 34, "Hallway floor return",     "Hallway floor",    "HALLWAY", "FLOOR",    SENSOR_DIR_RETURN},\
    {"9b00000009029f28", 99, "Outdoor",                  "Outdoor",          "OUTDOOR", "MAIN",     SENSOR_DIR_NONE},\
}

/* The three numbers on the Live tab: one sensor, or the drop across a supply/return pair. */
typedef enum {
    HIGHLIGHT_NONE = 0,
    HIGHLIGHT_SENSOR,
    HIGHLIGHT_DELTA,
} highlight_kind_t;

typedef struct {
    highlight_kind_t kind;
    const char *caption;            // short, fits under a number
    const char *id;                 // HIGHLIGHT_SENSOR, or the first of the pair
    const char *id_b;               // HIGHLIGHT_DELTA only: subtracted from id
} highlight_t;

#define HIGHLIGHTS_COUNT 3

typedef struct {
    const char *mac;                // 12 lowercase hex chars, STA MAC
    const char *hostname;           // DHCP/WiFi hostname, also the accepted Host header
    const char *label;              // app headline and ntfy title
    const char *app_url;            // tapping a notification opens this
    highlight_t highlights[HIGHLIGHTS_COUNT];
} device_meta_t;

/* Boards in production. An unknown MAC falls back to DEVICE_FALLBACK_* and the first three
 * sensors found, ordered by display_order. */
#define DEVICE_META_TABLE {                                                             \
    {                                                                                   \
        .mac = "2462abf200dc",                                                          \
        .hostname = "h-controller-1",                                                   \
        .label = "Heating 1",                                                           \
        .app_url = "http://192.168.11.248/",                                            \
        .highlights = {                                                                 \
            {HIGHLIGHT_SENSOR, "outdoor",        "9b00000009029f28", NULL},             \
            {HIGHLIGHT_SENSOR, "heater supply",  "370000001287ce28", NULL},             \
            {HIGHLIGHT_SENSOR, "heater return",  "5900000008f0a828", NULL},             \
        },                                                                              \
    },                                                                                  \
    {                                                                                   \
        .mac = "ec626083a66c",                                                          \
        .hostname = "h-controller-2",                                                   \
        .label = "Heating 2",                                                           \
        .app_url = "http://192.168.11.249/",                                            \
        .highlights = {                                                                 \
            {HIGHLIGHT_SENSOR, "floor supply",   "7100000008ece828", NULL},             \
            {HIGHLIGHT_SENSOR, "kitchen supply", "2800000008efc028", NULL},             \
            {HIGHLIGHT_DELTA,  "floor drop",     "7100000008ece828", "a60416586fb5ff28"},\
        },                                                                              \
    },                                                                                  \
}

#define DEVICE_FALLBACK_HOSTNAME    "h-controller"
#define DEVICE_FALLBACK_LABEL       "Heating"
#define DEVICE_FALLBACK_APP_URL     ""
