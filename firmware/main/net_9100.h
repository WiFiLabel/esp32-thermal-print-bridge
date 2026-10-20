#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** JetDirect-style raw TCP listener (default port 9100). */
#define NET_9100_PORT 9100

/**
 * Start the TCP 9100 accept loop.
 * Binds INADDR_ANY; useful once Ethernet or Wi-Fi brings a netif up.
 * Received bytes are forwarded to usb_printer_write().
 */
esp_err_t net_9100_start(void);

#ifdef __cplusplus
}
#endif
