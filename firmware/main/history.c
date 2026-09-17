#include "freertos/FreeRTOS.h"

#include "config/config.h"
#include "events.h"
#include "history.h"

/* MAX_DEVICES x HISTORY_SAMPLES x 2 B = 23 KB of DRAM for 24 h of eight sensors. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int16_t s_temps[MAX_DEVICES][HISTORY_SAMPLES];
static size_t s_head;
static size_t s_count;
static int64_t s_newest_mono_s;

void history_init(void)
{
    s_head = 0;
    s_count = 0;
    s_newest_mono_s = 0;
}

void history_add(const int16_t *temps_x10, size_t count)
{
    if (count > MAX_DEVICES) count = MAX_DEVICES;
    int64_t now = events_mono_s();
    portENTER_CRITICAL(&s_mux);
    for (size_t slot = 0; slot < MAX_DEVICES; slot++) {
        s_temps[slot][s_head] = slot < count ? temps_x10[slot] : HISTORY_NO_READING;
    }
    s_head = (s_head + 1) % HISTORY_SAMPLES;
    if (s_count < HISTORY_SAMPLES) s_count++;
    s_newest_mono_s = now;
    portEXIT_CRITICAL(&s_mux);
}

size_t history_copy(size_t slot, int16_t *out, size_t max, int64_t *newest_mono_s)
{
    if (slot >= MAX_DEVICES) {
        *newest_mono_s = 0;
        return 0;
    }
    portENTER_CRITICAL(&s_mux);
    size_t n = s_count < max ? s_count : max;
    size_t start = (s_head + HISTORY_SAMPLES - n) % HISTORY_SAMPLES;
    for (size_t i = 0; i < n; i++) {
        out[i] = s_temps[slot][(start + i) % HISTORY_SAMPLES];
    }
    *newest_mono_s = s_newest_mono_s;
    portEXIT_CRITICAL(&s_mux);
    return n;
}
