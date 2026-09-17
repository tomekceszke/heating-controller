#pragma once

#include <stdbool.h>
#include <stdint.h>

/* esp-mqtt client to the hc-data broker. The topic layout and payload are the contract with
 * hc-ingest and the PostgreSQL `temperature_raw` table; they must not change. password is the
 * revealed MQTT_PASS and must stay valid for the lifetime of the client. */
void metrics_start(const char *password);

/* Queues one reading as QoS 1: it survives a WiFi or broker outage in the esp-mqtt outbox
 * (bounded by MQTT_OUTBOX_LIMIT_BYTES and CONFIG_MQTT_OUTBOX_EXPIRED_TIMEOUT_MS). Never blocks. */
void metrics_publish(uint32_t timestamp, const char *sensor_id, float value);

/* For GET /api/status. */
void metrics_state(bool *connected, int *outbox_bytes);
