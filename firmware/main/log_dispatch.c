#include <stdio.h>
#include <string.h>
#include <time.h>
#include <esp_log.h>

#include "log_dispatch.h"
#include "log_udp.h"

// Slightly larger than raw buffer to accommodate datetime replacing uptime digits
#define LOG_BUF_SIZE 300

// Replaces ESP-IDF uptime counter "(NNNNN)" with "(HH:MM:SS)" once NTP is synced.
// Falls back to original string if time is not yet available.
static void replace_timestamp(const char *raw, char *out, size_t out_size)
{
    const char *open = strchr(raw, '(');
    const char *close = open ? strchr(open, ')') : NULL;

    if (!open || !close) {
        snprintf(out, out_size, "%s", raw);
        return;
    }

    time_t now = time(NULL);
    if (now < 1000000000L) {    // time not yet synced (epoch before 2001 = RTC not set)
        snprintf(out, out_size, "%s", raw);
        return;
    }

    struct tm t;
    localtime_r(&now, &t);
    char time_str[10];          // "HH:MM:SS\0"
    strftime(time_str, sizeof(time_str), "%H:%M:%S", &t);

    size_t prefix_len = (size_t)(open - raw);
    snprintf(out, out_size, "%.*s(%s)%s", (int)prefix_len, raw, time_str, close + 1);
}

static int log_dispatch_vprintf(const char *fmt, va_list args)
{
    char raw[256];
    int len = vsnprintf(raw, sizeof(raw), fmt, args);
    if (len <= 0) return len;

    char buf[LOG_BUF_SIZE];
    replace_timestamp(raw, buf, sizeof(buf));

    fputs(buf, stdout);
    log_udp_send(buf, strlen(buf));
    return len;
}

void log_dispatch_init(void)
{
    log_udp_init();
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    esp_log_set_vprintf(log_dispatch_vprintf);
}
