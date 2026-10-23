#include "ipp_server.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "net_bringup.h"
#include "raster_zpl.h"
#include "usb_printer.h"
#include "web_ui.h"

static const char *TAG = "ipp";

#define IPP_STATUS_OK 0x0000
#define IPP_STATUS_OK_IGNORED 0x0001
#define IPP_STATUS_BAD_REQUEST 0x0400
#define IPP_STATUS_NOT_FOUND 0x0406
#define IPP_STATUS_DOC_FORMAT 0x040a
#define IPP_STATUS_DOC_FORMAT_ERROR 0x0410
#define IPP_STATUS_COMPRESSION 0x040f
#define IPP_STATUS_INTERNAL 0x0500
#define IPP_STATUS_OP_UNSUPPORTED 0x0501
#define IPP_STATUS_VERSION 0x0503
#define IPP_STATUS_DEVICE 0x0504
#define IPP_STATUS_NOT_ACCEPTING 0x0506
#define IPP_STATUS_BUSY 0x0507

#define IPP_OP_PRINT_JOB 0x0002
#define IPP_OP_VALIDATE_JOB 0x0004
#define IPP_OP_CREATE_JOB 0x0005
#define IPP_OP_SEND_DOCUMENT 0x0006
#define IPP_OP_CANCEL_JOB 0x0008
#define IPP_OP_GET_JOB_ATTRIBUTES 0x0009
#define IPP_OP_GET_JOBS 0x000a
#define IPP_OP_GET_PRINTER_ATTRIBUTES 0x000b
#define IPP_OP_IDENTIFY_PRINTER 0x003c

#define TAG_OP 0x01
#define TAG_JOB 0x02
#define TAG_END 0x03
#define TAG_PRINTER 0x04
#define TAG_INTEGER 0x21
#define TAG_BOOLEAN 0x22
#define TAG_ENUM 0x23
#define TAG_RESOLUTION 0x32
#define TAG_BEGIN_COLLECTION 0x34
#define TAG_END_COLLECTION 0x37
#define TAG_TEXT 0x41
#define TAG_NAME 0x42
#define TAG_KEYWORD 0x44
#define TAG_URI 0x45
#define TAG_CHARSET 0x47
#define TAG_LANGUAGE 0x48
#define TAG_MIMETYPE 0x49
#define TAG_MEMBER 0x4a

#define JOB_NONE 0
#define JOB_PENDING 3
#define JOB_PROCESSING 5
#define JOB_CANCELED 7
#define JOB_ABORTED 8
#define JOB_COMPLETED 9

typedef struct {
    int sock;
    bool chunked;
    int body_left;
    int chunk_left;
    bool chunk_eof;
    bool need_crlf;
    bool saw_chunk;
} body_t;

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} ipp_buf_t;

typedef struct {
    uint16_t op;
    uint32_t request_id;
    char format[64];
    char compression[32];
    int32_t job_id;
    int32_t copies;
    int last_document;
    bool saw_last_document;
} ipp_req_t;

static int s_job_id;
static int s_job_state;
static int s_job_copies = 1;

static int recv_some(int sock, uint8_t *dst, size_t n)
{
    while (true) {
        int r = recv(sock, dst, n, 0);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        return r;
    }
}

static int read_line(body_t *b, char *dst, size_t dst_len)
{
    size_t n = 0;
    while (n + 1 < dst_len) {
        uint8_t c = 0;
        int r = recv_some(b->sock, &c, 1);
        if (r <= 0) {
            return -1;
        }
        dst[n++] = (char)c;
        if (c == '\n') {
            dst[n] = '\0';
            return 0;
        }
    }
    return -1;
}

static int next_chunk(body_t *b)
{
    if (b->need_crlf) {
        char crlf[4];
        if (read_line(b, crlf, sizeof(crlf)) != 0) {
            return -1;
        }
        b->need_crlf = false;
    }
    char line[32];
    if (read_line(b, line, sizeof(line)) != 0) {
        return -1;
    }
    char *semi = strchr(line, ';');
    if (semi) {
        *semi = '\0';
    }
    unsigned long sz = strtoul(line, NULL, 16);
    if (sz == 0) {
        b->chunk_eof = true;
        b->chunk_left = 0;
        char trail[64];
        do {
            if (read_line(b, trail, sizeof(trail)) != 0) {
                return -1;
            }
        } while (trail[0] != '\r' && trail[0] != '\n');
        return 0;
    }
    if (sz > 1024 * 1024) {
        return -1;
    }
    b->chunk_left = (int)sz;
    b->saw_chunk = true;
    return 0;
}

