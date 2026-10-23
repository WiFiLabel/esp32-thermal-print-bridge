#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 4x6 inch at 203 dpi, row-packed 1-bit. */
#define LABEL_203DPI_W 812
#define LABEL_203DPI_H 1218
#define LABEL_203DPI_BYTES_PER_ROW ((LABEL_203DPI_W + 7) / 8)
#define LABEL_203DPI_BYTES (LABEL_203DPI_BYTES_PER_ROW * LABEL_203DPI_H)

/**
 * Read callback. Return the number of bytes copied, 0 on clean EOF, or -1 on error.
 * Short reads are allowed.
 */
typedef int (*raster_read_fn)(void *ctx, uint8_t *dst, size_t len);

/** Called once per decoded page with a complete ZPL job. Caller frees nothing. */
typedef esp_err_t (*raster_page_fn)(const uint8_t *zpl, size_t zpl_len, void *ctx);

/**
 * Decode image/pwg-raster or image/urf and emit ZPL ^GFA pages at 203 dpi, 4x6.
 * mime may be NULL. copies is clamped to 1..9.
 * Follows the CUPS raster reader that PAPPL and LPrint use for these two formats.
 */
esp_err_t raster_job_to_zpl(raster_read_fn read_fn, void *read_ctx, const char *mime,
                            int copies, raster_page_fn page_fn, void *page_ctx);

#ifdef __cplusplus
}
#endif
