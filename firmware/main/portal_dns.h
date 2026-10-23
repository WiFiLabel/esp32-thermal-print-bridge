#pragma once

#include <stdint.h>

#include "esp_err.h"

/** Hijack DNS on the setup AP. Every query gets an A record for ip_nbo. */
esp_err_t portal_dns_start(uint32_t ip_nbo);

/** Stop the hijack. Safe to call when it is not running. */
void portal_dns_stop(void);
