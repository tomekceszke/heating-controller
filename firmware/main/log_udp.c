#include <string.h>
#include <esp_log.h>
#include <lwip/sockets.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config/config.h"
#include "log_udp.h"

static int s_sock = -1;
static struct sockaddr_in s_server_addr;
static const char *TAG = "LOG_UDP";

void log_udp_init(void)
{
    if (s_sock >= 0) return;

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        esp_log_write(ESP_LOG_ERROR, TAG, "Failed to create UDP socket\n");
        return;
    }

    memset(&s_server_addr, 0, sizeof(s_server_addr));
    s_server_addr.sin_family = AF_INET;
    s_server_addr.sin_port = htons(LOG_UDP_PORT);
    if (inet_pton(AF_INET, LOG_UDP_IP, &s_server_addr.sin_addr) != 1) {
        esp_log_write(ESP_LOG_ERROR, TAG, "Invalid UDP log IP address\n");
        close(s_sock);
        s_sock = -1;
        return;
    }
}

void log_udp_send(const char *msg, size_t len)
{
    if (s_sock < 0) return;
    // Socket calls from the lwIP TCP/IP thread itself would wait on that same thread (deadlock)
    if (strcmp(pcTaskGetName(NULL), "tiT") == 0) return;
    sendto(s_sock, msg, len, 0, (struct sockaddr *)&s_server_addr, sizeof(s_server_addr));
}
