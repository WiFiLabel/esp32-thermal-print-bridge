#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "net_9100.h"
#include "usb_printer.h"

static const char *TAG = "print";

void app_main(void)
{
    ESP_LOGI(TAG, "WiFiLabel (ESP32-P4-NANO) print bridge");
    ESP_LOGI(TAG, "modules: USB HS printer host, TCP %d, OTA partitions", NET_9100_PORT);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Ethernet / Wi-Fi (esp_hosted C6) come next; 9100 binds and retries until netif is up. */
    ESP_ERROR_CHECK(usb_printer_start());
    ESP_ERROR_CHECK(net_9100_start());

    ESP_LOGI(TAG, "bring-up complete; connect Ethernet then run scripts/send_9100.py");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "heartbeat; usb_printer_ready=%d", (int)usb_printer_is_ready());
    }
}
