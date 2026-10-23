#include "raster_zpl.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "wlt_zpl.h"

static const char *TAG = "raster";

/* Offsets into cups_page_header2_t (1796 bytes, 32-bit ABI). */
#define H_BYTES 1796
#define OFF_HWRES 276
#define OFF_WIDTH 372
#define OFF_HEIGHT 376
#define OFF_BPC 384
#define OFF_BPP 388
#define OFF_BPL 392
#define OFF_ORDER 396
#define OFF_CSPACE 400
#define OFF_COMP 404
#define OFF_NCOLOR 420
/* cupsInteger[1] and [2] are PWG CrossFeedTransform and FeedTransform. */
#define OFF_CROSS_FEED 456
#define OFF_FEED 460

#define CSPACE_W 0
#define CSPACE_RGB 1
#define CSPACE_RGBA 2
#define CSPACE_K 3
#define CSPACE_CMYK 6
#define CSPACE_CIELAB 16
#define CSPACE_RGBW 17
#define CSPACE_SW 18
#define CSPACE_SRGB 19
#define CSPACE_ADOBERGB 20
#define CSPACE_DEVICE1 48

#define MAX_PIXELS (8 * 1024 * 1024)
#define MAX_LINE 65536

static int read_full(raster_read_fn fn, void *ctx, uint8_t *dst, size_t n, bool *eof)
{
    size_t got = 0;
    while (got < n) {
        int r = fn(ctx, dst + got, n - got);
        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            if (got == 0 && eof) {
                *eof = true;
                return 0;
            }
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

static uint32_t rd32(const uint8_t *p, int off, bool be)
{
    if (be) {
        return ((uint32_t)p[off] << 24) | ((uint32_t)p[off + 1] << 16) | ((uint32_t)p[off + 2] << 8) | p[off + 3];
    }
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) | ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 3] << 24);
}

static int bytes_per_pixel(uint32_t bits, uint32_t order, uint32_t bpc)
{
    uint32_t src = (order == 0) ? bits : bpc;
    int bpp = (int)((src + 7) / 8);
    if (bpp < 1) {
        bpp = 1;
    }
    return bpp;
}

static bool white_fill(uint32_t cspace)
{
    return cspace == CSPACE_W || cspace == CSPACE_RGB || cspace == CSPACE_RGBA || cspace == CSPACE_SW ||
           cspace == CSPACE_SRGB || cspace == CSPACE_RGBW || cspace == CSPACE_ADOBERGB;
}

/* CUPS modified PackBits (the reader PAPPL/LPrint use). Not TIFF PackBits. */
static int decode_packbits(raster_read_fn fn, void *ctx, uint8_t *dst, int dst_len, int bpp, bool fill_white)
{
    int filled = 0;
    while (filled < dst_len) {
        uint8_t code = 0;
        if (read_full(fn, ctx, &code, 1, NULL) != 0) {
            return -1;
        }
        if (code == 128) {
            memset(dst + filled, fill_white ? 0xff : 0x00, (size_t)(dst_len - filled));
            return dst_len;
        }
        if (code & 0x80) {
            int count = (int)(257 - code) * bpp;
            if (count > dst_len - filled) {
                count = dst_len - filled;
            }
            if (count > 0 && read_full(fn, ctx, dst + filled, (size_t)count, NULL) != 0) {
                return -1;
            }
            filled += count;
        } else {
            int count = (int)(code + 1) * bpp;
            if (count > dst_len - filled) {
                count = dst_len - filled;
            }
            if (count < bpp) {
                return -1;
            }
            uint8_t pix[8];
            if (bpp > (int)sizeof(pix)) {
                return -1;
            }
            if (read_full(fn, ctx, pix, (size_t)bpp, NULL) != 0) {
                return -1;
            }
            while (count > 0) {
                int n = bpp;
                if (n > count) {
                    n = count;
                }
                memcpy(dst + filled, pix, (size_t)n);
                filled += n;
                count -= n;
            }
        }
    }
    return filled;
}

