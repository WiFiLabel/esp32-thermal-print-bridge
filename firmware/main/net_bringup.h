#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Called when the preferred IPv4 address appears, changes, or goes away. */
typedef void (*net_ip_cb_t)(void);

/**
 * Bring up Ethernet (preferred) and Wi-Fi (fallback), then advertise mDNS.
 * Hostname is wifilabel-XXXX from the MAC. Does not abort the app if one
 * interface fails to start.
 */
esp_err_t net_bringup_start(void);

void net_set_ip_cb(net_ip_cb_t cb);

/** Hostname without the .local suffix. Valid after net_bringup_start(). */
const char *net_hostname(void);

/**
 * Bonjour instance name: "WiFiLabel-XXXX" plus the USB model when one is known.
 * Returns true when the model came from the USB printer.
 */
bool net_copy_printer_label(char *dst, size_t dst_len);

/** True when no Wi-Fi credentials are stored and the setup page is armed. */
bool net_in_setup(void);

/**
 * Copy the preferred address (Ethernet if it has an address, otherwise Wi-Fi).
 * iface receives "eth" or "wifi". Returns false when neither interface has an address.
 */
bool net_copy_ip(char *ip, size_t ip_len, char *iface, size_t iface_len);

/** SSID and password travel together. ssid is at most 32 characters, pass at most 63. */
typedef struct {
    char ssid[33];
    char pass[65];
} net_wifi_creds_t;

/** Listen on port. Dual-stack when IPv6 is enabled, otherwise IPv4. Returns -1 on failure. */
int net_listen_tcp(uint16_t port, int backlog);

/** Write the whole buffer. Retries EINTR and EAGAIN. Returns false if the peer stops. */
bool net_send_all(int sock, const void *data, size_t len);

/** Receive and send timeouts, plus TCP keepalive, on an accepted socket. */
void net_tune_accepted(int sock, int timeout_ms);

#ifdef __cplusplus
}
#endif
