#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Restart after delay_ms so the caller can finish its response first. */
esp_err_t app_schedule_restart(uint32_t delay_ms);

#ifdef __cplusplus
}
#endif
