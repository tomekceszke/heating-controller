#include <esp_log.h>
#include <esp_system.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "api.h"
#include "log_dispatch.h"
#include "metrics.h"
#include "ntp.h"
#include "ota.h"
#include "sensor.h"
#include "wifi.h"

static const char *TAG = "MAIN";

void app_main(void) {
    ESP_LOGI(TAG, "\n\n");
    ESP_LOGI(TAG, "Connecting to AP...");
    wifi();
    ESP_LOGI(TAG, "Init log dispatch...");
    log_dispatch_init();
    ESP_LOGI(TAG, "Checking OTA...");
    ota();
    ESP_LOGI(TAG, "Starting API server...");
    api();
    ESP_LOGI(TAG, "Setting time...");
    start_ntp_client();
    ESP_LOGI(TAG, "Starting metrics publisher...");
    metrics_start();
    ESP_LOGI(TAG, "Init sensors...");
    init_sensors();
    ESP_LOGI(TAG, "Creating temperature monitoring task...");
    if (xTaskCreate(start_monitoring, "sensors_monitor", 16384, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create monitoring task, rebooting...");
        esp_restart();
    }
    ESP_LOGE(TAG, "(not error) All done! Built: %s %s Free heap size: %zu", __DATE__, __TIME__,
             xPortGetFreeHeapSize());
}