static int body_read(void *ctx, uint8_t *dst, size_t n)
{
    body_t *b = ctx;
    size_t got = 0;
    while (got < n) {
        if (b->chunked) {
            if (b->chunk_eof) {
                break;
            }
            if (b->chunk_left == 0) {
                if (next_chunk(b) != 0) {
                    return -1;
                }
                if (b->chunk_eof) {
                    break;
                }
            }
            size_t want = n - got;
            if ((int)want > b->chunk_left) {
                want = (size_t)b->chunk_left;
            }
            int r = recv_some(b->sock, dst + got, want);
            if (r <= 0) {
                return got ? (int)got : -1;
            }
            b->chunk_left -= r;
            got += (size_t)r;
            if (b->chunk_left == 0) {
                b->need_crlf = true;
            }
        } else {
            if (b->body_left == 0) {
                break;
            }
            size_t want = n - got;
            if (b->body_left > 0 && (int)want > b->body_left) {
                want = (size_t)b->body_left;
            }
            int r = recv_some(b->sock, dst + got, want);
            if (r < 0) {
                return -1;
            }
            if (r == 0) {
                break;
            }
            if (b->body_left > 0) {
                b->body_left -= r;
            }
            got += (size_t)r;
        }
    }
    return (int)got;
}

static int rd_exact(body_t *b, void *dst, size_t n)
{
    uint8_t *p = dst;
    size_t got = 0;
    while (got < n) {
        int r = body_read(b, p + got, n - got);
        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

static int rd_u8(body_t *b, uint8_t *v)
{
    return rd_exact(b, v, 1);
}

static int rd_u16(body_t *b, uint16_t *v)
{
    uint8_t p[2];
    if (rd_exact(b, p, 2) != 0) {
        return -1;
    }
    *v = (uint16_t)((p[0] << 8) | p[1]);
    return 0;
}

static int rd_u32(body_t *b, uint32_t *v)
{
    uint8_t p[4];
    if (rd_exact(b, p, 4) != 0) {
        return -1;
    }
    *v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    return 0;
}

static int discard_n(body_t *b, size_t n)
{
    uint8_t tmp[64];
    while (n) {
        size_t c = n > sizeof(tmp) ? sizeof(tmp) : n;
        if (rd_exact(b, tmp, c) != 0) {
            return -1;
        }
        n -= c;
    }
    return 0;
}

static int parse_ipp(body_t *b, ipp_req_t *req)
{
    memset(req, 0, sizeof(*req));
    req->copies = 1;
    req->last_document = 1;
    uint8_t ver[2];
    uint16_t op = 0;
    if (rd_exact(b, ver, 2) != 0 || rd_u16(b, &op) != 0 || rd_u32(b, &req->request_id) != 0) {
        return -1;
    }
    if (ver[0] != 1 && ver[0] != 2) {
        req->op = 0xffff;
        return 0;
    }
    req->op = op;

    uint8_t tag = 0;
    if (rd_u8(b, &tag) != 0) {
        return -1;
    }
    while (tag != TAG_END) {
        if (tag < 0x10) {
            if (rd_u8(b, &tag) != 0) {
                return -1;
            }
            continue;
        }
        uint16_t name_len = 0;
        uint16_t val_len = 0;
        if (rd_u16(b, &name_len) != 0) {
            return -1;
        }
        char name[80];
        name[0] = '\0';
        if (name_len > 0) {
            if (name_len >= sizeof(name)) {
                if (discard_n(b, name_len) != 0) {
                    return -1;
                }
            } else if (rd_exact(b, name, name_len) != 0) {
                return -1;
            } else {
                name[name_len] = '\0';
            }
        }
        if (rd_u16(b, &val_len) != 0) {
            return -1;
        }
        uint8_t val[128];
        memset(val, 0, sizeof(val));
        if (val_len > 0) {
            if (val_len < sizeof(val)) {
                if (rd_exact(b, val, val_len) != 0) {
                    return -1;
                }
            } else if (discard_n(b, val_len) != 0) {
                return -1;
            }
        }
        if (name[0]) {
            if (strcmp(name, "document-format") == 0 && val_len < sizeof(req->format)) {
                memcpy(req->format, val, val_len);
                req->format[val_len] = '\0';
            } else if (strcmp(name, "compression") == 0 && val_len < sizeof(req->compression)) {
                memcpy(req->compression, val, val_len);
                req->compression[val_len] = '\0';
            } else if (strcmp(name, "copies") == 0 && val_len == 4) {
                req->copies = (int32_t)(((uint32_t)val[0] << 24) | ((uint32_t)val[1] << 16) | ((uint32_t)val[2] << 8) | val[3]);
            } else if (strcmp(name, "job-id") == 0 && val_len == 4) {
                req->job_id = (int32_t)(((uint32_t)val[0] << 24) | ((uint32_t)val[1] << 16) | ((uint32_t)val[2] << 8) | val[3]);
            } else if (strcmp(name, "last-document") == 0 && val_len >= 1) {
                req->last_document = val[0] ? 1 : 0;
                req->saw_last_document = true;
            }
        }
        if (rd_u8(b, &tag) != 0) {
            return -1;
        }
    }
    return 0;
}

static bool ipp_add(ipp_buf_t *b, const void *src, size_t n)
{
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap : 512;
        while (nc < b->len + n) {
            nc *= 2;
        }
        if (nc > 24576) {
            return false;
        }
        uint8_t *nb = realloc(b->buf, nc);
        if (!nb) {
            return false;
        }
        b->buf = nb;
        b->cap = nc;
    }
    memcpy(b->buf + b->len, src, n);
    b->len += n;
    return true;
}

static bool ipp_u8(ipp_buf_t *b, uint8_t v)
{
    return ipp_add(b, &v, 1);
}

static bool ipp_u16(ipp_buf_t *b, uint16_t v)
{
    uint8_t p[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    return ipp_add(b, p, 2);
}

static bool ipp_u32(ipp_buf_t *b, uint32_t v)
{
    uint8_t p[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    return ipp_add(b, p, 4);
}

static bool ipp_str(ipp_buf_t *b, uint8_t tag, const char *name, const char *value)
{
    size_t nl = name ? strlen(name) : 0;
    size_t vl = value ? strlen(value) : 0;
    return ipp_u8(b, tag) && ipp_u16(b, (uint16_t)nl) && (nl == 0 || ipp_add(b, name, nl)) && ipp_u16(b, (uint16_t)vl) &&
           (vl == 0 || ipp_add(b, value, vl));
}

static bool ipp_i32(ipp_buf_t *b, uint8_t tag, const char *name, int32_t v)
{
    size_t nl = name ? strlen(name) : 0;
    return ipp_u8(b, tag) && ipp_u16(b, (uint16_t)nl) && (nl == 0 || ipp_add(b, name, nl)) && ipp_u16(b, 4) &&
           ipp_u32(b, (uint32_t)v);
}

static bool ipp_bool(ipp_buf_t *b, const char *name, bool v)
{
    size_t nl = name ? strlen(name) : 0;
    uint8_t byte = v ? 1 : 0;
    return ipp_u8(b, TAG_BOOLEAN) && ipp_u16(b, (uint16_t)nl) && (nl == 0 || ipp_add(b, name, nl)) && ipp_u16(b, 1) &&
           ipp_u8(b, byte);
}

static bool ipp_res(ipp_buf_t *b, const char *name, int32_t x, int32_t y)
{
    size_t nl = name ? strlen(name) : 0;
    return ipp_u8(b, TAG_RESOLUTION) && ipp_u16(b, (uint16_t)nl) && (nl == 0 || ipp_add(b, name, nl)) && ipp_u16(b, 9) &&
           ipp_u32(b, (uint32_t)x) && ipp_u32(b, (uint32_t)y) && ipp_u8(b, 3);
}

static bool ipp_member(ipp_buf_t *b, const char *member)
{
    return ipp_str(b, TAG_MEMBER, "", member);
}

static bool ipp_end_col(ipp_buf_t *b)
{
    return ipp_u8(b, TAG_END_COLLECTION) && ipp_u16(b, 0) && ipp_u16(b, 0);
}

static bool ipp_media_col(ipp_buf_t *b, const char *name)
{
    if (!ipp_u8(b, TAG_BEGIN_COLLECTION) || !ipp_u16(b, (uint16_t)strlen(name)) || !ipp_add(b, name, strlen(name)) ||
        !ipp_u16(b, 0)) {
        return false;
    }
    if (!ipp_member(b, "media-size") || !ipp_u8(b, TAG_BEGIN_COLLECTION) || !ipp_u16(b, 0) || !ipp_u16(b, 0)) {
        return false;
    }
    /* 4 x 6 inches in hundredths of a millimeter. */
    if (!ipp_member(b, "x-dimension") || !ipp_i32(b, TAG_INTEGER, "", 10160)) {
        return false;
    }
    if (!ipp_member(b, "y-dimension") || !ipp_i32(b, TAG_INTEGER, "", 15240)) {
        return false;
    }
    if (!ipp_end_col(b)) {
        return false;
    }
    if (!ipp_member(b, "media-type") || !ipp_str(b, TAG_KEYWORD, "", "labels")) {
        return false;
    }
    if (!ipp_member(b, "media-source") || !ipp_str(b, TAG_KEYWORD, "", "main")) {
        return false;
    }
    if (!ipp_member(b, "media-top-margin") || !ipp_i32(b, TAG_INTEGER, "", 0)) {
        return false;
    }
    if (!ipp_member(b, "media-bottom-margin") || !ipp_i32(b, TAG_INTEGER, "", 0)) {
        return false;
    }
    if (!ipp_member(b, "media-left-margin") || !ipp_i32(b, TAG_INTEGER, "", 0)) {
        return false;
    }
    if (!ipp_member(b, "media-right-margin") || !ipp_i32(b, TAG_INTEGER, "", 0)) {
        return false;
    }
    return ipp_end_col(b);
}

static bool begin_response(ipp_buf_t *b, uint16_t status, uint32_t req_id)
{
    memset(b, 0, sizeof(*b));
    return ipp_u16(b, 0x0200) && ipp_u16(b, status) && ipp_u32(b, req_id) && ipp_u8(b, TAG_OP) &&
           ipp_str(b, TAG_CHARSET, "attributes-charset", "utf-8") &&
           ipp_str(b, TAG_LANGUAGE, "attributes-natural-language", "en");
}

static void printer_uuid(char *dst, size_t dst_len)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_ETH);
    snprintf(dst, dst_len, "urn:uuid:%02x%02x%02x%02x-%02x%02x-4000-8000-%02x%02x%02x%02x%02x%02x", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5], mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool add_printer_attrs(ipp_buf_t *b)
{
    const char *host = net_hostname();
    char uri[96];
    char uri_ip[96];
    char uuid[64];
    char more[96];
    char ip[16] = {0};
    snprintf(uri, sizeof(uri), "ipp://%s.local:631/ipp/print", host);
    snprintf(more, sizeof(more), "http://%s.local:631/", host);
    printer_uuid(uuid, sizeof(uuid));
    bool have_ip = net_copy_ip(ip, sizeof(ip), NULL, 0);
    if (have_ip) {
        snprintf(uri_ip, sizeof(uri_ip), "ipp://%s:631/ipp/print", ip);
    }

    bool usb = usb_printer_is_ready();
    int state = usb ? (s_job_state == JOB_PROCESSING ? 4 : 3) : 5;
    const char *reason = usb ? "none" : "offline";
    int32_t uptime = (int32_t)(esp_timer_get_time() / 1000000LL);

    if (!ipp_u8(b, TAG_PRINTER)) {
        return false;
    }
    if (!ipp_str(b, TAG_URI, "printer-uri-supported", uri)) {
        return false;
    }
    if (have_ip && !ipp_str(b, TAG_URI, "", uri_ip)) {
        return false;
    }
    if (!ipp_str(b, TAG_KEYWORD, "uri-authentication-supported", "none") ||
        (have_ip && !ipp_str(b, TAG_KEYWORD, "", "none"))) {
        return false;
    }
    if (!ipp_str(b, TAG_KEYWORD, "uri-security-supported", "none") ||
        (have_ip && !ipp_str(b, TAG_KEYWORD, "", "none"))) {
        return false;
    }
    char label[64];
    (void)net_copy_printer_label(label, sizeof(label));
    const char *pname = label[0] ? label : "WiFiLabel";
    const char *pinfo = pname;
    usb_printer_identity_t ident;
    memset(&ident, 0, sizeof(ident));
    usb_printer_copy_identity(&ident);
    if (!ipp_str(b, TAG_NAME, "printer-name", pname) || !ipp_str(b, TAG_TEXT, "printer-info", pinfo) ||
        !ipp_str(b, TAG_TEXT, "printer-make-and-model", pname) ||
        !ipp_str(b, TAG_TEXT, "printer-location", "local") || !ipp_str(b, TAG_URI, "printer-uuid", uuid) ||
        !ipp_str(b, TAG_URI, "printer-more-info", more)) {
        return false;
    }
    if (ident.device_id[0] && !ipp_str(b, TAG_TEXT, "printer-device-id", ident.device_id)) {
        return false;
    }
    if (ident.firmware[0] && !ipp_str(b, TAG_TEXT, "printer-firmware-string-version", ident.firmware)) {
        return false;
    }
    if (!ipp_i32(b, TAG_ENUM, "printer-state", state) || !ipp_str(b, TAG_KEYWORD, "printer-state-reasons", reason) ||
        !ipp_bool(b, "printer-is-accepting-jobs", usb) || !ipp_i32(b, TAG_INTEGER, "printer-up-time", uptime) ||
        !ipp_i32(b, TAG_INTEGER, "queued-job-count", s_job_state == JOB_PROCESSING || s_job_state == JOB_PENDING ? 1 : 0)) {
        return false;
    }
    if (!ipp_str(b, TAG_CHARSET, "charset-configured", "utf-8") || !ipp_str(b, TAG_CHARSET, "charset-supported", "utf-8") ||
        !ipp_str(b, TAG_LANGUAGE, "natural-language-configured", "en") ||
        !ipp_str(b, TAG_LANGUAGE, "generated-natural-language-supported", "en")) {
        return false;
    }
    if (!ipp_str(b, TAG_KEYWORD, "ipp-versions-supported", "1.1") || !ipp_str(b, TAG_KEYWORD, "", "2.0")) {
        return false;
    }
    if (!ipp_str(b, TAG_KEYWORD, "ipp-features-supported", "ipp-everywhere")) {
        return false;
    }
    if (!ipp_i32(b, TAG_ENUM, "operations-supported", IPP_OP_PRINT_JOB) ||
        !ipp_i32(b, TAG_ENUM, "", IPP_OP_VALIDATE_JOB) || !ipp_i32(b, TAG_ENUM, "", IPP_OP_CREATE_JOB) ||
        !ipp_i32(b, TAG_ENUM, "", IPP_OP_SEND_DOCUMENT) || !ipp_i32(b, TAG_ENUM, "", IPP_OP_CANCEL_JOB) ||
        !ipp_i32(b, TAG_ENUM, "", IPP_OP_GET_JOB_ATTRIBUTES) || !ipp_i32(b, TAG_ENUM, "", IPP_OP_GET_JOBS) ||
        !ipp_i32(b, TAG_ENUM, "", IPP_OP_GET_PRINTER_ATTRIBUTES) || !ipp_i32(b, TAG_ENUM, "", IPP_OP_IDENTIFY_PRINTER)) {
        return false;
    }
    if (!ipp_str(b, TAG_MIMETYPE, "document-format-default", "image/pwg-raster") ||
        !ipp_str(b, TAG_MIMETYPE, "document-format-supported", "image/pwg-raster") ||
        !ipp_str(b, TAG_MIMETYPE, "", "image/urf")) {
        return false;
    }
    if (!ipp_str(b, TAG_KEYWORD, "compression-supported", "none") ||
        !ipp_str(b, TAG_KEYWORD, "pdl-override-supported", "attempted") ||
        !ipp_bool(b, "color-supported", false) || !ipp_str(b, TAG_KEYWORD, "print-color-mode-default", "monochrome") ||
        !ipp_str(b, TAG_KEYWORD, "print-color-mode-supported", "auto") ||
        !ipp_str(b, TAG_KEYWORD, "", "monochrome")) {
        return false;
    }
    if (!ipp_str(b, TAG_KEYWORD, "sides-supported", "one-sided") || !ipp_str(b, TAG_KEYWORD, "sides-default", "one-sided") ||
        !ipp_str(b, TAG_KEYWORD, "output-bin-supported", "face-up") ||
        !ipp_str(b, TAG_KEYWORD, "output-bin-default", "face-up")) {
        return false;
    }
    if (!ipp_str(b, TAG_KEYWORD, "media-default", "na_index-4x6_4x6in") ||
        !ipp_str(b, TAG_KEYWORD, "media-supported", "na_index-4x6_4x6in") ||
        !ipp_str(b, TAG_KEYWORD, "media-ready", "na_index-4x6_4x6in") ||
        !ipp_str(b, TAG_KEYWORD, "media-type-supported", "labels") ||
        !ipp_str(b, TAG_KEYWORD, "media-source-supported", "main")) {
        return false;
    }
    if (!ipp_media_col(b, "media-col-default") || !ipp_media_col(b, "media-col-ready") ||
        !ipp_media_col(b, "media-col-database")) {
        return false;
    }
    if (!ipp_str(b, TAG_KEYWORD, "media-col-supported", "media-size") || !ipp_str(b, TAG_KEYWORD, "", "media-type") ||
        !ipp_str(b, TAG_KEYWORD, "", "media-source") || !ipp_str(b, TAG_KEYWORD, "", "media-top-margin") ||
        !ipp_str(b, TAG_KEYWORD, "", "media-bottom-margin") || !ipp_str(b, TAG_KEYWORD, "", "media-left-margin") ||
        !ipp_str(b, TAG_KEYWORD, "", "media-right-margin")) {
        return false;
    }
    if (!ipp_res(b, "printer-resolution-default", 203, 203) || !ipp_res(b, "printer-resolution-supported", 203, 203) ||
        !ipp_res(b, "pwg-raster-document-resolution-supported", 203, 203)) {
        return false;
    }
    if (!ipp_str(b, TAG_KEYWORD, "pwg-raster-document-type-supported", "black_1") ||
        !ipp_str(b, TAG_KEYWORD, "", "sgray_8") || !ipp_str(b, TAG_KEYWORD, "", "srgb_8")) {
        return false;
    }
    if (!ipp_str(b, TAG_KEYWORD, "urf-supported", "V1.4") || !ipp_str(b, TAG_KEYWORD, "", "SRGB24") ||
        !ipp_str(b, TAG_KEYWORD, "", "RS203") || !ipp_str(b, TAG_KEYWORD, "", "DM1") ||
        !ipp_str(b, TAG_KEYWORD, "", "OB9")) {
        return false;
    }
    if (!ipp_str(b, TAG_NAME, "marker-colors", "#000000") || !ipp_str(b, TAG_NAME, "marker-names", "label") ||
        !ipp_str(b, TAG_KEYWORD, "marker-types", "ink") || !ipp_i32(b, TAG_INTEGER, "marker-high-levels", 100) ||
        !ipp_i32(b, TAG_INTEGER, "marker-low-levels", 10) || !ipp_i32(b, TAG_INTEGER, "marker-levels", 80)) {
        return false;
    }
    return true;
}

static bool add_job_attrs(ipp_buf_t *b, int job_id, int state)
{
    char uri[96];
    const char *reasons = "none";
    if (state == JOB_CANCELED) {
        reasons = "job-canceled-by-user";
    } else if (state == JOB_ABORTED) {
        reasons = "aborted-by-system";
    } else if (state == JOB_COMPLETED) {
        reasons = "job-completed-successfully";
    } else if (state == JOB_PROCESSING) {
        reasons = "job-printing";
    }
    snprintf(uri, sizeof(uri), "ipp://%s.local:631/ipp/print/%d", net_hostname(), job_id);
    if (!ipp_u8(b, TAG_JOB)) {
        return false;
    }
    return ipp_i32(b, TAG_INTEGER, "job-id", job_id) && ipp_str(b, TAG_URI, "job-uri", uri) &&
           ipp_i32(b, TAG_ENUM, "job-state", state) && ipp_str(b, TAG_KEYWORD, "job-state-reasons", reasons);
}

static bool finish(ipp_buf_t *b)
{
    return ipp_u8(b, TAG_END);
}

static bool format_ok(const char *fmt)
{
    if (!fmt || !fmt[0]) {
        return true;
    }
    return strcasecmp(fmt, "image/pwg-raster") == 0 || strcasecmp(fmt, "image/urf") == 0;
}

static esp_err_t page_to_usb(const uint8_t *zpl, size_t zpl_len, void *ctx)
{
    (void)ctx;
    return usb_printer_write(zpl, zpl_len);
}

static void replace_unfinished_job(void)
{
    if (s_job_state == JOB_PENDING || s_job_state == JOB_PROCESSING) {
        s_job_state = JOB_ABORTED;
    }
}

static int respond(int sock, ipp_buf_t *b)
{
    char hdr[160];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: application/ipp\r\n"
                     "Content-Length: %u\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     (unsigned)b->len);
    if (n <= 0 || !net_send_all(sock, hdr, (size_t)n)) {
        return -1;
    }
    return net_send_all(sock, b->buf, b->len) ? 0 : -1;
}

static void http_text(int sock, int code, const char *reason, const char *body)
{
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: text/plain; charset=utf-8\r\n"
                     "Content-Length: %u\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     code, reason, (unsigned)strlen(body));
    if (n > 0) {
        net_send_all(sock, hdr, (size_t)n);
        net_send_all(sock, body, strlen(body));
    }
}

static void http_status_page(int sock)
{
    char ip[16] = "0.0.0.0";
    char iface[8] = "-";
    net_copy_ip(ip, sizeof(ip), iface, sizeof(iface));
    bool ready = usb_printer_is_ready();
    usb_printer_identity_t ident;
    memset(&ident, 0, sizeof(ident));
    usb_printer_copy_identity(&ident);

    /* Identity strings are escaped, so a long device id can grow. Heap, not the IPP task stack. */
    enum { STATUS_PAGE_MAX = 5120 };
    char *body = malloc(STATUS_PAGE_MAX);
    if (!body) {
        http_text(sock, 500, "Error", "out of memory\n");
        return;
    }
    web_ui_out_t out;
    web_ui_out_init(&out, body, STATUS_PAGE_MAX);
    web_ui_open(&out);
    web_ui_text(&out, "<h1>WiFiLabel</h1><p class=\"sub\">");
    web_ui_html(&out, net_hostname());
    web_ui_text(&out, ".local</p><p class=\"pill");
    web_ui_text(&out, ready ? "" : " warn");
    web_ui_text(&out, "\">");
    web_ui_html(&out, ready ? "Ready" : "Printer not connected");
    web_ui_text(&out, "</p>");
    if (ident.mdl[0]) {
        web_ui_row(&out, "Model", ident.mdl);
    } else if (ready) {
        web_ui_row(&out, "Model", "Unknown");
    }
    if (ident.mfg[0] && strcasecmp(ident.mfg, ident.mdl) != 0) {
        web_ui_row(&out, "Manufacturer", ident.mfg);
    }
    web_ui_row(&out, "Firmware", ident.firmware);
    if (ident.cmd[0]) {
        web_ui_row(&out, "Language", ident.cmd);
    } else if (ident.zpl) {
        web_ui_row(&out, "Language", "ZPL");
    }
    char address[32];
    snprintf(address, sizeof(address), "%s %s", iface, ip);
    web_ui_row(&out, "Address", address);
    web_ui_row(&out, "IPP", "631 /ipp/print");
    web_ui_row(&out, "Raw", "TCP 9100");
    web_ui_close(&out, "status");
    int bl = (int)(out.cursor - body);
    if (out.full || bl <= 0) {
        free(body);
        http_text(sock, 500, "Error", "out of memory\n");
        return;
    }
    char hdr[160];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: %d\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     bl);
    if (n > 0) {
        net_send_all(sock, hdr, (size_t)n);
        net_send_all(sock, body, (size_t)bl);
    }
    free(body);
}

static const char *header_value(const char *headers, const char *key)
{
    size_t klen = strlen(key);
    const char *p = headers;
    while (*p) {
        if (strncasecmp(p, key, klen) == 0 && p[klen] == ':') {
            p += klen + 1;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            return p;
        }
        const char *nl = strstr(p, "\r\n");
        if (!nl) {
            break;
        }
        p = nl + 2;
    }
    return NULL;
}

static int read_headers(int sock, char *buf, size_t buflen)
{
    size_t n = 0;
    while (n + 1 < buflen) {
        /* Peek so a chunked read cannot swallow the IPP body that follows the headers. */
        int peeked = recv(sock, buf + n, buflen - 1 - n, MSG_PEEK);
        if (peeked < 0 && errno == EINTR) {
            continue;
        }
        if (peeked <= 0) {
            return -1;
        }
        buf[n + (size_t)peeked] = '\0';
        char *end = strstr(buf, "\r\n\r\n");
        size_t take = (size_t)peeked;
        if (end) {
            take = (size_t)(end + 4 - buf) - n;
        }
        int got = 0;
        while (got < (int)take) {
            int r = recv_some(sock, (uint8_t *)buf + n + (size_t)got, take - (size_t)got);
            if (r < 0 && errno == EINTR) {
                continue;
            }
            if (r <= 0) {
                return -1;
            }
            got += r;
        }
        n += take;
        buf[n] = '\0';
        if (end) {
            return 0;
        }
    }
    return -1;
}

static bool request_is_waiting(int sock)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);
    struct timeval tv = {.tv_sec = 0, .tv_usec = 400000};
    int sel = select(sock + 1, &rfds, NULL, NULL, &tv);
    return sel > 0 && FD_ISSET(sock, &rfds);
}

