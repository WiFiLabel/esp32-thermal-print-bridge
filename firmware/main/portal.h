#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Serve the Wi-Fi form on port 80. The open setup network starts only when
 * portal_note_cable(false) is in effect. Call when no credentials are stored.
 */
esp_err_t portal_arm_setup(const char *hostname);

/** True when the Ethernet link is up. Turns the setup radio off while a cable is connected. */
void portal_note_cable(bool link_up);

/** True once the station has an address. That also turns the setup radio off. */
void portal_note_sta(bool have_ip);

/** Station radio is already running. A later setup AP must not initialise Wi-Fi again. */
void portal_note_radio_started(void);

/**
 * Count a failed station join. With no cable and no station address, the fourth
 * failure opens the setup network so the password can be entered again.
 */
void portal_note_join_failed(void);

#ifdef __cplusplus
}
#endif
