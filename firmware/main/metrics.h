#pragma once

#include <stdint.h>

/* Starts the shared MQTT client (hi_mqtt) and the metrics task.
 *
 * The topic layout and payload of heating/<mac>/temp/<sensor_id> are the contract with hc-ingest and
 * the PostgreSQL `temperature_raw` table; they must not change. password is the revealed MQTT_PASS and
 * must stay valid for the lifetime of the client.
 *
 * Call after api_start(): the retained state topic is built from the same status document the API
 * serves. */
void metrics_start(const char *password);

/* Non-blocking; dropped when the queue is full or metrics are disabled. mono_s is the monotonic time of
 * the reading (events_mono_s()); the wall clock is applied when the item is sent, so readings taken
 * before the clock is set are kept rather than thrown away. */
void metrics_post(int64_t mono_s, const char *sensor_id, float value);

/* Readings lost because the handover queue was full, as opposed to the MQTT outbox rejections that
 * hi_mqtt_stats() counts. */
uint32_t metrics_dropped(void);
