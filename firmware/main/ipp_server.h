#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** IPP Everywhere listener (TCP 631, path /ipp/print). */
#define IPP_PORT 631

esp_err_t ipp_server_start(void);

#ifdef __cplusplus
}
#endif
