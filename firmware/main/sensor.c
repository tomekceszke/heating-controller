#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_log.h"

#include "owb.h"
#include "owb_rmt.h"
#include "ds18b20.h"

#include "config/config.h"
#include "sensor.h"
#include "metrics.h"
#include "ntp.h"

/*
 * 1-Wire bus: esp32-owb RMT driver (DavidAntliff/esp32-owb @ 60d977e + IDF 5 fixes).
 * Deliberately NOT the bit-banged esp-idf-lib/onewire used in floor-heating-controller:
 * on this bus (7 sensors, long cables) its search found only 1 of 7 devices.
 */

static const char *TAG = "SENSOR";

// Max time to wait for the 1-Wire bus; one measurement cycle holds it for ~1 s
#define BUS_LOCK_TIMEOUT_MS 5000

static OneWireBus *s_owb = NULL;
static owb_rmt_driver_info s_rmt_driver_info;
static DS18B20_Info *s_devices[MAX_DEVICES] = {0};
static char s_ids[MAX_DEVICES][SENSOR_ID_LEN];
static volatile size_t s_count = 0;
// Serializes bus access between the monitoring task and the HTTP API task
static SemaphoreHandle_t s_bus_mutex = NULL;

static const char *ds18b20_error_str(DS18B20_ERROR error)
{
    switch (error) {
        case DS18B20_OK:           return "OK";
        case DS18B20_ERROR_CRC:    return "CRC check failed";
        case DS18B20_ERROR_DEVICE: return "device error";
        case DS18B20_ERROR_OWB:    return "bus fault";
        case DS18B20_ERROR_NULL:   return "null value or parameter";
        default:                   return "unknown error";
    }
}

// Searches the bus; fills rom_codes/s_ids. Returns number of devices found (<= MAX_DEVICES).
static size_t search_devices(OneWireBus_ROMCode rom_codes[MAX_DEVICES])
{
    size_t found_count = 0;
    OneWireBus_SearchState search_state = {0};
    bool found = false;
    owb_status status = owb_search_first(s_owb, &search_state, &found);
    while (status == OWB_STATUS_OK && found) {
        if (found_count == MAX_DEVICES) {
            ESP_LOGW(TAG, "More than %d devices on the bus, ignoring the rest (MAX_DEVICES)", MAX_DEVICES);
            break;
        }
        rom_codes[found_count] = search_state.rom_code;
        // ROM bytes 7..0 as hex, e.g. "9b00000009029f28" — stored in BigQuery, do not change
        owb_string_from_rom_code(search_state.rom_code, s_ids[found_count], sizeof(s_ids[found_count]));
        ++found_count;
        status = owb_search_next(s_owb, &search_state, &found);
    }
    if (status != OWB_STATUS_OK) {
        ESP_LOGE(TAG, "Bus search failed, status %d", (int) status);
    }
    return found_count;
}

void init_sensors(void)
{
    s_bus_mutex = xSemaphoreCreateMutex();
    configASSERT(s_bus_mutex);
    xSemaphoreTake(s_bus_mutex, portMAX_DELAY);

    s_owb = owb_rmt_initialize(&s_rmt_driver_info, GPIO_DS18B20_0, RMT_CHANNEL_1, RMT_CHANNEL_0);
    owb_use_crc(s_owb, true);

    OneWireBus_ROMCode rom_codes[MAX_DEVICES] = {0};
    size_t found = search_devices(rom_codes);
    TickType_t last_wake_time = xTaskGetTickCount();
    while (found == 0) {
        ESP_LOGE(TAG, "There is no 1-Wire device available on the bus. Scanning...");
        xSemaphoreGive(s_bus_mutex);
        xTaskDelayUntil(&last_wake_time, (TEMP_SENSOR_SCAN_RETRY_S * 1000) / portTICK_PERIOD_MS);
        xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
        found = search_devices(rom_codes);
    }

    ESP_LOGI(TAG, "Sensors:");
    ESP_LOGI(TAG, "-------------------");
    for (size_t i = 0; i < found; ++i) {
        ESP_LOGI(TAG, "%u: %s", (unsigned) i, s_ids[i]);
    }
    ESP_LOGI(TAG, "-------------------");

    for (size_t i = 0; i < found; ++i) {
        DS18B20_Info *info = ds18b20_malloc();
        configASSERT(info);
        if (found == 1) {
            ds18b20_init_solo(info, s_owb);
        } else {
            ds18b20_init(info, s_owb, rom_codes[i]);
        }
        ds18b20_use_crc(info, true);
        ds18b20_set_resolution(info, DS18B20_RESOLUTION);
        s_devices[i] = info;
    }

    bool parasitic_power = false;
    ds18b20_check_for_parasite_power(s_owb, &parasitic_power);
    if (parasitic_power) {
        ESP_LOGI(TAG, "Parasitic-powered devices detected");
    }
    owb_use_parasitic_power(s_owb, parasitic_power);

    s_count = found;
    xSemaphoreGive(s_bus_mutex);

    vTaskDelay(pdMS_TO_TICKS(2000));    // settle time after resolution setup (as in the original firmware)
}

