#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_system.h>
#include "esp_http_server.h"

#include "config/config.h"
#include "config/credentials.h"
#include "api.h"
#include "ntp.h"
#include "ota.h"
#include "sensor.h"

static const char *HEADER_AUTHORIZATION_KEY = "Authorization";
static const char *TAG = "API";

static httpd_handle_t s_httpd = NULL;

static bool auth(httpd_req_t *req) {
    const size_t auth_data_len = httpd_req_get_hdr_value_len(req, HEADER_AUTHORIZATION_KEY) + 1;

    if (auth_data_len <= 1) {
        ESP_LOGW(TAG, "Authorization header not found");
        return false;
    }
    char *auth_data = malloc(auth_data_len);
    if (auth_data == NULL) return false;

    bool authorized = false;
    const esp_err_t err = httpd_req_get_hdr_value_str(req, HEADER_AUTHORIZATION_KEY, auth_data, auth_data_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Authorization invalid! %s", esp_err_to_name(err));
    } else if (HEADER_AUTHORIZATION_VALUE[0] != '\0' && strcmp(auth_data, HEADER_AUTHORIZATION_VALUE) == 0) {
        ESP_LOGD(TAG, "Authorization successful!");
        authorized = true;
    } else {
        ESP_LOGE(TAG, "Authorization invalid!");
    }
    free(auth_data);
    return authorized;
}

static esp_err_t doAuth(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Secure\"");
    httpd_resp_set_status(req, "401 Unauthorized");
    return httpd_resp_send(req, "", 0);
}

static esp_err_t su_handler(httpd_req_t *req) {
    if (!auth(req)) {
        return doAuth(req);
    }
    const char resp[] = "Upgrade in progress...";
    ota();      // reboots on success; returns only when no update was applied

    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, resp, strlen(resp));
}

static esp_err_t reboot_handler(httpd_req_t *req) {
    if (!auth(req)) {
        return doAuth(req);
    }
    ESP_LOGE(TAG, "(not error) Rebooting!");
    esp_restart();
    return ESP_OK;
}

static esp_err_t get_hw_status_handler(httpd_req_t *req) {
    size_t free_bytes = esp_get_free_heap_size();
    char data[200];
    snprintf(data, sizeof(data),
             "{\n"
             "   \"up_since\":\"%s\",\n"
             "   \"free_mem_kb\":\"%u\"\n"
             "}",
             boot_time,
             (unsigned int) (free_bytes / 1024));
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, data, strlen(data));
}

static esp_err_t read_sensor_handler(httpd_req_t *req) {
    float sensor_value = INVALID_TEMPERATURE_INDICATOR;
    const size_t buf_len = httpd_req_get_url_query_len(req) + 1;

    if (buf_len > 1) {
        char *buf = malloc(buf_len);
        ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "buffer alloc failed");
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char sensor_id[SENSOR_ID_LEN] = {0};    // e.g. 9b00000009029f28
            if (httpd_query_key_value(buf, "sensor_id", sensor_id, sizeof(sensor_id)) == ESP_OK) {
                sensor_value = read_sensor(sensor_id);
                if (sensor_value != INVALID_TEMPERATURE_INDICATOR) {
                    ESP_LOGI(TAG, "Sensor ID %s reported value %.1f", sensor_id, sensor_value);
                }
            } else {
                ESP_LOGE(TAG, "Cannot get key sensor_id");
            }
        } else {
            ESP_LOGE(TAG, "Cannot get query string");
        }
        free(buf);
    }

    char data[32];
    snprintf(data, sizeof(data), "{\"value\":%.1f}", sensor_value);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, data, strlen(data));
}

void api(void) {
    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.server_port = HTTPD_PORT;
    http_config.lru_purge_enable = true;
    http_config.recv_wait_timeout = 5;
    http_config.send_wait_timeout = 5;
    http_config.stack_size = 16384;     // /admin/su runs the HTTPS OTA in this task

    httpd_uri_t read_sensor_uri = {
        .uri = "/sensor",
        .method = HTTP_GET,
        .handler = read_sensor_handler,
        .user_ctx = NULL
    };

    httpd_uri_t su_uri = {
        .uri = "/admin/su",
        .method = HTTP_POST,
        .handler = su_handler,
        .user_ctx = NULL
    };

    httpd_uri_t reboot_uri = {
        .uri = "/admin/reboot",
        .method = HTTP_POST,
        .handler = reboot_handler,
        .user_ctx = NULL
    };

    httpd_uri_t get_hw_status_uri = {
        .uri = "/admin/hw-status",
        .method = HTTP_GET,
        .handler = get_hw_status_handler,
        .user_ctx = NULL
    };

    esp_err_t err = httpd_start(&s_httpd, &http_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return;
    }
    httpd_register_uri_handler(s_httpd, &read_sensor_uri);
    httpd_register_uri_handler(s_httpd, &su_uri);
    httpd_register_uri_handler(s_httpd, &reboot_uri);
    httpd_register_uri_handler(s_httpd, &get_hw_status_uri);
    ESP_LOGI(TAG, "HTTP server started on port: %d", http_config.server_port);
}
