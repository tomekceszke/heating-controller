#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_task_wdt.h"

#include "owb.h"
#include "owb_rmt.h"
#include "ds18b20.h"

#include "hi_ntp.h"

#include "config/config.h"
#include "device.h"
#include "events.h"
#include "history.h"
#include "metrics.h"
#include "sensor.h"

/*
 * 1-Wire bus: esp32-owb RMT driver (DavidAntliff/esp32-owb @ 60d977e + IDF 5 fixes).
 * Deliberately NOT the bit-banged esp-idf-lib/onewire used in floor-heating-controller:
 * on this bus (7 sensors, long cables) its search found only 1 of 7 devices.
 * Replacing it is the ESP-IDF 6 work in docs/IDF6_MIGRATION.md.
 *
 * Slots are assigned on the first scan and never move: a later rescan keeps a known sensor in its
 * slot and only appends new ones, so history series stay attached to the same sensor.
 */

static const char *TAG = "SENSOR";

// Max time to wait for the 1-Wire bus; one measurement cycle holds it for ~1 s
#define BUS_LOCK_TIMEOUT_MS 5000

static OneWireBus *s_owb = NULL;
static owb_rmt_driver_info s_rmt_driver_info;
static DS18B20_Info *s_devices[MAX_DEVICES] = {0};
static char s_ids[MAX_DEVICES][SENSOR_ID_LEN];
static volatile size_t s_count = 0;
// Serializes bus access between the sensor task and the HTTP API task
static SemaphoreHandle_t s_bus_mutex = NULL;

typedef struct {
    bool valid;
    int16_t temp_x10;
    int64_t mono_s;
    uint32_t errors;
    uint16_t fails_in_row;
    bool lost;
} slot_state_t;

// Written by the sensor task, read by HTTP handlers
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static slot_state_t s_state[MAX_DEVICES];
static volatile int64_t s_last_cycle_mono_s = 0;

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

/* Slot of an id already known, or the next free one. SIZE_MAX when the table is full. */
static size_t slot_for_id(const char *id)
{
    for (size_t i = 0; i < s_count; ++i) {
        if (strcmp(s_ids[i], id) == 0) return i;
    }
    return s_count < MAX_DEVICES ? s_count : SIZE_MAX;
}

/* Searches the bus and (re)builds s_devices/s_ids. Must hold the bus mutex. Returns devices found. */
static size_t discover_locked(void)
{
    OneWireBus_ROMCode rom_codes[MAX_DEVICES] = {0};
    size_t slots[MAX_DEVICES];
    size_t found = 0;

    OneWireBus_SearchState search_state = {0};
    bool found_one = false;
    owb_status status = owb_search_first(s_owb, &search_state, &found_one);
    while (status == OWB_STATUS_OK && found_one) {
        if (found == MAX_DEVICES) {
            ESP_LOGW(TAG, "More than %d devices on the bus, ignoring the rest (MAX_DEVICES)", MAX_DEVICES);
            break;
        }
        // ROM bytes 7..0 as hex, e.g. "9b00000009029f28" — a key in temperature_raw, do not change
        char id[SENSOR_ID_LEN] = {0};
        owb_string_from_rom_code(search_state.rom_code, id, sizeof(id));
        size_t slot = slot_for_id(id);
        if (slot == SIZE_MAX) {
            ESP_LOGW(TAG, "No free slot for sensor %s (MAX_DEVICES)", id);
        } else {
            rom_codes[found] = search_state.rom_code;
            slots[found] = slot;
            if (slot == s_count) {
                snprintf(s_ids[slot], SENSOR_ID_LEN, "%s", id);
                s_count = slot + 1;
            }
            ++found;
        }
        status = owb_search_next(s_owb, &search_state, &found_one);
    }
    if (status != OWB_STATUS_OK) {
        ESP_LOGE(TAG, "Bus search failed, status %d", (int) status);
    }
    if (found == 0) return 0;

    for (size_t i = 0; i < found; ++i) {
        size_t slot = slots[i];
        if (s_devices[slot] != NULL) ds18b20_free(&s_devices[slot]);
        DS18B20_Info *info = ds18b20_malloc();
        configASSERT(info);
        if (found == 1) {
            ds18b20_init_solo(info, s_owb);
        } else {
            ds18b20_init(info, s_owb, rom_codes[i]);
        }
        ds18b20_use_crc(info, true);
        ds18b20_set_resolution(info, DS18B20_RESOLUTION);
        s_devices[slot] = info;
    }

    bool parasitic_power = false;
    ds18b20_check_for_parasite_power(s_owb, &parasitic_power);
    if (parasitic_power) {
        ESP_LOGI(TAG, "Parasitic-powered devices detected");
    }
    owb_use_parasitic_power(s_owb, parasitic_power);

    ESP_LOGI(TAG, "Sensors:");
    ESP_LOGI(TAG, "-------------------");
    for (size_t i = 0; i < s_count; ++i) {
        ESP_LOGI(TAG, "%u: %s (%s)%s", (unsigned) i, s_ids[i], sensor_label(s_ids[i]),
                 s_devices[i] == NULL ? " MISSING" : "");
    }
    ESP_LOGI(TAG, "-------------------");
    return found;
}

