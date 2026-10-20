#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start USB HS host task (printer-class scaffold; real BULK OUT later). */
esp_err_t usb_printer_start(void);

/**
 * Queue raw bytes for the attached USB printer.
 * Stub: logs length and returns ESP_OK even when no device is present.
 */
esp_err_t usb_printer_write(const uint8_t *data, size_t len);

/** True when a USB printer-class device has been seen (stub: always false). */
bool usb_printer_is_ready(void);

#ifdef __cplusplus
}
#endif
