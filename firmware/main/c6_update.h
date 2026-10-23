#pragma once

#include "esp_err.h"

/* If the onboard ESP32-C6 is not running esp-hosted 3.0.10, push the embedded
 * image over SDIO and restart. A matching coprocessor returns ESP_OK. */
esp_err_t c6_update_if_needed(void);
