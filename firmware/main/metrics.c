#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <esp_log.h>

#include "hi_mqtt.h"
#include "hi_ntp.h"

#include "api.h"
#include "config/config.h"
#include "config/credentials.h"
#include "device.h"
#include "events.h"
#include "metrics.h"
#include "sensor.h"

/*
 * Readings go out as QoS1 into the esp-mqtt outbox, so they survive WiFi/broker outages (bounded by
 * MQTT_OUTBOX_LIMIT_BYTES and CONFIG_MQTT_OUTBOX_EXPIRED_TIMEOUT_MS) and are sent on reconnect.
 *
 * The sensor task must never publish directly: esp_mqtt_client_enqueue() takes the esp-mqtt API lock
 * with portMAX_DELAY and the client task holds that lock across a connect attempt, so an unreachable
 * broker would block the caller for CONFIG_MQTT_NETWORK_TIMEOUT_MS. That is how heating-controller-1
 * reset on the task watchdog on 2026-09-20. Everything crosses this queue.
 */

static const char *TAG = "METRICS";

#define KIND_MAX_LEN 32
#define WAIT_FOR_CLOCK_MS 1000

typedef struct {
    int64_t mono_s;
    char id[SENSOR_ID_LEN];
    float value;
} item_t;

static QueueHandle_t s_queue = NULL;
static volatile uint32_t s_dropped = 0;

static void publish_item(const item_t *item)
{
    char kind[KIND_MAX_LEN];
    char payload[48];
    const int64_t ts = events_unix_time(item->mono_s);
    snprintf(kind, sizeof(kind), "temp/%s", item->id);
    snprintf(payload, sizeof(payload), "{\"ts\":%" PRIu32 ",\"value\":%.4f}", (uint32_t) ts, item->value);

    /* This may block for the network timeout while the broker is unreachable, which is exactly why it
     * runs in this task and not in the sensor task. */
    if (hi_mqtt_publish(kind, payload, 1, false) == -2) {
        events_publish(&(event_t) {.type = EV_OUTBOX_FULL, .mono_s = events_mono_s()});
    }
}

static void metrics_task(void *arg)
{
    item_t item;
    for (;;) {
        if (xQueuePeek(s_queue, &item, portMAX_DELAY) != pdTRUE) continue;
        // Without a wall clock the timestamps would be wrong: keep the item (the queue drops new ones when full)
        if (!hi_ntp_synced()) {
            vTaskDelay(pdMS_TO_TICKS(WAIT_FOR_CLOCK_MS));
            continue;
        }
        xQueueReceive(s_queue, &item, 0);
        publish_item(&item);
    }
}

static void on_connection(bool connected)
{
    events_publish(&(event_t) {.type = EV_MQTT, .mono_s = events_mono_s(), .on = connected});
}

void metrics_start(const char *password)
{
    s_queue = xQueueCreate(METRICS_QUEUE_LEN, sizeof(item_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "Metrics disabled: out of memory");
        return;
    }
    const esp_err_t err = hi_mqtt_start(&(hi_mqtt_config_t) {
        .broker_uri = MQTT_BROKER_URI,
        .topic_prefix = MQTT_TOPIC_PREFIX,
        .username = MQTT_USER,
        .password = password,
        .client_id_prefix = "hc",
        .outbox_limit_bytes = MQTT_OUTBOX_LIMIT_BYTES,
        .state_fn = api_status_json,
        .state_period_s = MQTT_STATE_PERIOD_S,
        .on_connection = on_connection,
    });
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Metrics disabled: MQTT client not started (%s)", esp_err_to_name(err));
        vQueueDelete(s_queue);
        s_queue = NULL;
        return;
    }
    if (xTaskCreate(metrics_task, "metrics", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Metrics task not started");
    }
}

void metrics_post(int64_t mono_s, const char *sensor_id, float value)
{
    if (s_queue == NULL) return;
    item_t item = {.mono_s = mono_s, .value = value};
    snprintf(item.id, sizeof(item.id), "%s", sensor_id);
    if (xQueueSend(s_queue, &item, 0) != pdTRUE) s_dropped++;
}

uint32_t metrics_dropped(void)
{
    return s_dropped;
}
