#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 4x6" at 203 DPI (common thermal shipping label), row-packed 1-bit. */
#define LABEL_203DPI_W 812
#define LABEL_203DPI_H 1218
#define LABEL_203DPI_BYTES_PER_ROW ((LABEL_203DPI_W + 7) / 8)
#define LABEL_203DPI_BYTES (LABEL_203DPI_BYTES_PER_ROW * LABEL_203DPI_H)

/**
 * Wrap a 1-bit packed bitmap as ZPL II ^GFA graphic field.
 * On success, *out points to a heap buffer the caller must free().
 * Returns byte length, or 0 on error.
 */
size_t label_wrap_zpl_gfa(const uint8_t *bitmap, size_t bitmap_len,
                          int width_dots, int height_dots,
                          uint8_t **out);

/** Wrap as EPL2 GW (Graphic Write). Caller frees *out. */
size_t label_wrap_epl_gw(const uint8_t *bitmap, size_t bitmap_len,
                         int width_dots, int height_dots,
                         uint8_t **out);

/** Wrap as TSPL2 BITMAP. Caller frees *out. */
size_t label_wrap_tspl_bitmap(const uint8_t *bitmap, size_t bitmap_len,
                              int width_dots, int height_dots,
                              uint8_t **out);

#ifdef __cplusplus
}
#endif
