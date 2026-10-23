#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Read one byte from the wifilabel namespace. Missing namespace or key returns an error. */
esp_err_t wifilabel_nvs_get_u8(const char *key, uint8_t *value);

/** Write one byte and commit. */
esp_err_t wifilabel_nvs_set_u8(const char *key, uint8_t value);

/** Erase one key and commit. A missing key is success. */
esp_err_t wifilabel_nvs_erase(const char *key);

/**
 * Copy a string into out. On any failure out is empty when out_len > 0.
 * out_len is the buffer size, including the trailing NUL.
 */
esp_err_t wifilabel_nvs_get_str(const char *key, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
