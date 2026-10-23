#include "app_ota.h"
#include "c6_update.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ipp_server.h"
#include "log_tap.h"
#include "net_9100.h"
#include "net_bringup.h"
#include "nvs_flash.h"
#include "usb_printer.h"
#include "welcome.h"

static const char *TAG = "print";

void app_main(void)
{
    log_tap_start();
    ESP_LOGI(TAG, "WiFiLabel (ESP32-P4-NANO) print bridge");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* May restart once after pushing matching firmware to the C6. */
    if (c6_update_if_needed() != ESP_OK) {
        ESP_LOGW(TAG, "C6 left on its current firmware");
    }

    ESP_ERROR_CHECK(esp_netif_init());
    log_tap_net_ready();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(welcome_start());
    ESP_ERROR_CHECK(net_bringup_start());
    ESP_ERROR_CHECK(usb_printer_start());
    ESP_ERROR_CHECK(net_9100_start());
    ESP_ERROR_CHECK(ipp_server_start());
    ESP_ERROR_CHECK(app_ota_start());

    ESP_LOGI(TAG, "IPP Everywhere on :%d and raw TCP %d; hostname %s.local", IPP_PORT, NET_9100_PORT, net_hostname());

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        char ip[16] = {0};
        char iface[8] = {0};
        bool up = net_copy_ip(ip, sizeof(ip), iface, sizeof(iface));
        ESP_LOGI(TAG, "heartbeat usb=%d %s %s", (int)usb_printer_is_ready(), up ? iface : "no-ip", up ? ip : "-");
    }
}
