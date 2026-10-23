#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "printer_id.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Called from the USB task when a printer-class device is claimed or released. */
typedef void (*usb_printer_hotplug_cb_t)(bool ready);

/**
 * Start the USB host, the printer-class client, and the bulk-OUT queue.
 * peripheral 0 is the high-speed OTG port wired to the USB-A connector.
 */
esp_err_t usb_printer_start(void);

/** Replace the first hotplug listener. */
void usb_printer_set_hotplug_cb(usb_printer_hotplug_cb_t cb);

/** Register another hotplug listener. The first slot is the one set_hotplug_cb writes. */
void usb_printer_add_hotplug_cb(usb_printer_hotplug_cb_t cb);

/**
 * Copy the claimed printer's 1284 identity.
 * Returns false when no printer identity is stored.
 */
bool usb_printer_copy_identity(usb_printer_identity_t *out);

/**
 * Reserve the printer until usb_printer_job_end().
 * IPP, raw port 9100, and the welcome label take this so their bytes are not mixed.
 */
esp_err_t usb_printer_job_begin(void);

/**
 * Drain queued bytes, then release the printer.
 * A zero-length packet is sent only when the finished job's last OUT was a full max packet.
 */
esp_err_t usb_printer_job_end(void);

/**
 * Queue raw bytes for the attached USB printer (class 0x07 bulk OUT).
 * Call between usb_printer_job_begin() and usb_printer_job_end().
 * Blocks while the PSRAM ring is full. Returns ESP_ERR_INVALID_STATE when
 * no printer is claimed.
 */
esp_err_t usb_printer_write(const uint8_t *data, size_t len);

/**
 * Wait until queued OUT bytes have been sent.
 * Call between job_begin and job_end, before reading a reply.
 */
esp_err_t usb_printer_flush(uint32_t timeout_ms);

/**
 * Read one bulk-IN reply from the printer into dst.
 * Returns the byte count, 0 when the printer sends nothing before timeout_ms,
 * and -1 when the printer is gone or the IN endpoint stalls.
 */
int usb_printer_read(uint8_t *dst, size_t dst_len, uint32_t timeout_ms);

/** True after a printer-class interface has been claimed. */
bool usb_printer_is_ready(void);

#ifdef __cplusplus
}
#endif
