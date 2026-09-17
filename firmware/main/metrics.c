#include <inttypes.h>
#include <stdio.h>
#include <esp_log.h>
#include "mqtt_client.h"

#include "config/config.h"
#include "config/credentials.h"
#include "device.h"
#include "events.h"
#include "metrics.h"

/*
 * Publishes readings to the local Mosquitto broker (hc-data). Readings are enqueued as QoS1
 * into the esp-mqtt outbox, so they are kept across WiFi/broker outages (bounded by
 * MQTT_OUTBOX_LIMIT_BYTES and CONFIG_MQTT_OUTBOX_EXPIRED_TIMEOUT_MS) and sent on reconnect.
 */

static const char *TAG = "METRICS";

#define TOPIC_MAX_LEN 64

static esp_mqtt_client_handle_t s_client = NULL;
static char s_client_id[16];
static char s_status_topic[TOPIC_MAX_LEN];
static volatile bool s_connected = false;

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
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGE(TAG, "MQTT connection refused, code %d", event->error_handle->connect_return_code);
        } else {
            ESP_LOGE(TAG, "MQTT error type %d, socket errno %d", event->error_handle->error_type,
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
    };
    s_client = esp_mqtt_client_init(&config);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "Failed to init MQTT client");
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
    ESP_LOGI(TAG, "MQTT client %s started, broker %s", s_client_id, MQTT_BROKER_URI);
}

void metrics_publish(uint32_t timestamp, const char *sensor_id, float value)
{
    if (s_client == NULL) {
        ESP_LOGE(TAG, "MQTT client not started, dropping %s", sensor_id);
        return;
    }
    char topic[TOPIC_MAX_LEN];
    char payload[48];
    snprintf(topic, sizeof(topic), MQTT_TOPIC_PREFIX "/%s/temp/%s", device_mac_hex(), sensor_id);
    snprintf(payload, sizeof(payload), "{\"ts\":%" PRIu32 ",\"value\":%.4f}", timestamp, value);

    int msg_id = esp_mqtt_client_enqueue(s_client, topic, payload, 0, 1, 0, true);
    if (msg_id == -2) {
        ESP_LOGW(TAG, "Outbox full (%d bytes), dropping %s", esp_mqtt_client_get_outbox_size(s_client), sensor_id);
        events_publish(&(event_t) {.type = EV_OUTBOX_FULL, .mono_s = events_mono_s()});
    } else if (msg_id < 0) {
        ESP_LOGE(TAG, "Enqueue failed, dropping %s", sensor_id);
    }
}

void metrics_state(bool *connected, int *outbox_bytes)
{
    *connected = s_connected;
    *outbox_bytes = s_client != NULL ? esp_mqtt_client_get_outbox_size(s_client) : 0;
}
