#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Listen on TCP 8080 when an OTA token is configured.
 * GET and POST /update both require the header X-OTA-Token.
 * POST writes a new application bin into the idle slot and reboots into it.
 * A pending image is confirmed after it holds an address for 60 seconds.
 * With a previous slot and no address, it reboots after 3 minutes so the
 * bootloader can roll back. That window starts again each time the address
 * drops. Setup mode and a first flash do not.
 */
esp_err_t app_ota_start(void);

/** True while the running slot is waiting for confirmation. */
bool app_ota_is_pending(void);

/** Preferred address appeared (true) or went away (false). */
void app_ota_note_address(bool have);

#ifdef __cplusplus
}
#endif
