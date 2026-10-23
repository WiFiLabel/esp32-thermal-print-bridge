#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Print one setup label after the Wi-Fi form is saved. Later boots do not. */
esp_err_t welcome_start(void);

#ifdef __cplusplus
}
#endif