static void handle_client(int sock)
{
    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    /* A quiet socket must not sit in front of the browser for the old 120s timeout. */
    net_tune_accepted(sock, 5000);
    if (!request_is_waiting(sock)) {
        return;
    }

    char *headers = malloc(4096);
    if (!headers) {
        http_text(sock, 500, "Error", "out of memory\n");
        return;
    }
    if (read_headers(sock, headers, 4096) != 0) {
        free(headers);
        return;
    }

    char method[8] = {0};
    sscanf(headers, "%7s", method);
    if (strcasecmp(method, "GET") == 0) {
        http_status_page(sock);
        shutdown(sock, SHUT_WR);
        free(headers);
        return;
    }
    net_tune_accepted(sock, 120000);
    if (strcasecmp(method, "POST") != 0) {
        http_text(sock, 405, "Method Not Allowed", "POST or GET\n");
        free(headers);
        return;
    }

    const char *expect = header_value(headers, "Expect");
    if (expect && strncasecmp(expect, "100-continue", 12) == 0) {
        const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
        net_send_all(sock, cont, strlen(cont));
    }

    body_t body = {.sock = sock, .body_left = -1};
    const char *te = header_value(headers, "Transfer-Encoding");
    const char *cl = header_value(headers, "Content-Length");
    if (te && strncasecmp(te, "chunked", 7) == 0) {
        body.chunked = true;
    } else if (cl) {
        body.body_left = atoi(cl);
        if (body.body_left < 0) {
            body.body_left = 0;
        }
    } else {
        http_text(sock, 411, "Length Required", "Content-Length required\n");
        free(headers);
        return;
    }
    free(headers);

    ipp_req_t req;
    if (parse_ipp(&body, &req) != 0) {
        http_text(sock, 400, "Bad Request", "bad IPP message\n");
        return;
    }

    ipp_buf_t msg = {0};
    uint16_t status = IPP_STATUS_OK;
    bool with_printer = false;
    bool with_job = false;
    int job_id = s_job_id;
    int job_state = s_job_state;

    if (req.op == 0xffff) {
        status = IPP_STATUS_VERSION;
    } else if (req.compression[0] && strcasecmp(req.compression, "none") != 0) {
        status = IPP_STATUS_COMPRESSION;
    } else {
        switch (req.op) {
        case IPP_OP_GET_PRINTER_ATTRIBUTES:
        case IPP_OP_IDENTIFY_PRINTER:
            with_printer = (req.op == IPP_OP_GET_PRINTER_ATTRIBUTES);
            break;
        case IPP_OP_VALIDATE_JOB:
            if (!format_ok(req.format)) {
                status = IPP_STATUS_DOC_FORMAT;
            }
            break;
        case IPP_OP_CREATE_JOB:
            if (!usb_printer_is_ready()) {
                status = IPP_STATUS_NOT_ACCEPTING;
                break;
            }
            replace_unfinished_job();
            s_job_id++;
            if (s_job_id <= 0) {
                s_job_id = 1;
            }
            s_job_state = JOB_PENDING;
            s_job_copies = req.copies > 0 ? req.copies : 1;
            job_id = s_job_id;
            job_state = s_job_state;
            with_job = true;
            break;
        case IPP_OP_CANCEL_JOB:
            if (req.job_id != 0 && req.job_id != s_job_id) {
                status = IPP_STATUS_NOT_FOUND;
                break;
            }
            if (s_job_state == JOB_PENDING || s_job_state == JOB_PROCESSING) {
                s_job_state = JOB_CANCELED;
            }
            job_id = s_job_id;
            job_state = s_job_state;
            with_job = s_job_id != 0;
            break;
        case IPP_OP_GET_JOB_ATTRIBUTES:
            if (req.job_id != 0 && req.job_id != s_job_id) {
                status = IPP_STATUS_NOT_FOUND;
                break;
            }
            if (s_job_id == 0) {
                status = IPP_STATUS_NOT_FOUND;
                break;
            }
            job_id = req.job_id ? req.job_id : s_job_id;
            job_state = s_job_state;
            with_job = true;
            break;
        case IPP_OP_GET_JOBS:
            with_job = s_job_id != 0;
            job_id = s_job_id;
            job_state = s_job_state;
            break;
        case IPP_OP_PRINT_JOB:
        case IPP_OP_SEND_DOCUMENT: {
            if (!format_ok(req.format)) {
                status = IPP_STATUS_DOC_FORMAT;
                break;
            }
            if (!usb_printer_is_ready()) {
                status = IPP_STATUS_DEVICE;
                break;
            }
            if (req.op == IPP_OP_SEND_DOCUMENT && req.job_id != 0 && req.job_id != s_job_id) {
                status = IPP_STATUS_NOT_FOUND;
                break;
            }
            if (req.op == IPP_OP_PRINT_JOB) {
                replace_unfinished_job();
                s_job_id++;
                if (s_job_id <= 0) {
                    s_job_id = 1;
                }
                s_job_copies = req.copies > 0 ? req.copies : 1;
            }
            s_job_state = JOB_PROCESSING;
            int copies = s_job_copies > 0 ? s_job_copies : 1;
            esp_err_t err = ESP_OK;
            if (usb_printer_job_begin() != ESP_OK) {
                err = ESP_ERR_TIMEOUT;
            } else {
                err = raster_job_to_zpl(body_read, &body, req.format[0] ? req.format : NULL, copies, page_to_usb, NULL);
                esp_err_t end = usb_printer_job_end();
                if (err == ESP_OK) {
                    err = end;
                }
            }
            if (err == ESP_ERR_NOT_SUPPORTED) {
                status = IPP_STATUS_DOC_FORMAT_ERROR;
                s_job_state = JOB_ABORTED;
            } else if (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_TIMEOUT) {
                status = IPP_STATUS_DEVICE;
                s_job_state = JOB_ABORTED;
            } else if (err != ESP_OK) {
                status = IPP_STATUS_DOC_FORMAT_ERROR;
                s_job_state = JOB_ABORTED;
            } else if (req.op == IPP_OP_PRINT_JOB || req.last_document) {
                s_job_state = JOB_COMPLETED;
            } else {
                /* Another Send-Document may follow on a later connection. */
                s_job_state = JOB_PENDING;
            }
            job_id = s_job_id;
            job_state = s_job_state;
            with_job = true;
            break;
        }
        default:
            status = IPP_STATUS_OP_UNSUPPORTED;
            break;
        }
    }

    if (!begin_response(&msg, status, req.request_id) || (with_job && !add_job_attrs(&msg, job_id, job_state)) ||
        (with_printer && !add_printer_attrs(&msg)) || !finish(&msg)) {
        free(msg.buf);
        http_text(sock, 500, "Error", "IPP response failed\n");
        return;
    }
    bool native_fmt = req.format[0] && (strcasecmp(req.format, "image/pwg-raster") == 0 || strcasecmp(req.format, "image/urf") == 0);
    if (status == IPP_STATUS_OK && (req.op == IPP_OP_VALIDATE_JOB || req.op == IPP_OP_PRINT_JOB) && !native_fmt) {
        /* An omitted document format is sniffed, so tell the client the value was substituted. */
        msg.buf[2] = (uint8_t)(IPP_STATUS_OK_IGNORED >> 8);
        msg.buf[3] = (uint8_t)(IPP_STATUS_OK_IGNORED & 0xff);
    }
    respond(sock, &msg);
    free(msg.buf);
    ESP_LOGI(TAG, "op 0x%04x status 0x%04x job %d", req.op, status, s_job_id);
}

static void ipp_task(void *arg)
{
    (void)arg;
    while (true) {
        int listen_sock = net_listen_tcp(IPP_PORT, 8);
        if (listen_sock < 0) {
            ESP_LOGW(TAG, "bind :%d failed, errno %d; retry", IPP_PORT, errno);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ESP_LOGI(TAG, "IPP Everywhere listening on TCP %d /ipp/print", IPP_PORT);
        while (true) {
            struct sockaddr_storage src;
            socklen_t sl = sizeof(src);
            int sock = accept(listen_sock, (struct sockaddr *)&src, &sl);
            if (sock < 0) {
                ESP_LOGE(TAG, "accept errno %d", errno);
                break;
            }
            handle_client(sock);
            shutdown(sock, SHUT_RDWR);
            close(sock);
        }
        close(listen_sock);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t ipp_server_start(void)
{
    if (xTaskCreate(ipp_task, "ipp", 12288, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
