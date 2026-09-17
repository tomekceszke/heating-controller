#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"

#include "device.h"

static const char *TAG = "DEVICE";

static const sensor_meta_t s_sensors[] = SENSOR_META_TABLE;
static const device_meta_t s_devices[] = DEVICE_META_TABLE;
static const highlight_t s_no_highlights[HIGHLIGHTS_COUNT] = {0};

static char s_mac_hex[13];
static const device_meta_t *s_self = NULL;

void device_init(void)
{
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(s_mac_hex, sizeof(s_mac_hex), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    for (size_t i = 0; i < sizeof(s_devices) / sizeof(s_devices[0]); ++i) {
        if (strcmp(s_devices[i].mac, s_mac_hex) == 0) {
            s_self = &s_devices[i];
            ESP_LOGI(TAG, "%s (%s), MAC %s", s_self->label, s_self->hostname, s_mac_hex);
            return;
        }
    }
    ESP_LOGW(TAG, "MAC %s is not in config/devices.h: running as %s", s_mac_hex, DEVICE_FALLBACK_HOSTNAME);
}

const char *device_mac_hex(void) { return s_mac_hex; }

const char *device_hostname(void) { return s_self ? s_self->hostname : DEVICE_FALLBACK_HOSTNAME; }

const char *device_label(void) { return s_self ? s_self->label : DEVICE_FALLBACK_LABEL; }

const char *device_app_url(void) { return s_self ? s_self->app_url : DEVICE_FALLBACK_APP_URL; }

const highlight_t *device_highlights(void) { return s_self ? s_self->highlights : s_no_highlights; }

const sensor_meta_t *sensor_meta(const char *id)
{
    if (id == NULL) return NULL;
    for (size_t i = 0; i < sizeof(s_sensors) / sizeof(s_sensors[0]); ++i) {
        if (strcmp(s_sensors[i].id, id) == 0) return &s_sensors[i];
    }
    return NULL;
}

const char *sensor_label(const char *id)
{
    const sensor_meta_t *meta = sensor_meta(id);
    return meta ? meta->label : (id ? id : "");
}

int sensor_display_order(const char *id)
{
    const sensor_meta_t *meta = sensor_meta(id);
    return meta ? meta->display_order : SENSOR_ORDER_UNKNOWN;
}
