#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <esp_log.h>
#include "mqtt_client.h"

#include "hi_ntp.h"

#include "config/config.h"
#include "config/credentials.h"
#include "device.h"
#include "events.h"
#include "metrics.h"
#include "sensor.h"

/*
 * Publishes readings to the local Mosquitto broker (hc-data). Readings are enqueued as QoS1
 * into the esp-mqtt outbox, so they are kept across WiFi/broker outages (bounded by
 * MQTT_OUTBOX_LIMIT_BYTES and CONFIG_MQTT_OUTBOX_EXPIRED_TIMEOUT_MS) and sent on reconnect.
 *
 * The sensor task must never call in here directly: esp_mqtt_client_enqueue() takes the esp-mqtt
 * API lock with portMAX_DELAY, and the client task holds that lock across a connect attempt, so an
 * unreachable broker would block the caller for CONFIG_MQTT_NETWORK_TIMEOUT_MS. That is how
 * heating-controller-1 reset on the task watchdog on 2026-09-20. Everything crosses a queue.
 */

static const char *TAG = "METRICS";

#define TOPIC_MAX_LEN 64
#define WAIT_FOR_CLOCK_MS 1000

typedef struct {
    int64_t mono_s;
    char id[SENSOR_ID_LEN];
    float value;
} item_t;

static esp_mqtt_client_handle_t s_client = NULL;
static QueueHandle_t s_queue = NULL;
static char s_client_id[16];
static char s_status_topic[TOPIC_MAX_LEN];
static volatile bool s_connected = false;
static volatile uint32_t s_dropped = 0;

static void publish_item(const item_t *item)
{
    char topic[TOPIC_MAX_LEN];
    char payload[48];
    const int64_t ts = events_unix_time(item->mono_s);
    snprintf(topic, sizeof(topic), MQTT_TOPIC_PREFIX "/%s/temp/%s", device_mac_hex(), item->id);
    snprintf(payload, sizeof(payload), "{\"ts\":%" PRIu32 ",\"value\":%.4f}", (uint32_t) ts, item->value);

    /* This may block for the network timeout while the broker is unreachable, which is exactly why
     * it runs in this task and not in the sensor task. */
    int msg_id = esp_mqtt_client_enqueue(s_client, topic, payload, 0, 1, 0, true);
    if (msg_id == -2) {
        ESP_LOGW(TAG, "Outbox full (%d bytes), dropping %s", esp_mqtt_client_get_outbox_size(s_client), item->id);
        events_publish(&(event_t) {.type = EV_OUTBOX_FULL, .mono_s = events_mono_s()});
    } else if (msg_id < 0) {
        ESP_LOGW(TAG, "Enqueue failed, dropping %s", item->id);
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

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    const esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t) event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected, %d bytes queued", esp_mqtt_client_get_outbox_size(s_client));
        s_connected = true;
        esp_mqtt_client_enqueue(s_client, s_status_topic, "online", 0, 1, 1, true);
        events_publish(&(event_t) {.type = EV_MQTT, .mono_s = events_mono_s(), .on = true});
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_connected = false;
        events_publish(&(event_t) {.type = EV_MQTT, .mono_s = events_mono_s(), .on = false});
        break;
    case MQTT_EVENT_ERROR:
        // Warning, not error: an unreachable hc-data must not turn into ntfy error notifications
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGW(TAG, "MQTT connection refused, code %d", event->error_handle->connect_return_code);
        } else {
            ESP_LOGW(TAG, "MQTT error type %d, socket errno %d", event->error_handle->error_type,
                     event->error_handle->esp_transport_sock_errno);
        }
        break;
    default:
        break;
    }
}

void metrics_start(const char *password)
{
    snprintf(s_client_id, sizeof(s_client_id), "hc-%s", device_mac_hex());
    snprintf(s_status_topic, sizeof(s_status_topic), MQTT_TOPIC_PREFIX "/%s/status", device_mac_hex());

    const esp_mqtt_client_config_t config = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials = {
            .username = MQTT_USER,
            .client_id = s_client_id,
            .authentication.password = password,
        },
        .session.last_will = {
            .topic = s_status_topic,
            .msg = "offline",
            .qos = 1,
            .retain = 1,
        },
        .outbox.limit = MQTT_OUTBOX_LIMIT_BYTES,
        .network.reconnect_timeout_ms = 30000,
    };
    s_client = esp_mqtt_client_init(&config);
    s_queue = xQueueCreate(METRICS_QUEUE_LEN, sizeof(item_t));
    if (s_client == NULL || s_queue == NULL) {
        ESP_LOGE(TAG, "Metrics disabled: out of memory");
        if (s_queue) vQueueDelete(s_queue);
        s_queue = NULL;
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL);
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return;
    }
    if (xTaskCreate(metrics_task, "metrics", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Metrics task not started");
        return;
    }
    ESP_LOGI(TAG, "MQTT client %s started, broker %s", s_client_id, MQTT_BROKER_URI);
}

void metrics_post(int64_t mono_s, const char *sensor_id, float value)
{
    if (s_queue == NULL) return;
    item_t item = {.mono_s = mono_s, .value = value};
    snprintf(item.id, sizeof(item.id), "%s", sensor_id);
    if (xQueueSend(s_queue, &item, 0) != pdTRUE) s_dropped++;
}

void metrics_stats(metrics_stats_t *out)
{
    out->enabled = s_queue != NULL;
    out->connected = s_connected;
    out->dropped = s_dropped;
    out->outbox_bytes = s_client != NULL ? esp_mqtt_client_get_outbox_size(s_client) : 0;
}
