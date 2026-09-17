#pragma once

#include <stdbool.h>

extern char boot_time[64];

void start_ntp_client(void);
bool ntp_time_synced(void);
