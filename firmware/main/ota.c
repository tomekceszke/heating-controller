#include <esp_err.h>
#include <esp_system.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"

#include "config/config.h"
#include "ota.h"

static const char *TAG = "OTA";
extern const uint8_t server_cert_pem_start[] asm("_binary_ota_server_cert_15_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ota_server_cert_15_pem_end");

static esp_err_t remove_bin_file(void) {
    esp_http_client_config_t config = {
        .url = OTA_URL,
        .cert_pem = (char *) server_cert_pem_start,
        .method = HTTP_METHOD_DELETE,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGW(TAG, "Failed to init HTTP client for bin removal");
        return ESP_FAIL;
    }
    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    return err;
}

void ota(void)
{
    ESP_LOGI(TAG, "Starting OTA task");
    esp_http_client_config_t config = {
        .url = OTA_URL,
        .cert_pem = (char *) server_cert_pem_start,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };
    ESP_LOGI(TAG, "Attempting to download update from %s", config.url);

    esp_err_t ret = esp_https_ota(&ota_config);

    if (ret == ESP_OK) {
        if (remove_bin_file() != ESP_OK) {
            ESP_LOGW(TAG, "Failed to delete firmware file from server");
        }
        ESP_LOGI(TAG, "OTA succeeded, rebooting...");
        esp_restart();
    } else {
        ESP_LOGW(TAG, "OTA failed: %s", esp_err_to_name(ret));
    }
}
