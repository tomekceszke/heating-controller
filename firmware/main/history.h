#pragma once

#include <stdint.h>
#include <stddef.h>

#define HISTORY_NO_READING INT16_MIN

void history_init(void);

/* One column per HISTORY_PERIOD_S for the last 24 h, RAM only (lost on reboot). temps_x10 holds one
 * value per sensor slot in the order sensor.c discovered them, HISTORY_NO_READING for a failed read.
 * Called once per sampling cycle from the sensor task. */
void history_add(const int16_t *temps_x10, size_t count);

/* One sensor slot, oldest first into out[max]. Returns the count and the monotonic time of the
 * newest sample. */
size_t history_copy(size_t slot, int16_t *out, size_t max, int64_t *newest_mono_s);
