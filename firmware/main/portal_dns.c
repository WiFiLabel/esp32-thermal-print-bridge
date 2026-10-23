#include "portal_dns.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "portal_dns";

static TaskHandle_t s_task;
static volatile int s_sock = -1;
static volatile bool s_run;
static uint32_t s_ip;

/* Answer every query with an A record for the setup address, whatever type was asked. */
static int build_reply(uint8_t *pkt, int len, uint32_t ip_nbo)
{
    if (len < 12 || len > 512) {
        return -1;
    }
    if (((pkt[2] << 8) | pkt[3]) & 0x7800) {
        return 0;
    }
    uint16_t qd = (uint16_t)((pkt[4] << 8) | pkt[5]);
    if (qd < 1) {
        return 0;
    }
    int i = 12;
    while (i < len && pkt[i] != 0) {
        int lab = pkt[i];
        if ((lab & 0xC0) == 0xC0) {
            return 0;
        }
        i += lab + 1;
        if (i > len) {
            return -1;
        }
    }
    if (i >= len) {
        return -1;
    }
    i++;
    if (i + 4 > len) {
        return -1;
    }
    i += 4;

    pkt[2] |= 0x80;
    pkt[6] = 0;
    pkt[7] = 0;
    pkt[8] = pkt[9] = pkt[10] = pkt[11] = 0;
    if (i + 16 > 512) {
        return i;
    }
    pkt[7] = 1;
    pkt[i++] = 0xC0;
    pkt[i++] = 0x0C;
    pkt[i++] = 0x00;
    pkt[i++] = 0x01;
    pkt[i++] = 0x00;
    pkt[i++] = 0x01;
    pkt[i++] = 0x00;
    pkt[i++] = 0x00;
    pkt[i++] = 0x00;
    pkt[i++] = 60;
    pkt[i++] = 0x00;
    pkt[i++] = 0x04;
    memcpy(pkt + i, &ip_nbo, 4);
    i += 4;
    return i;
}

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    s_sock = sock;
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = s_ip,
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    s_run = true;
    ESP_LOGI(TAG, "DNS answers every query with the setup address");
    uint8_t pkt[512];
    while (s_run) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &fromlen);
        if (!s_run) {
            break;
        }
        if (n <= 0) {
            continue;
        }
        int out = build_reply(pkt, n, s_ip);
        if (out > 0) {
            sendto(sock, pkt, out, 0, (struct sockaddr *)&from, fromlen);
        }
    }
    s_sock = -1;
    close(sock);
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t portal_dns_start(uint32_t ip_nbo)
{
    if (s_task) {
        return ESP_OK;
    }
    s_ip = ip_nbo;
    s_run = true;
    if (xTaskCreate(dns_task, "dns", 4096, NULL, 5, &s_task) != pdPASS) {
        s_run = false;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void portal_dns_stop(void)
{
    s_run = false;
    int sock = s_sock;
    if (sock >= 0) {
        shutdown(sock, SHUT_RDWR);
    }
    for (int i = 0; i < 20 && s_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
