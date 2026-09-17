#pragma once

#include <stddef.h>

void log_udp_init(void);
void log_udp_send(const char *msg, size_t len);
