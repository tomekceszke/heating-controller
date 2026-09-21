#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"

#include "hi_auth.h"
#include "hi_httpd.h"
#include "hi_mqtt.h"

#include "api.h"
#include "config/config.h"
#include "device.h"
#include "events.h"
#include "history.h"
#include "metrics.h"
#include "sensor.h"

static const char *TAG = "API";

extern const uint8_t login_html_gz_start[] asm("_binary_login_html_gz_start");
extern const uint8_t login_html_gz_end[] asm("_binary_login_html_gz_end");
extern const uint8_t app_html_gz_start[] asm("_binary_app_html_gz_start");
extern const uint8_t app_html_gz_end[] asm("_binary_app_html_gz_end");
extern const uint8_t icon_png_start[] asm("_binary_apple_touch_icon_png_start");
extern const uint8_t icon_png_end[] asm("_binary_apple_touch_icon_png_end");
extern const char manifest_start[] asm("_binary_manifest_webmanifest_start");

static double celsius(int16_t x10)
{
    return x10 / 10.0;
}

static const char *direction_name(const sensor_meta_t *meta)
{
    if (meta == NULL) return "";
    return meta->direction == SENSOR_DIR_SUPPLY ? "supply" : meta->direction == SENSOR_DIR_RETURN ? "return" : "";
}

/* Slot of a sensor id in the snapshot, or -1. */
static int find_slot(const sensor_status_t *list, size_t n, const char *id)
{
    if (id == NULL) return -1;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(list[i].id, id) == 0) return (int) i;
    }
    return -1;
}

/* Slots ordered by display_order, for the fallback highlights of an unknown board. */
static void sorted_slots(const sensor_status_t *list, size_t n, int *out)
{
    for (size_t i = 0; i < n; i++) out[i] = (int) i;
    for (size_t i = 1; i < n; i++) {                    // insertion sort, at most MAX_DEVICES entries
        int v = out[i];
        size_t j = i;
        while (j > 0 && list[out[j - 1]].display_order > list[v].display_order) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = v;
    }
}

/* The three numbers on the Live tab. A board that is not in config/devices.h gets the first three
 * sensors found, ordered by display_order. */
static void add_highlights(cJSON *root, const sensor_status_t *list, size_t n, int64_t now)
{
    cJSON *arr = cJSON_AddArrayToObject(root, "highlights");
    const highlight_t *spec = device_highlights();
    int by_order[MAX_DEVICES];
    sorted_slots(list, n, by_order);

    for (size_t h = 0; h < HIGHLIGHTS_COUNT; h++) {
        cJSON *o = cJSON_CreateObject();
        highlight_kind_t kind = spec[h].kind;
        const char *caption = spec[h].caption;
        int a = -1;
        int b = -1;

        if (kind == HIGHLIGHT_NONE) {
            kind = HIGHLIGHT_SENSOR;
            a = h < n ? by_order[h] : -1;
            caption = a >= 0 ? list[a].label : "";
        } else {
            a = find_slot(list, n, spec[h].id);
            if (kind == HIGHLIGHT_DELTA) b = find_slot(list, n, spec[h].id_b);
        }

        const bool have = kind == HIGHLIGHT_DELTA
                          ? (a >= 0 && b >= 0 && list[a].valid && list[b].valid)
                          : (a >= 0 && list[a].valid);
        cJSON_AddStringToObject(o, "caption", caption ? caption : "");
        cJSON_AddStringToObject(o, "unit", kind == HIGHLIGHT_DELTA ? "K" : "°C");
        if (have) {
            cJSON_AddNumberToObject(o, "value", kind == HIGHLIGHT_DELTA
                                                ? celsius((int16_t) (list[a].temp_x10 - list[b].temp_x10))
                                                : celsius(list[a].temp_x10));
            /* Age of the oldest reading the number is built from. */
            int64_t oldest = list[a].mono_s;
            if (kind == HIGHLIGHT_DELTA && list[b].mono_s < oldest) oldest = list[b].mono_s;
            cJSON_AddNumberToObject(o, "age_s", (double) (now - oldest));
        } else {
            cJSON_AddNullToObject(o, "value");
            cJSON_AddNumberToObject(o, "age_s", -1);
        }
        cJSON_AddItemToArray(arr, o);
    }
}