float read_sensor(const char *sensor_id)
{
    if (s_bus_mutex == NULL || s_count == 0) {
        ESP_LOGE(TAG, "Sensors not initialized!");
        return INVALID_TEMPERATURE_INDICATOR;
    }

    int sensor_index = -1;
    for (size_t i = 0; i < s_count; ++i) {
        if (strcmp(sensor_id, s_ids[i]) == 0) {
            sensor_index = (int) i;
            break;
        }
    }
    if (sensor_index == -1) {
        ESP_LOGE(TAG, "Given sensor ID %s not found!", sensor_id);
        return INVALID_TEMPERATURE_INDICATOR;
    }

    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(BUS_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "1-Wire bus busy, cannot read %s", sensor_id);
        return INVALID_TEMPERATURE_INDICATOR;
    }
    float value = INVALID_TEMPERATURE_INDICATOR;
    DS18B20_ERROR error = ds18b20_convert_and_read_temp(s_devices[sensor_index], &value);
    xSemaphoreGive(s_bus_mutex);

    if (error != DS18B20_OK) {
        ESP_LOGE(TAG, "%s: read failed: %s", sensor_id, ds18b20_error_str(error));
        return INVALID_TEMPERATURE_INDICATOR;
    }
    return value;
}

void start_monitoring(void *arg)
{
    uint32_t errors_count[MAX_DEVICES] = {0};
    TickType_t last_wake_time = xTaskGetTickCount();

    while (1) {
        const size_t count = s_count;
        float readings[MAX_DEVICES] = {0};
        DS18B20_ERROR errors[MAX_DEVICES];
        time_t now = time(NULL);

        if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(BUS_LOCK_TIMEOUT_MS)) == pdTRUE) {
            // One conversion for all devices, then read each scratchpad separately
            ds18b20_convert_all(s_owb);
            ds18b20_wait_for_conversion(s_devices[0]);
            for (size_t i = 0; i < count; ++i) {
                errors[i] = ds18b20_read_temp(s_devices[i], &readings[i]);
            }
            xSemaphoreGive(s_bus_mutex);
        } else {
            for (size_t i = 0; i < count; ++i) {
                errors[i] = DS18B20_ERROR_OWB;
            }
        }

        // Readings need a valid timestamp; while WiFi is down they are buffered in the MQTT outbox
        const bool can_send = ntp_time_synced();
        if (!can_send) {
            ESP_LOGW(TAG, "Skipping upload: time not synced");
        }

        for (size_t i = 0; i < count; ++i) {
            if (errors[i] != DS18B20_OK) {
                ++errors_count[i];
                ESP_LOGE(TAG, "%s: read failed: %s [%" PRIu32 " errors]",
                         s_ids[i], ds18b20_error_str(errors[i]), errors_count[i]);
                continue;
            }
            ESP_LOGI(TAG, "%s: %.4f C [%" PRIu32 " errors]", s_ids[i], readings[i], errors_count[i]);
            if (can_send) {
                metrics_publish((uint32_t) now, s_ids[i], readings[i]);
            }
        }
        ESP_LOGI(TAG, "Free heap: %" PRIu32 " (min %" PRIu32 "), stack left: %u",
                 esp_get_free_heap_size(), esp_get_minimum_free_heap_size(),
                 (unsigned) uxTaskGetStackHighWaterMark(NULL));

        xTaskDelayUntil(&last_wake_time, (SAMPLE_PERIOD_S * 1000) / portTICK_PERIOD_MS);
    }
}
