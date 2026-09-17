#include <time.h>
#include <esp_sntp.h>
#include <esp_log.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config/config.h"
#include "ntp.h"

static const char *TAG = "NTP";

// now < 1000000000 means before year 2001 — i.e. time not yet synced
#define NTP_SYNCED_EPOCH_MIN 1000000000L

char boot_time[64];

bool ntp_time_synced(void)
{
    return time(NULL) >= NTP_SYNCED_EPOCH_MIN;
}

void start_ntp_client(void) {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "0.pl.pool.ntp.org");
    esp_sntp_setservername(2, "1.pl.pool.ntp.org");

    esp_sntp_init();

    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    time_t now = 0;
    time(&now);

    int i = 0;
    while (now < NTP_SYNCED_EPOCH_MIN && i < NTP_MAX_ATTEMPTS) {
        ESP_LOGI(TAG, "Getting time, attempt: %d", ++i);
        vTaskDelay((NTP_RETRY_DELAY_S * 1000) / portTICK_PERIOD_MS);
        time(&now);
    }

    if (now < NTP_SYNCED_EPOCH_MIN) {
        ESP_LOGE(TAG, "Couldn't get current time by NTP. Some features won't work!");
        return;
    }

    struct tm timeinfo = {0};
    localtime_r(&now, &timeinfo);
    strftime(boot_time, sizeof(boot_time), "%c", &timeinfo);
    ESP_LOGI(TAG, "The current local date/time is: %s", boot_time);
}
