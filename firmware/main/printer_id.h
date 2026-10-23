#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** IEEE 1284 identity of the claimed printer. Strings are empty when unknown. */
typedef struct {
    bool present;
    bool zpl;
    char device_id[256];
    char mfg[48];
    char mdl[64];
    char cmd[96];
    char firmware[32];
} usb_printer_identity_t;

/** Replace bytes outside ASCII 32..126 with '?' so field separators stay put. */
void printer_id_scrub(char *s);

/**
 * Fill mfg, mdl, cmd, zpl, and present from device_id.
 * device_id must already be NUL-terminated. firmware is cleared.
 */
void printer_id_parse(usb_printer_identity_t *id);

/** Second comma field of a ~HI reply, or empty when the reply is not that shape. */
void printer_id_take_firmware(const uint8_t *buf, int n, char *fw, size_t fw_len);

/**
 * Model, else manufacturer, else the literal "WiFiLabel".
 * from_printer is true when the result points at the model or manufacturer.
 */
const char *printer_id_name_base(const usb_printer_identity_t *id, bool *from_printer);

/**
 * "WiFiLabel-B448" plus the USB model when model is non-empty.
 * The result is at most 63 bytes, which is the mDNS instance limit.
 */
void printer_id_format_name(char *dst, size_t dst_len, const char *brand, const char *model);

#ifdef __cplusplus
}
#endif
