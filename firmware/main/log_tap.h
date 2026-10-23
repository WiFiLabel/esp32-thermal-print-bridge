#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** TCP port that streams a copy of the USB serial console. */
#define LOG_TAP_PORT 2323

/**
 * Copy every console line into a ring and serve it on LOG_TAP_PORT.
 * The USB serial keeps its own copy. Safe to call at the start of app_main.
 */
esp_err_t log_tap_start(void);

/** Call after esp_netif_init(). The TCP listener stays idle until then. */
void log_tap_net_ready(void);

#ifdef __cplusplus
}
#endif
