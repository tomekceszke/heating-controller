#pragma once

#include "config/devices.h"

/* Resolves which board this is from its STA MAC. Call first in app_main. */
void device_init(void);

const char *device_mac_hex(void);       // 12 lowercase hex chars, also the MQTT topic segment
const char *device_hostname(void);
const char *device_label(void);
const char *device_app_url(void);

/* HIGHLIGHTS_COUNT entries. All HIGHLIGHT_NONE on a board that is not in the table; the API then
 * falls back to the first sensors found, ordered by display_order. */
const highlight_t *device_highlights(void);

/* Sensor metadata by id, NULL when the sensor is not in the table. */
const sensor_meta_t *sensor_meta(const char *id);
/* Table label, or the id itself for an unknown sensor (never NULL). */
const char *sensor_label(const char *id);
/* Table display_order, or SENSOR_ORDER_UNKNOWN so unknown sensors sort last. */
int sensor_display_order(const char *id);

#define SENSOR_ORDER_UNKNOWN 1000