static uint8_t luma_at(uint32_t cspace, uint32_t bits, const uint8_t *row, int x, int bpl)
{
    if (bits <= 1) {
        int byte = x >> 3;
        if (byte < 0 || byte >= bpl) {
            return 255;
        }
        int bit = (row[byte] >> (7 - (x & 7))) & 1;
        bool one_is_black = (cspace == CSPACE_K || cspace >= CSPACE_DEVICE1);
        if (one_is_black) {
            return bit ? 0 : 255;
        }
        return bit ? 255 : 0;
    }

    int bpp = (int)(bits / 8);
    if (bpp < 1) {
        bpp = 1;
    }
    if (x < 0 || (x + 1) * bpp > bpl) {
        return 255;
    }
    const uint8_t *p = row + (size_t)x * (size_t)bpp;
    if (cspace == CSPACE_K || (cspace >= CSPACE_DEVICE1 && bpp == 1)) {
        return (uint8_t)(255 - p[0]);
    }
    if (cspace == CSPACE_W || cspace == CSPACE_SW || cspace == CSPACE_CIELAB) {
        return p[0];
    }
    if (bpp >= 3 && (cspace == CSPACE_RGB || cspace == CSPACE_RGBA || cspace == CSPACE_SRGB || cspace == CSPACE_ADOBERGB ||
                     cspace == CSPACE_RGBW)) {
        return (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
    }
    if (cspace == CSPACE_CMYK && bpp >= 4) {
        int dark = p[3];
        int cmy = (p[0] + p[1] + p[2]) / 3;
        if (cmy > dark) {
            dark = cmy;
        }
        return (uint8_t)(255 - dark);
    }
    return p[0];
}

static void scale_to_4x6(const uint8_t *gray, int sw, int sh, uint8_t *bits, bool flip_x, bool flip_y)
{
    memset(bits, 0, LABEL_203DPI_BYTES);
    bool rot = sw > sh;
    for (int dy = 0; dy < LABEL_203DPI_H; dy++) {
        int oy = flip_y ? (LABEL_203DPI_H - 1 - dy) : dy;
        for (int dx = 0; dx < LABEL_203DPI_W; dx++) {
            int ox = flip_x ? (LABEL_203DPI_W - 1 - dx) : dx;
            int sx;
            int sy;
            if (!rot) {
                sx = (int)((int64_t)ox * sw / LABEL_203DPI_W);
                sy = (int)((int64_t)oy * sh / LABEL_203DPI_H);
            } else {
                int nx = (int)((int64_t)ox * sh / LABEL_203DPI_W);
                int ny = (int)((int64_t)oy * sw / LABEL_203DPI_H);
                sx = ny;
                sy = sh - 1 - nx;
            }
            if (sx < 0) {
                sx = 0;
            }
            if (sy < 0) {
                sy = 0;
            }
            if (sx >= sw) {
                sx = sw - 1;
            }
            if (sy >= sh) {
                sy = sh - 1;
            }
            if (gray[(size_t)sy * (size_t)sw + (size_t)sx] < 128) {
                /* ^GFA rows are byte-padded. 812 dots occupy 102 bytes, not 812 bits. */
                size_t byte = (size_t)dy * LABEL_203DPI_BYTES_PER_ROW + (size_t)(dx >> 3);
                bits[byte] |= (uint8_t)(0x80 >> (dx & 7));
            }
        }
    }
}

static int urf_cspace(uint8_t idx, uint32_t *cspace, uint32_t *ncolor)
{
    static const uint32_t spaces[] = {CSPACE_SW, CSPACE_SRGB, CSPACE_CIELAB, CSPACE_ADOBERGB, CSPACE_W, CSPACE_RGB, CSPACE_CMYK};
    static const uint32_t colors[] = {1, 3, 3, 3, 1, 3, 4};
    if (idx >= sizeof(spaces) / sizeof(spaces[0])) {
        *cspace = CSPACE_DEVICE1;
        *ncolor = 1;
        return 0;
    }
    *cspace = spaces[idx];
    *ncolor = colors[idx];
    return 0;
}

static esp_err_t emit_page(const uint8_t *gray, int width, int height, int copies, bool flip_x, bool flip_y,
                            raster_page_fn page_fn, void *page_ctx)
{
    uint8_t *bits = heap_caps_malloc(LABEL_203DPI_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bits) {
        bits = malloc(LABEL_203DPI_BYTES);
    }
    if (!bits) {
        return ESP_ERR_NO_MEM;
    }
    scale_to_4x6(gray, width, height, bits, flip_x, flip_y);

    /* ^GFA stores two hex characters per bitmap byte, plus a short header. */
    size_t zpl_cap = (size_t)LABEL_203DPI_BYTES * 2 + 128;
    char *zpl = heap_caps_malloc(zpl_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!zpl) {
        zpl = malloc(zpl_cap);
    }
    if (!zpl) {
        free(bits);
        return ESP_ERR_NO_MEM;
    }
    int zpl_len = wlt_wrap_zpl_gfa(zpl, zpl_cap, bits, LABEL_203DPI_BYTES, LABEL_203DPI_W, LABEL_203DPI_H);
    free(bits);
    if (zpl_len < 0) {
        free(zpl);
        return ESP_FAIL;
    }

    if (copies < 1) {
        copies = 1;
    }
    if (copies > 9) {
        copies = 9;
    }
    esp_err_t err = ESP_OK;
    for (int i = 0; i < copies && err == ESP_OK; i++) {
        err = page_fn((const uint8_t *)zpl, (size_t)zpl_len, page_ctx);
    }
    free(zpl);
    return err;
}

static esp_err_t decode_rows(raster_read_fn fn, void *ctx, uint8_t *gray, int width, int height, int bpl, uint32_t bits,
                             uint32_t cspace, int bpp, bool compressed)
{
    uint8_t *line = malloc((size_t)bpl);
    if (!line) {
        return ESP_ERR_NO_MEM;
    }
    int row = 0;
    while (row < height) {
        int repeat = 1;
        if (compressed) {
            uint8_t rep = 0;
            if (read_full(fn, ctx, &rep, 1, NULL) != 0) {
                free(line);
                return ESP_FAIL;
            }
            repeat = (int)rep + 1;
            if (decode_packbits(fn, ctx, line, bpl, bpp, white_fill(cspace)) != bpl) {
                free(line);
                return ESP_FAIL;
            }
        } else if (read_full(fn, ctx, line, (size_t)bpl, NULL) != 0) {
            free(line);
            return ESP_FAIL;
        }
        for (int r = 0; r < repeat && row < height; r++, row++) {
            uint8_t *dst = gray + (size_t)row * (size_t)width;
            for (int x = 0; x < width; x++) {
                dst[x] = luma_at(cspace, bits, line, x, bpl);
            }
        }
    }
    free(line);
    return ESP_OK;
}

esp_err_t raster_job_to_zpl(raster_read_fn read_fn, void *read_ctx, const char *mime, int copies, raster_page_fn page_fn,
                            void *page_ctx)
{
    if (!read_fn || !page_fn) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t magic[4];
    bool eof = false;
    if (read_full(read_fn, read_ctx, magic, 4, &eof) != 0) {
        return ESP_FAIL;
    }

    bool urf = memcmp(magic, "UNIR", 4) == 0;
    bool pwg_le = memcmp(magic, "2SaR", 4) == 0 || memcmp(magic, "3SaR", 4) == 0;
    bool pwg_be = memcmp(magic, "RaS2", 4) == 0 || memcmp(magic, "RaS3", 4) == 0;
    if (!urf && !pwg_le && !pwg_be) {
        ESP_LOGE(TAG, "not PWG raster or URF (magic %02x %02x %02x %02x)", magic[0], magic[1], magic[2], magic[3]);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (mime && mime[0]) {
        bool want_urf = strstr(mime, "image/urf") != NULL;
        bool want_pwg = strstr(mime, "image/pwg-raster") != NULL;
        if (want_urf && !urf) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (want_pwg && urf) {
            return ESP_ERR_NOT_SUPPORTED;
        }
    }

    int page_limit = 32;
    if (urf) {
        uint8_t rest[8];
        if (read_full(read_fn, read_ctx, rest, sizeof(rest), NULL) != 0) {
            return ESP_FAIL;
        }
        uint32_t count = rd32(rest, 4, true);
        if (count > 0 && count < 32) {
            page_limit = (int)count;
        }
    }

    bool v3 = memcmp(magic, "3SaR", 4) == 0 || memcmp(magic, "RaS3", 4) == 0;
    if (v3) {
        ESP_LOGE(TAG, "CUPS raster version 3 is not supported");
        return ESP_ERR_NOT_SUPPORTED;
    }

    bool be = pwg_be;
    bool v2 = memcmp(magic, "2SaR", 4) == 0 || memcmp(magic, "RaS2", 4) == 0;
    int pages = 0;
    while (pages < page_limit) {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t bits = 0;
        uint32_t bpc = 8;
        uint32_t bpl = 0;
        uint32_t order = 0;
        uint32_t cspace = CSPACE_SW;
        uint32_t ncolor = 1;
        uint32_t compression = 0;
        bool compressed = urf || v2;
        bool flip_x = false;
        bool flip_y = false;

        if (urf) {
            uint8_t hdr[32];
            eof = false;
            int rr = read_full(read_fn, read_ctx, hdr, sizeof(hdr), &eof);
            if (eof) {
                break;
            }
            if (rr != 0) {
                return pages ? ESP_OK : ESP_FAIL;
            }
            bits = hdr[0];
            urf_cspace(hdr[1], &cspace, &ncolor);
            width = rd32(hdr, 12, true);
            height = rd32(hdr, 16, true);
            bpl = (bits >= 8) ? width * (bits / 8) : (width * bits + 7) / 8;
            (void)ncolor;
        } else {
            uint8_t hdr[H_BYTES];
            eof = false;
            int rr = read_full(read_fn, read_ctx, hdr, sizeof(hdr), &eof);
            if (eof) {
                break;
            }
            if (rr != 0) {
                return pages ? ESP_OK : ESP_FAIL;
            }
            width = rd32(hdr, OFF_WIDTH, be);
            height = rd32(hdr, OFF_HEIGHT, be);
            bpc = rd32(hdr, OFF_BPC, be);
            bits = rd32(hdr, OFF_BPP, be);
            bpl = rd32(hdr, OFF_BPL, be);
            order = rd32(hdr, OFF_ORDER, be);
            cspace = rd32(hdr, OFF_CSPACE, be);
            compression = rd32(hdr, OFF_COMP, be);
            compressed = v2 || compression != 0;
            (void)rd32(hdr, OFF_HWRES, be);
            int32_t cross = (int32_t)rd32(hdr, OFF_CROSS_FEED, be);
            int32_t feed = (int32_t)rd32(hdr, OFF_FEED, be);
            flip_x = cross < 0;
            flip_y = feed < 0;
        }

        if (width == 0 || height == 0 || width > 20000 || height > 20000) {
            ESP_LOGE(TAG, "bad raster size %u x %u", (unsigned)width, (unsigned)height);
            return ESP_FAIL;
        }
        if (bpl == 0) {
            bpl = (bits >= 8) ? width * (bits / 8) : (width * (bits ? bits : 1) + 7) / 8;
        }
        if (bpl == 0 || bpl > MAX_LINE) {
            return ESP_FAIL;
        }
        if ((uint64_t)width * height > MAX_PIXELS) {
            ESP_LOGE(TAG, "raster too large");
            return ESP_ERR_NO_MEM;
        }

        int bpp = bytes_per_pixel(bits ? bits : 8, order, bpc ? bpc : 8);
        if (bpp > 8) {
            ESP_LOGE(TAG, "unsupported %u bits/pixel", (unsigned)bits);
            return ESP_ERR_NOT_SUPPORTED;
        }

        uint8_t *gray = heap_caps_malloc((size_t)width * (size_t)height, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!gray) {
            gray = malloc((size_t)width * (size_t)height);
        }
        if (!gray) {
            return ESP_ERR_NO_MEM;
        }

        ESP_LOGI(TAG, "page %d %ux%u cspace %u bits %u %s", pages + 1, (unsigned)width, (unsigned)height,
                 (unsigned)cspace, (unsigned)bits, compressed ? "packbits" : "raw");

        esp_err_t err = decode_rows(read_fn, read_ctx, gray, (int)width, (int)height, (int)bpl, bits ? bits : 8, cspace, bpp,
                                     compressed);
        if (err != ESP_OK) {
            free(gray);
            return err;
        }
        err = emit_page(gray, (int)width, (int)height, copies, flip_x, flip_y, page_fn, page_ctx);
        free(gray);
        if (err != ESP_OK) {
            return err;
        }
        pages++;
    }

    if (pages == 0) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "converted %d page(s) to ZPL ^GFA %dx%d", pages, LABEL_203DPI_W, LABEL_203DPI_H);
    return ESP_OK;
}