cJSON *api_status_json(void)
{
    /* On the stack, not static: the hi_mqtt state task builds this document too, concurrently with a
     * request being served. */
    sensor_status_t list[MAX_DEVICES];
    const size_t n = sensor_snapshot(list, MAX_DEVICES);
    const int64_t now = events_mono_s();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device", device_label());
    cJSON_AddStringToObject(root, "mac", device_mac_hex());
    cJSON_AddBoolToObject(root, "sensors_alive", sensor_alive());

    cJSON *arr = cJSON_AddArrayToObject(root, "sensors");
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", list[i].id);
        cJSON_AddStringToObject(o, "label", list[i].label);
        cJSON_AddStringToObject(o, "group", list[i].meta ? list[i].meta->group : "Other");
        cJSON_AddStringToObject(o, "dir", direction_name(list[i].meta));
        cJSON_AddNumberToObject(o, "order", list[i].display_order);
        if (list[i].valid) cJSON_AddNumberToObject(o, "c", celsius(list[i].temp_x10));
        else cJSON_AddNullToObject(o, "c");
        cJSON_AddNumberToObject(o, "age_s", list[i].mono_s > 0 ? (double) (now - list[i].mono_s) : -1);
        cJSON_AddNumberToObject(o, "errors", list[i].errors);
        cJSON_AddBoolToObject(o, "lost", list[i].lost);
        cJSON_AddItemToArray(arr, o);
    }
    add_highlights(root, list, n, now);

    hi_mqtt_stats_t m;
    hi_mqtt_stats(&m);
    cJSON *mqtt = cJSON_AddObjectToObject(root, "mqtt");
    cJSON_AddBoolToObject(mqtt, "connected", m.connected);
    cJSON_AddBoolToObject(mqtt, "enabled", m.enabled);
    cJSON_AddNumberToObject(mqtt, "dropped", m.dropped);
    cJSON_AddNumberToObject(mqtt, "queue_dropped", metrics_dropped());
    cJSON_AddNumberToObject(mqtt, "outbox_bytes", m.outbox_bytes);
    cJSON_AddNumberToObject(mqtt, "outbox_limit_bytes", MQTT_OUTBOX_LIMIT_BYTES);
    cJSON_AddStringToObject(mqtt, "broker", MQTT_BROKER_URI);

    cJSON_AddNumberToObject(root, "sample_period_s", SAMPLE_PERIOD_S);
    hi_httpd_add_system_status(cJSON_AddObjectToObject(root, "system"));
    return root;
}

static bool admin_ok(httpd_req_t *req, esp_err_t *result)
{
    if (hi_auth_admin_header_valid(req)) return true;
    httpd_resp_set_status(req, "401 Unauthorized");
    *result = httpd_resp_send(req, "", 0);
    return false;
}

/* Reading also accepts the read-only value, so a permanent subscriber never needs the admin secret
 * that carries OTA and reboot with it. */
static bool admin_read_ok(httpd_req_t *req, esp_err_t *result)
{
    if (hi_auth_readonly_header_valid(req)) return true;
    httpd_resp_set_status(req, "401 Unauthorized");
    *result = httpd_resp_send(req, "", 0);
    return false;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    esp_err_t result;
    if (!hi_httpd_guard(req, HI_GUARD_SESSION, NULL, &result)) return result;
    return hi_httpd_send_json(req, "200 OK", api_status_json());
}

static esp_err_t admin_status_handler(httpd_req_t *req)
{
    esp_err_t result;
    if (!admin_read_ok(req, &result)) return result;
    return hi_httpd_send_json(req, "200 OK", api_status_json());
}

static esp_err_t events_handler(httpd_req_t *req)
{
    esp_err_t result;
    if (!hi_httpd_guard(req, HI_GUARD_SESSION, NULL, &result)) return result;
    static event_t list[EVENTS_RING_SIZE];      // httpd runs one handler at a time
    size_t n = events_recent(list, EVENTS_RING_SIZE);

    static const char *const TYPES[] = {"sensors", "sensor_lost", "sensor_back", "mqtt", "outbox_full"};
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "events");
    for (size_t i = 0; i < n; i++) {
        const event_t *e = &list[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "ts", (double) events_unix_time(e->mono_s));
        cJSON_AddStringToObject(o, "type", TYPES[e->type]);
        cJSON_AddBoolToObject(o, "on", e->on);
        cJSON_AddNumberToObject(o, "count", e->count);
        cJSON_AddNumberToObject(o, "prev_count", e->prev_count);
        if (e->has_temp) cJSON_AddNumberToObject(o, "temp_c", celsius(e->temp_x10));
        else cJSON_AddNullToObject(o, "temp_c");
        cJSON_AddStringToObject(o, "detail", e->detail);
        cJSON_AddItemToArray(arr, o);
    }
    return hi_httpd_send_json(req, "200 OK", root);
}

/* {"period_s":60,"newest":<unix or 0>,"newest_age_s":12,
 *  "series":[{"id":"...","label":"...","t":[312,null,...]}]}, oldest first, streamed. */
