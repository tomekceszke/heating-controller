#pragma once

#define SENSOR_ID_LEN 17   // 16 hex chars + NUL

void init_sensors(void);
void start_monitoring(void *arg);
float read_sensor(const char *sensor_id);
