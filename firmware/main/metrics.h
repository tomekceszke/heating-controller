#pragma once

#include <stdint.h>

void metrics_start(void);
void metrics_publish(uint32_t timestamp, const char *sensor_id, float value);