static esp_err_t history_handler(httpd_req_t *req)
{
    esp_err_t result;
    if (!hi_httpd_guard(req, HI_GUARD_SESSION, NULL, &result)) return result;
    static sensor_status_t list[MAX_DEVICES];
    static int16_t temps[HISTORY_SAMPLES];
    const size_t sensors = sensor_snapshot(list, MAX_DEVICES);

    int64_t newest_mono_s = 0;
    size_t n = sensors ? history_copy(0, temps, HISTORY_SAMPLES, &newest_mono_s) : 0;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char buf[512];
    int len = snprintf(buf, sizeof(buf), "{\"period_s\":%d,\"newest\":%lld,\"newest_age_s\":%lld,\"series\":[",
                       HISTORY_PERIOD_S, (long long) (n ? events_unix_time(newest_mono_s) : 0),
                       (long long) (n ? events_mono_s() - newest_mono_s : 0));

    for (size_t s = 0; s < sensors; s++) {
        n = history_copy(s, temps, HISTORY_SAMPLES, &newest_mono_s);
        len += snprintf(buf + len, sizeof(buf) - len, "%s{\"id\":\"%s\",\"label\":\"%s\",\"t\":[",
                        s ? "," : "", list[s].id, list[s].label);
        for (size_t i = 0; i < n; i++) {
            if (len > (int) sizeof(buf) - 16) {
                if (httpd_resp_send_chunk(req, buf, len) != ESP_OK) return ESP_FAIL;
                len = 0;
            }
            const char *sep = i ? "," : "";
            len += temps[i] == HISTORY_NO_READING ? snprintf(buf + len, sizeof(buf) - len, "%snull", sep)
                                                  : snprintf(buf + len, sizeof(buf) - len, "%s%d", sep, temps[i]);
        }
        len += snprintf(buf + len, sizeof(buf) - len, "]}");
    }
    len += snprintf(buf + len, sizeof(buf) - len, "]}");
    if (httpd_resp_send_chunk(req, buf, len) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

/*
 * GET /sensor?sensor_id=<id> -> {"value":21.5}, INVALID_TEMPERATURE_INDICATOR on any failure.
 * Unauthenticated and unchanged since the legacy firmware: ~/dev/ulanzi_tc001 polls the outdoor
 * sensor through it. The old Access-Control-Allow-Origin header is gone with home-idf's no-CORS
 * policy; the display is not a browser and sends no Origin.
 */
static esp_err_t read_sensor_handler(httpd_req_t *req)
{
    esp_err_t result;
    if (!hi_httpd_guard(req, HI_GUARD_PUBLIC, NULL, &result)) return result;

    float sensor_value = INVALID_TEMPERATURE_INDICATOR;
    char query[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char sensor_id[SENSOR_ID_LEN] = {0};    // e.g. 9b00000009029f28
        if (httpd_query_key_value(query, "sensor_id", sensor_id, sizeof(sensor_id)) == ESP_OK) {
            sensor_value = read_sensor(sensor_id);
            if (sensor_value != INVALID_TEMPERATURE_INDICATOR) {
                ESP_LOGI(TAG, "Sensor ID %s reported value %.1f", sensor_id, sensor_value);
            }
        } else {
            ESP_LOGE(TAG, "Cannot get key sensor_id");
        }
    }

    char data[32];
    int len = snprintf(data, sizeof(data), "{\"value\":%.1f}", sensor_value);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, data, len);
}

void api_start(void)
{
    /* The hostname is per board (config/devices.h), so the Host allowlist is built at runtime. */
    static char host[32];
    static char host_lan[40];
    static const char *hosts[2];
    snprintf(host, sizeof(host), "%s", device_hostname());
    snprintf(host_lan, sizeof(host_lan), "%s.lan", device_hostname());
    hosts[0] = host;
    hosts[1] = host_lan;

    static hi_httpd_ui_t ui;
    ui = (hi_httpd_ui_t) {
        .login_html_gz = {login_html_gz_start, login_html_gz_end},
        .app_html_gz = {app_html_gz_start, app_html_gz_end},
        .icon_png = {icon_png_start, icon_png_end},
        .manifest_json = manifest_start,
    };
    if (hi_httpd_start(&(hi_httpd_config_t) {
            .port = HTTPD_PORT,
            .allowed_hosts = hosts,
            .allowed_hosts_count = sizeof(hosts) / sizeof(hosts[0]),
            .ui = &ui,
            .max_uri_handlers = 20,
            .stack_size = 16384,        // /admin/su runs the HTTPS OTA in this task
        }) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server not started (sensor sampling unaffected)");
        return;
    }
    const httpd_uri_t routes[] = {
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/events", .method = HTTP_GET, .handler = events_handler},
        {.uri = "/api/history", .method = HTTP_GET, .handler = history_handler},
        {.uri = "/admin/status", .method = HTTP_GET, .handler = admin_status_handler},
        {.uri = "/sensor", .method = HTTP_GET, .handler = read_sensor_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        hi_httpd_register(&routes[i]);
    }
}
