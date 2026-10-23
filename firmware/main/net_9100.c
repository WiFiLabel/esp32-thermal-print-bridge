#include "net_9100.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "net_bringup.h"
#include "usb_printer.h"

static const char *TAG = "net_9100";

#define RX_CHUNK 4096
#define IN_CHUNK 2048

static uint8_t s_rx[RX_CHUNK];
static uint8_t s_from_printer[IN_CHUNK];

/* After the client pauses, read whatever the printer sends and write it back. */
static void pass_printer_reply(int sock)
{
    if (usb_printer_flush(5000) != ESP_OK) {
        ESP_LOGW(TAG, "OUT not flushed before reading the printer");
    }
    bool any = false;
    for (int i = 0; i < 6; i++) {
        int n = usb_printer_read(s_from_printer, sizeof(s_from_printer), any ? 300 : 1000);
        if (n < 0) {
            ESP_LOGW(TAG, "printer IN failed");
            return;
        }
        if (n == 0) {
            return;
        }
        any = true;
        char preview[121];
        int shown = n > 100 ? 100 : n;
        int w = 0;
        for (int i = 0; i < shown && w < (int)sizeof(preview) - 1; i++) {
            unsigned char c = s_from_printer[i];
            if (c == '\r' || c == '\n' || c == '\t') {
                preview[w++] = ' ';
            } else if (c >= 32 && c < 127) {
                preview[w++] = (char)c;
            } else {
                preview[w++] = '.';
            }
        }
        preview[w] = '\0';
        ESP_LOGI(TAG, "passed %d bytes from printer: %s", n, preview);
        if (!net_send_all(sock, s_from_printer, (size_t)n)) {
            return;
        }
    }
}

static void handle_client(int sock)
{
    size_t total = 0;
    bool pending = false;
    TickType_t idle_from = xTaskGetTickCount();

    net_tune_accepted(sock, 20000);
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }
    if (usb_printer_job_begin() != ESP_OK) {
        ESP_LOGE(TAG, "printer busy");
        return;
    }

    while ((xTaskGetTickCount() - idle_from) < pdMS_TO_TICKS(20000)) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        struct timeval tv = {.tv_sec = 0, .tv_usec = 150000};
        int sel = select(sock + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            ESP_LOGE(TAG, "select failed: errno %d", errno);
            break;
        }
        if (sel > 0 && FD_ISSET(sock, &rfds)) {
            int n = recv(sock, s_rx, sizeof(s_rx), 0);
            if (n == 0) {
                ESP_LOGI(TAG, "client closed after %u bytes", (unsigned)total);
                break;
            }
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                ESP_LOGE(TAG, "recv failed: errno %d", errno);
                break;
            }
            total += (size_t)n;
            ESP_LOGI(TAG, "accepted %d bytes (job total %u)", n, (unsigned)total);
            if (usb_printer_write(s_rx, (size_t)n) != ESP_OK) {
                ESP_LOGE(TAG, "usb_printer_write failed");
                break;
            }
            pending = true;
            idle_from = xTaskGetTickCount();
            continue;
        }

        if (pending) {
            pass_printer_reply(sock);
            pending = false;
            idle_from = xTaskGetTickCount();
        }
    }

    esp_err_t end = usb_printer_job_end();
    if (end != ESP_OK) {
        ESP_LOGW(TAG, "job end: %s", esp_err_to_name(end));
    }
}

static void net_9100_task(void *arg)
{
    (void)arg;

    while (true) {
        int listen_sock = net_listen_tcp(NET_9100_PORT, 2);
        if (listen_sock < 0) {
            ESP_LOGW(TAG, "bind :%d failed: errno %d (netif up yet?); retry in 2s", NET_9100_PORT, errno);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        ESP_LOGI(TAG, "listening on TCP %d (raw, printer replies passed back)", NET_9100_PORT);

        while (true) {
            struct sockaddr_storage source;
            socklen_t addr_len = sizeof(source);
            int sock = accept(listen_sock, (struct sockaddr *)&source, &addr_len);
            if (sock < 0) {
                ESP_LOGE(TAG, "accept failed: errno %d", errno);
                break;
            }

            char addr_str[48] = {0};
            if (source.ss_family == AF_INET) {
                inet_ntoa_r(((struct sockaddr_in *)&source)->sin_addr, addr_str, sizeof(addr_str));
            }
#if CONFIG_LWIP_IPV6
            else if (source.ss_family == AF_INET6) {
                inet6_ntoa_r(((struct sockaddr_in6 *)&source)->sin6_addr, addr_str, sizeof(addr_str));
            }
#endif
            ESP_LOGI(TAG, "connection from %s", addr_str[0] ? addr_str : "?");

            handle_client(sock);
            shutdown(sock, SHUT_RDWR);
            close(sock);
        }

        close(listen_sock);
        ESP_LOGW(TAG, "restarting listener");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t net_9100_start(void)
{
    BaseType_t ok = xTaskCreate(net_9100_task, "net_9100", 8192, NULL, 4, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "TCP 9100 task started");
    return ESP_OK;
}
