#include "app_restart.h"

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static uint32_t s_restart_ms;

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(s_restart_ms));
    esp_restart();
}

esp_err_t app_schedule_restart(uint32_t delay_ms)
{
    s_restart_ms = delay_ms ? delay_ms : 1;
    if (xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