float read_sensor(const char *sensor_id)
{
    if (s_bus_mutex == NULL || s_count == 0) {
        ESP_LOGE(TAG, "Sensors not initialized!");
        return INVALID_TEMPERATURE_INDICATOR;
    }

    int sensor_index = -1;
    for (size_t i = 0; i < s_count; ++i) {
        if (strcmp(sensor_id, s_ids[i]) == 0 && s_devices[i] != NULL) {
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

bool sensor_alive(void)
{
    // One missed cycle is tolerated; a 1-Wire read plus the MQTT enqueue is well under a second
    return s_last_cycle_mono_s > 0 && (events_mono_s() - s_last_cycle_mono_s) < (2 * SAMPLE_PERIOD_S);
}

size_t sensor_snapshot(sensor_status_t *out, size_t max)
{
    size_t n = s_count < max ? s_count : max;
    for (size_t i = 0; i < n; ++i) {
        portENTER_CRITICAL(&s_state_mux);
        slot_state_t st = s_state[i];
        portEXIT_CRITICAL(&s_state_mux);
        snprintf(out[i].id, SENSOR_ID_LEN, "%s", s_ids[i]);
        out[i].label = sensor_label(s_ids[i]);
        out[i].meta = sensor_meta(s_ids[i]);
        out[i].display_order = sensor_display_order(s_ids[i]);
        out[i].valid = st.valid;
        out[i].temp_x10 = st.temp_x10;
        out[i].mono_s = st.mono_s;
        out[i].errors = st.errors;
        out[i].lost = st.lost;
    }
    return n;
}

/* Records one cycle's result for a slot and raises lost/recovered events. Returns the slot's
 * cumulative error count. */
static uint32_t record_read(size_t slot, bool ok, int16_t temp_x10, int64_t now)
{
    bool became_lost = false;
    bool recovered = false;
    uint32_t errors;

    portENTER_CRITICAL(&s_state_mux);
    slot_state_t *st = &s_state[slot];
    if (ok) {
        recovered = st->lost;
        st->valid = true;
        st->temp_x10 = temp_x10;
        st->mono_s = now;
        st->fails_in_row = 0;
        st->lost = false;
    } else {
        st->valid = false;
        st->errors++;
        if (st->fails_in_row < UINT16_MAX) st->fails_in_row++;
        if (!st->lost && st->fails_in_row >= SENSOR_FAIL_READS) {
            st->lost = true;
            became_lost = true;
        }
    }
    errors = st->errors;
    portEXIT_CRITICAL(&s_state_mux);

    if (became_lost || recovered) {
        event_t e = {
            .type = became_lost ? EV_SENSOR_LOST : EV_SENSOR_BACK,
            .mono_s = now,
            .has_temp = ok,
            .temp_x10 = temp_x10,
        };
        snprintf(e.detail, sizeof(e.detail), "%s", sensor_label(s_ids[slot]));
        events_publish(&e);
    }
    return errors;
}

static bool all_lost(size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        portENTER_CRITICAL(&s_state_mux);
        bool lost = s_state[i].lost;
        portEXIT_CRITICAL(&s_state_mux);
        if (!lost) return false;
    }
    return count > 0;
}

static void sensor_task(void *arg)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    s_owb = owb_rmt_initialize(&s_rmt_driver_info, GPIO_DS18B20_0, RMT_CHANNEL_1, RMT_CHANNEL_0);
    owb_use_crc(s_owb, true);

    size_t found = 0;
    TickType_t last_wake_time = xTaskGetTickCount();
    while (found == 0) {
        xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
        found = discover_locked();
        xSemaphoreGive(s_bus_mutex);
        if (found == 0) {
            ESP_LOGE(TAG, "There is no 1-Wire device available on the bus. Scanning...");
            esp_task_wdt_reset();
            xTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(TEMP_SENSOR_SCAN_RETRY_S * 1000));
        }
    }
    events_publish(&(event_t) {.type = EV_SENSORS, .mono_s = events_mono_s(),
                               .count = (uint8_t) found, .prev_count = 0});

    vTaskDelay(pdMS_TO_TICKS(2000));    // settle time after resolution setup (as in the original firmware)
    last_wake_time = xTaskGetTickCount();

    while (1) {
        esp_task_wdt_reset();
        const size_t count = s_count;
        int16_t history_row[MAX_DEVICES];
        float readings[MAX_DEVICES] = {0};
        DS18B20_ERROR errors[MAX_DEVICES];
        for (size_t i = 0; i < MAX_DEVICES; ++i) {
            errors[i] = DS18B20_ERROR_OWB;
            history_row[i] = HISTORY_NO_READING;
        }
        time_t now = time(NULL);
        int64_t mono = events_mono_s();

        if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(BUS_LOCK_TIMEOUT_MS)) == pdTRUE) {
            // One conversion for all devices, then read each scratchpad separately
            ds18b20_convert_all(s_owb);
            for (size_t i = 0; i < count; ++i) {
                if (s_devices[i] != NULL) {
                    ds18b20_wait_for_conversion(s_devices[i]);
                    break;
                }
            }
            for (size_t i = 0; i < count; ++i) {
                if (s_devices[i] == NULL) continue;
                errors[i] = ds18b20_read_temp(s_devices[i], &readings[i]);
            }
            xSemaphoreGive(s_bus_mutex);
        }

        // Readings need a valid timestamp; while WiFi is down they are buffered in the MQTT outbox
        const bool can_send = hi_ntp_synced();
        if (!can_send) {
            ESP_LOGW(TAG, "Skipping upload: time not synced");
        }

        for (size_t i = 0; i < count; ++i) {
            const bool ok = errors[i] == DS18B20_OK;
            const int16_t temp_x10 = ok ? (int16_t) lroundf(readings[i] * 10.0f) : 0;
            const uint32_t error_count = record_read(i, ok, temp_x10, mono);
            if (!ok) {
                ESP_LOGE(TAG, "%s: read failed: %s [%" PRIu32 " errors]",
                         s_ids[i], ds18b20_error_str(errors[i]), error_count);
                continue;
            }
            history_row[i] = temp_x10;
            ESP_LOGI(TAG, "%s: %.4f C [%" PRIu32 " errors]", s_ids[i], readings[i], error_count);
            if (can_send) {
                metrics_publish((uint32_t) now, s_ids[i], readings[i]);
            }
        }
        history_add(history_row, count);
        s_last_cycle_mono_s = mono;

        // Every sensor gone usually means the bus came back different: rescan, keeping known slots
        if (all_lost(count)) {
            xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
            size_t again = discover_locked();
            xSemaphoreGive(s_bus_mutex);
            if (again > 0 && again != count) {
                events_publish(&(event_t) {.type = EV_SENSORS, .mono_s = mono,
                                           .count = (uint8_t) again, .prev_count = (uint8_t) count});
            }
        }

        xTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(SAMPLE_PERIOD_S * 1000));
    }
}

void sensor_start(void)
{
    s_bus_mutex = xSemaphoreCreateMutex();
    configASSERT(s_bus_mutex);
    if (xTaskCreate(sensor_task, "sensors_monitor", 16384, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create the sensor task");
    }
}
