#include "usb_printer.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

static const char *TAG = "usb_printer";

static bool s_host_installed;
static bool s_printer_ready;

bool usb_printer_is_ready(void)
{
    return s_printer_ready;
}

esp_err_t usb_printer_write(const uint8_t *data, size_t len)
{
    if (data == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_printer_ready) {
        ESP_LOGW(TAG, "would send %u bytes (no USB printer attached yet)", (unsigned)len);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "would send %u bytes to printer BULK OUT (not wired yet)", (unsigned)len);
    return ESP_OK;
}

static void usb_host_lib_task(void *arg)
{
    (void)arg;

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .peripheral_map = BIT0,
    };

    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    s_host_installed = true;
    ESP_LOGI(TAG, "USB Host installed (peripheral_map=0x%x); waiting for printer class 0x07",
             (unsigned)host_config.peripheral_map);

    bool has_clients = true;
    bool has_devices = false;

    while (true) {
        uint32_t event_flags = 0;
        err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (err != ESP_OK) {
            continue;
        }

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            has_clients = false;
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            has_devices = false;
            s_printer_ready = false;
            ESP_LOGI(TAG, "all USB devices freed");
        }

        /* Printer-class client registration comes next; keep host event loop alive. */
        (void)has_clients;
        (void)has_devices;
    }
}

esp_err_t usb_printer_start(void)
{
    BaseType_t ok = xTaskCreate(usb_host_lib_task, "usb_host", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "USB printer host task started (class driver stub)");
    return ESP_OK;
}
