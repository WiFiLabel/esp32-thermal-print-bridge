#include "welcome.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "raster_zpl.h"
#include "net_bringup.h"
#include "usb_printer.h"
#include "wifilabel_nvs.h"

static const char *TAG = "welcome";

#define EVT_IP BIT0
#define EVT_USB BIT1
#define EVT_REFRESH BIT2

static EventGroupHandle_t s_events;
static char s_shown[16];
static bool s_done;

static bool welcome_requested(void)
{
    uint8_t flag = 0;
    return wifilabel_nvs_get_u8("welcome", &flag) == ESP_OK && flag != 0;
}

static void welcome_clear(void)
{
    if (wifilabel_nvs_erase("welcome") != ESP_OK) {
        ESP_LOGW(TAG, "could not clear the welcome request");
    }
}

static void on_ip(void)
{
    if (s_done) {
        return;
    }
    char ip[16] = {0};
    if (net_copy_ip(ip, sizeof(ip), NULL, 0)) {
        xEventGroupSetBits(s_events, EVT_IP | EVT_REFRESH);
    } else {
        xEventGroupClearBits(s_events, EVT_IP);
        xEventGroupSetBits(s_events, EVT_REFRESH);
    }
}

static void on_printer(bool ready)
{
    if (s_done) {
        return;
    }
    if (ready) {
        xEventGroupSetBits(s_events, EVT_USB | EVT_REFRESH);
    } else {
        s_shown[0] = '\0';
        xEventGroupClearBits(s_events, EVT_USB);
        xEventGroupSetBits(s_events, EVT_REFRESH);
    }
}

static bool print_welcome(void)
{
    char ip[16] = {0};
    char iface[8] = {0};
    if (!net_copy_ip(ip, sizeof(ip), iface, sizeof(iface))) {
        return false;
    }

    char zpl[512];
    int n = snprintf(zpl, sizeof(zpl),
                     "^XA\n"
                     "^PW%d\n"
                     "^LL%d\n"
                     "^MMT\n"
                     "^FO40,70^A0N,64,64^FDWiFiLabel^FS\n"
                     "^FO40,170^A0N,40,40^FD%s.local^FS\n"
                     "^FO40,250^A0N,36,36^FD%s %s^FS\n"
                     "^FO40,340^A0N,30,30^FDIPP 631    Raw 9100^FS\n"
                     "^FO40,410^A0N,28,28^FD4x6  203 dpi^FS\n"
                     "^XZ\n",
                     LABEL_203DPI_W, LABEL_203DPI_H, net_hostname(), iface, ip);
    if (n <= 0 || n >= (int)sizeof(zpl)) {
        ESP_LOGE(TAG, "welcome label truncated");
        return false;
    }

    if (usb_printer_job_begin() != ESP_OK) {
        ESP_LOGW(TAG, "welcome label waiting on another job");
        return false;
    }
    esp_err_t err = usb_printer_write((const uint8_t *)zpl, (size_t)n);
    esp_err_t end = usb_printer_job_end();
    if (err == ESP_OK) {
        err = end;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "welcome label: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "welcome label queued for %s.local %s %s", net_hostname(), iface, ip);
    return true;
}

static void welcome_task(void *arg)
{
    (void)arg;
    while (true) {
        xEventGroupWaitBits(s_events, EVT_REFRESH, pdTRUE, pdFALSE, portMAX_DELAY);
        EventBits_t bits = xEventGroupGetBits(s_events);
        if ((bits & (EVT_IP | EVT_USB)) != (EVT_IP | EVT_USB)) {
            continue;
        }
        /* Let the printer finish USB reset before the first job. */
        vTaskDelay(pdMS_TO_TICKS(400));
        bits = xEventGroupGetBits(s_events);
        if ((bits & (EVT_IP | EVT_USB)) != (EVT_IP | EVT_USB)) {
            continue;
        }
        char ip[16] = {0};
        if (!net_copy_ip(ip, sizeof(ip), NULL, 0) || strcmp(ip, s_shown) == 0) {
            continue;
        }
        if (print_welcome()) {
            snprintf(s_shown, sizeof(s_shown), "%s", ip);
            s_done = true;
            welcome_clear();
            vTaskDelete(NULL);
        } else {
            xEventGroupSetBits(s_events, EVT_REFRESH);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

esp_err_t welcome_start(void)
{
    if (!welcome_requested()) {
        ESP_LOGI(TAG, "welcome label skipped until the next setup");
        return ESP_OK;
    }
    s_events = xEventGroupCreate();
    if (!s_events) {
        return ESP_ERR_NO_MEM;
    }
    net_set_ip_cb(on_ip);
    usb_printer_set_hotplug_cb(on_printer);
    if (xTaskCreate(welcome_task, "welcome", 5120, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
