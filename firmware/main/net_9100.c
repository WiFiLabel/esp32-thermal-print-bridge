#include "net_9100.h"

#include <errno.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "usb_printer.h"

static const char *TAG = "net_9100";

#define RX_CHUNK 4096

static void handle_client(int sock)
{
    uint8_t buf[RX_CHUNK];
    size_t total = 0;

    while (true) {
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n < 0) {
            ESP_LOGE(TAG, "recv failed: errno %d", errno);
            break;
        }
        if (n == 0) {
            ESP_LOGI(TAG, "client closed after %u bytes", (unsigned)total);
            break;
        }

        total += (size_t)n;
        ESP_LOGI(TAG, "accepted %d bytes (job total %u)", n, (unsigned)total);
        esp_err_t err = usb_printer_write(buf, (size_t)n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "usb_printer_write: %s", esp_err_to_name(err));
            break;
        }
    }
}

static void net_9100_task(void *arg)
{
    (void)arg;

    while (true) {
        struct sockaddr_in dest = {
            .sin_family = AF_INET,
            .sin_addr.s_addr = htonl(INADDR_ANY),
            .sin_port = htons(NET_9100_PORT),
        };

        int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (listen_sock < 0) {
            ESP_LOGE(TAG, "socket create failed: errno %d; retry in 2s", errno);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        int opt = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        if (bind(listen_sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
            ESP_LOGW(TAG, "bind :%d failed: errno %d (netif up yet?); retry in 2s",
                     NET_9100_PORT, errno);
            close(listen_sock);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (listen(listen_sock, 2) != 0) {
            ESP_LOGE(TAG, "listen failed: errno %d", errno);
            close(listen_sock);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        ESP_LOGI(TAG, "listening on TCP %d (JetDirect raw)", NET_9100_PORT);

        while (true) {
            struct sockaddr_in source;
            socklen_t addr_len = sizeof(source);
            int sock = accept(listen_sock, (struct sockaddr *)&source, &addr_len);
            if (sock < 0) {
                ESP_LOGE(TAG, "accept failed: errno %d", errno);
                break;
            }

            char addr_str[16];
            inet_ntoa_r(source.sin_addr, addr_str, sizeof(addr_str) - 1);
            ESP_LOGI(TAG, "connection from %s", addr_str);

            handle_client(sock);
            shutdown(sock, 0);
            close(sock);
        }

        close(listen_sock);
        ESP_LOGW(TAG, "restarting listener");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t net_9100_start(void)
{
    BaseType_t ok = xTaskCreate(net_9100_task, "net_9100", 6144, NULL, 4, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "TCP 9100 task started");
    return ESP_OK;
}
