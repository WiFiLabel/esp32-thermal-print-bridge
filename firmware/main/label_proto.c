#include "label_proto.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "label_proto";

static size_t append_bytes(uint8_t **buf, size_t *cap, size_t *len,
                           const void *src, size_t n)
{
    if (*len + n > *cap) {
        size_t nc = (*cap == 0) ? 256 : *cap;
        while (nc < *len + n) {
            nc *= 2;
        }
        uint8_t *nb = realloc(*buf, nc);
        if (!nb) {
            return 0;
        }
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, src, n);
    *len += n;
    return n;
}

static size_t append_fmt(uint8_t **buf, size_t *cap, size_t *len, const char *fmt, ...)
{
    char tmp[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        return 0;
    }
    return append_bytes(buf, cap, len, tmp, (size_t)n);
}

/* Minimal ASCII-hex encoder for ZPL ^GFA data. */
static size_t append_hex(uint8_t **buf, size_t *cap, size_t *len,
                         const uint8_t *data, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        char pair[2] = { hex[data[i] >> 4], hex[data[i] & 0x0f] };
        if (!append_bytes(buf, cap, len, pair, 2)) {
            return 0;
        }
    }
    return n * 2;
}

size_t label_wrap_zpl_gfa(const uint8_t *bitmap, size_t bitmap_len,
                          int width_dots, int height_dots,
                          uint8_t **out)
{
    if (!bitmap || !out || width_dots <= 0 || height_dots <= 0) {
        return 0;
    }

    const int bytes_per_row = (width_dots + 7) / 8;
    const size_t expected = (size_t)bytes_per_row * (size_t)height_dots;
    if (bitmap_len < expected) {
        ESP_LOGE(TAG, "ZPL: bitmap too short (%u < %u)", (unsigned)bitmap_len, (unsigned)expected);
        return 0;
    }

    uint8_t *buf = NULL;
    size_t cap = 0;
    size_t len = 0;

    if (!append_fmt(&buf, &cap, &len, "^XA\n^FO0,0^GFA,%u,%u,%d,",
                    (unsigned)expected, (unsigned)expected, bytes_per_row)) {
        free(buf);
        return 0;
    }
    if (!append_hex(&buf, &cap, &len, bitmap, expected)) {
        free(buf);
        return 0;
    }
    if (!append_fmt(&buf, &cap, &len, "^FS\n^XZ\n")) {
        free(buf);
        return 0;
    }

    *out = buf;
    return len;
}

size_t label_wrap_epl_gw(const uint8_t *bitmap, size_t bitmap_len,
                         int width_dots, int height_dots,
                         uint8_t **out)
{
    if (!bitmap || !out || width_dots <= 0 || height_dots <= 0) {
        return 0;
    }

    const int bytes_per_row = (width_dots + 7) / 8;
    const size_t expected = (size_t)bytes_per_row * (size_t)height_dots;
    if (bitmap_len < expected) {
        return 0;
    }

    uint8_t *buf = NULL;
    size_t cap = 0;
    size_t len = 0;

    /* EPL2: GW{x},{y},{widthBytes},{height}{binary data} */
    if (!append_fmt(&buf, &cap, &len, "\r\nN\r\nGW0,0,%d,%d,",
                    bytes_per_row, height_dots)) {
        free(buf);
        return 0;
    }
    if (!append_bytes(&buf, &cap, &len, bitmap, expected)) {
        free(buf);
        return 0;
    }
    if (!append_fmt(&buf, &cap, &len, "\r\nP1\r\n")) {
        free(buf);
        return 0;
    }

    *out = buf;
    return len;
}

size_t label_wrap_tspl_bitmap(const uint8_t *bitmap, size_t bitmap_len,
                              int width_dots, int height_dots,
                              uint8_t **out)
{
    if (!bitmap || !out || width_dots <= 0 || height_dots <= 0) {
        return 0;
    }

    const int bytes_per_row = (width_dots + 7) / 8;
    const size_t expected = (size_t)bytes_per_row * (size_t)height_dots;
    if (bitmap_len < expected) {
        return 0;
    }

    uint8_t *buf = NULL;
    size_t cap = 0;
    size_t len = 0;

    /* TSPL2: BITMAP x,y,widthBytes,height,mode,data */
    if (!append_fmt(&buf, &cap, &len, "SIZE 4,6\r\nCLS\r\nBITMAP 0,0,%d,%d,0,",
                    bytes_per_row, height_dots)) {
        free(buf);
        return 0;
    }
    if (!append_bytes(&buf, &cap, &len, bitmap, expected)) {
        free(buf);
        return 0;
    }
    if (!append_fmt(&buf, &cap, &len, "\r\nPRINT 1\r\n")) {
        free(buf);
        return 0;
    }

    *out = buf;
    return len;
}
