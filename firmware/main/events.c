#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "hi_notify.h"
#include "hi_ntp.h"

#include "config/config.h"
#include "device.h"
#include "events.h"

static const char *TAG = "EVENTS";

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static event_t s_ring[EVENTS_RING_SIZE];
static size_t s_head;       // next write position
static size_t s_count;

void events_init(void)
{
    s_head = 0;
    s_count = 0;
}

int64_t events_mono_s(void)
{
    return esp_timer_get_time() / 1000000;
}

int64_t events_unix_time(int64_t mono_s)
{
    if (!hi_ntp_synced()) return 0;
    return (int64_t) time(NULL) - (events_mono_s() - mono_s);
}

static void notify(const event_t *e)
{
    char msg[160];
    switch (e->type) {
        case EV_SENSORS:
            if (e->prev_count == 0) {
                ESP_LOGI(TAG, "%u sensor(s) on the bus", (unsigned) e->count);   // first scan, boot covers it
            } else if (e->count != e->prev_count) {
                snprintf(msg, sizeof(msg), "%s: bus rescan found %u sensor(s), was %u",
                         device_label(), (unsigned) e->count, (unsigned) e->prev_count);
                ESP_LOGE(TAG, "%s", msg);
            }
            break;
        case EV_SENSOR_LOST:
            /* Error line: hi_log forwards it to the error topic. */
            ESP_LOGE(TAG, "%s: sensor %s failed %d reads in a row", device_label(), e->detail,
                     SENSOR_FAIL_READS);
            break;
        case EV_SENSOR_BACK:
            snprintf(msg, sizeof(msg), "%s: sensor %s recovered", device_label(), e->detail);
            ESP_LOGW(TAG, "(not error) %s", msg);
            hi_notify_error(msg);
            break;
        case EV_MQTT:
            /* Ring only: a reconnect is routine and readings wait in the outbox. */
            break;
        case EV_OUTBOX_FULL:
            ESP_LOGE(TAG, "%s: MQTT outbox full, readings are being dropped", device_label());
            break;
    }
}

void events_publish(const event_t *e)
{
    portENTER_CRITICAL(&s_mux);
    s_ring[s_head] = *e;
    s_head = (s_head + 1) % EVENTS_RING_SIZE;
    if (s_count < EVENTS_RING_SIZE) s_count++;
    portEXIT_CRITICAL(&s_mux);
    notify(e);
}

size_t events_recent(event_t *out, size_t max)
{
    size_t n = 0;
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < s_count && n < max; i++) {
        out[n++] = s_ring[(s_head + EVENTS_RING_SIZE - 1 - i) % EVENTS_RING_SIZE];
    }
    portEXIT_CRITICAL(&s_mux);
    return n;
}
