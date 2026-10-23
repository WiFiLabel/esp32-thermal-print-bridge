#include "log_tap.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "net_bringup.h"

static const char *TAG = "console";

#define TAP_CAP 16384
#define TAP_LINE 1024

static char s_buf[TAP_CAP];
static size_t s_w;
static size_t s_r;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev;
static SemaphoreHandle_t s_net_ready;

static void ring_write(const char *src, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        s_buf[s_w] = src[i];
        s_w++;
        if (s_w >= TAP_CAP) {
            s_w = 0;
        }
        if (s_w == s_r) {
            s_r++;
            if (s_r >= TAP_CAP) {
                s_r = 0;
            }
        }
    }
}

static void tap_write(const char *src, size_t n)
{
    if (xPortInIsrContext()) {
        taskENTER_CRITICAL_ISR(&s_mux);
        ring_write(src, n);
        taskEXIT_CRITICAL_ISR(&s_mux);
        return;
    }
    taskENTER_CRITICAL(&s_mux);
    ring_write(src, n);
    taskEXIT_CRITICAL(&s_mux);
}

static size_t tap_read(char *dst, size_t cap)
{
    size_t n = 0;
    taskENTER_CRITICAL(&s_mux);
    while (n < cap && s_r != s_w) {
        dst[n++] = s_buf[s_r++];
        if (s_r >= TAP_CAP) {
            s_r = 0;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return n;
}

static int tap_vprintf(const char *fmt, va_list ap)
{
    char stack_line[TAP_LINE];
    va_list mine;
    va_copy(mine, ap);
    int m = vsnprintf(stack_line, sizeof(stack_line), fmt, mine);
    va_end(mine);
    if (m > 0) {
        if ((size_t)m < sizeof(stack_line) || xPortInIsrContext()) {
            size_t n = (size_t)m;
            if (n >= sizeof(stack_line)) {
                n = sizeof(stack_line) - 1;
            }
            tap_write(stack_line, n);
        } else {
            char *big = malloc((size_t)m + 1);
            if (!big) {
                tap_write(stack_line, sizeof(stack_line) - 1);
            } else {
                va_list again;
                va_copy(again, ap);
                vsnprintf(big, (size_t)m + 1, fmt, again);
                va_end(again);
                tap_write(big, (size_t)m);
                free(big);
            }
        }
    }

    va_list theirs;
    va_copy(theirs, ap);
    int wrote = s_prev ? s_prev(fmt, theirs) : m;
    va_end(theirs);
    return wrote;
}

static bool client_gone(int sock)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);
    struct timeval tv = {.tv_sec = 0, .tv_usec = 0};
    int sel = select(sock + 1, &rfds, NULL, NULL, &tv);
    if (sel <= 0 || !FD_ISSET(sock, &rfds)) {
        return false;
    }
    char junk;
    int n = recv(sock, &junk, 1, MSG_DONTWAIT);
    return n == 0;
}

static void serve(int sock)
{
    const char *hello = "WiFiLabel console. This is a copy of the USB serial.\r\n";
    if (!net_send_all(sock, hello, strlen(hello))) {
        return;
    }

    char chunk[512];
    while (true) {
        size_t n = tap_read(chunk, sizeof(chunk));
        if (n == 0) {
            if (client_gone(sock)) {
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (!net_send_all(sock, chunk, n)) {
            return;
        }
    }
}

static int listen_console(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        return -1;
    }
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(LOG_TAP_PORT),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(sock, 1) != 0) {
        close(sock);
        return -1;
    }
    return sock;
}

static void tap_task(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_net_ready, portMAX_DELAY);
    int listen_sock = -1;
    while (listen_sock < 0) {
        listen_sock = listen_console();
        if (listen_sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    ESP_LOGI(TAG, "console duplicate on TCP %d", LOG_TAP_PORT);

    while (true) {
        int sock = accept(listen_sock, NULL, NULL);
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        struct timeval snd = {.tv_sec = 2, .tv_usec = 0};
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
        ESP_LOGI(TAG, "remote console connected");
        serve(sock);
        ESP_LOGI(TAG, "remote console closed");
        shutdown(sock, SHUT_RDWR);
        close(sock);
    }
}

esp_err_t log_tap_start(void)
{
    s_net_ready = xSemaphoreCreateBinary();
    if (!s_net_ready) {
        return ESP_ERR_NO_MEM;
    }
    s_prev = esp_log_set_vprintf(tap_vprintf);
    if (xTaskCreate(tap_task, "log_tap", 4096, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void log_tap_net_ready(void)
{
    if (s_net_ready) {
        xSemaphoreGive(s_net_ready);
    }
}
