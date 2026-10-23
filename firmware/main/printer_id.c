#include "printer_id.h"

#include <stdio.h>
#include <string.h>

static bool key_eq(const char *s, size_t n, const char *key)
{
    size_t k = strlen(key);
    if (n != k) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char a = s[i];
        char b = key[i];
        if (a >= 'a' && a <= 'z') {
            a = (char)(a - 'a' + 'A');
        }
        if (b >= 'a' && b <= 'z') {
            b = (char)(b - 'a' + 'A');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

static void field_copy(const char *id, const char *const *keys, char *dst, size_t dst_len)
{
    dst[0] = '\0';
    if (!id || dst_len == 0) {
        return;
    }
    const char *p = id;
    while (*p) {
        while (*p == ' ' || *p == ';') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *semi = strchr(p, ';');
        const char *end = semi ? semi : p + strlen(p);
        const char *colon = NULL;
        for (const char *c = p; c < end; c++) {
            if (*c == ':') {
                colon = c;
                break;
            }
        }
        if (colon) {
            const char *ks = p;
            const char *ke = colon;
            while (ks < ke && (*ks == ' ' || *ks == '\t')) {
                ks++;
            }
            while (ke > ks && (ke[-1] == ' ' || ke[-1] == '\t')) {
                ke--;
            }
            bool match = false;
            for (int i = 0; keys[i]; i++) {
                if (key_eq(ks, (size_t)(ke - ks), keys[i])) {
                    match = true;
                    break;
                }
            }
            if (match) {
                const char *vs = colon + 1;
                const char *ve = end;
                while (vs < ve && (*vs == ' ' || *vs == '\t')) {
                    vs++;
                }
                while (ve > vs && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\r' || ve[-1] == '\n')) {
                    ve--;
                }
                size_t n = (size_t)(ve - vs);
                if (n >= dst_len) {
                    n = dst_len - 1;
                }
                memcpy(dst, vs, n);
                dst[n] = '\0';
                return;
            }
        }
        if (!semi) {
            break;
        }
        p = semi + 1;
    }
}

/* CMD lists look like "ZPL,EPL". A token that starts with ZPL counts. "EZPL" does not. */
static bool cmd_has_zpl(const char *cmd)
{
    const char *p = cmd ? cmd : "";
    while (*p) {
        while (*p == ' ' || *p == ',') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *start = p;
        while (*p && *p != ',') {
            p++;
        }
        const char *e = p;
        while (e > start && e[-1] == ' ') {
            e--;
        }
        if ((size_t)(e - start) < 3) {
            continue;
        }
        char a = start[0];
        char b = start[1];
        char c = start[2];
        if (a >= 'a' && a <= 'z') {
            a = (char)(a - 'a' + 'A');
        }
        if (b >= 'a' && b <= 'z') {
            b = (char)(b - 'a' + 'A');
        }
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        if (a == 'Z' && b == 'P' && c == 'L') {
            return true;
        }
    }
    return false;
}

void printer_id_scrub(char *s)
{
    if (!s) {
        return;
    }
    for (char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || c > 126) {
            *p = '?';
        }
    }
}

void printer_id_parse(usb_printer_identity_t *id)
{
    if (!id) {
        return;
    }
    printer_id_scrub(id->device_id);
    id->mfg[0] = '\0';
    id->mdl[0] = '\0';
    id->cmd[0] = '\0';
    id->firmware[0] = '\0';
    id->zpl = false;
    id->present = false;
    if (!id->device_id[0]) {
        return;
    }

    static const char *const k_mfg[] = {"MFG", "MANUFACTURER", NULL};
    static const char *const k_mdl[] = {"MDL", "MODEL", NULL};
    static const char *const k_cmd[] = {"CMD", "COMMAND SET", NULL};
    field_copy(id->device_id, k_mfg, id->mfg, sizeof(id->mfg));
    field_copy(id->device_id, k_mdl, id->mdl, sizeof(id->mdl));
    field_copy(id->device_id, k_cmd, id->cmd, sizeof(id->cmd));
    id->zpl = cmd_has_zpl(id->cmd);
    id->present = id->mfg[0] || id->mdl[0] || id->device_id[0];
}

void printer_id_take_firmware(const uint8_t *buf, int n, char *fw, size_t fw_len)
{
    fw[0] = '\0';
    if (!buf || n <= 0 || fw_len < 2) {
        return;
    }
    int i = 0;
    while (i < n && buf[i] < 32) {
        i++;
    }
    int commas = 0;
    int fs = -1;
    int fe = n;
    for (; i < n; i++) {
        unsigned char c = buf[i];
        if (c == 0x03 || c == '\r' || c == '\n') {
            fe = i;
            break;
        }
        if (c == ',') {
            commas++;
            if (commas == 1) {
                fs = i + 1;
            } else if (commas == 2) {
                fe = i;
                break;
            }
        }
    }
    if (commas < 1 || fs < 0 || fs >= fe) {
        return;
    }
    while (fs < fe && buf[fs] == ' ') {
        fs++;
    }
    while (fe > fs && buf[fe - 1] == ' ') {
        fe--;
    }
    size_t len = (size_t)(fe - fs);
    if (len == 0) {
        return;
    }
    if (len >= fw_len) {
        len = fw_len - 1;
    }
    for (size_t k = 0; k < len; k++) {
        unsigned char c = buf[fs + (int)k];
        if (c < 32 || c > 126) {
            fw[0] = '\0';
            return;
        }
        fw[k] = (char)c;
    }
    fw[len] = '\0';
}

const char *printer_id_name_base(const usb_printer_identity_t *id, bool *from_printer)
{
    bool real = false;
    const char *base = "WiFiLabel";
    if (id && id->mdl[0]) {
        base = id->mdl;
        real = true;
    } else if (id && id->mfg[0]) {
        base = id->mfg;
        real = true;
    }
    if (from_printer) {
        *from_printer = real;
    }
    return base;
}

void printer_id_format_name(char *dst, size_t dst_len, const char *brand, const char *model)
{
    if (!dst || dst_len == 0) {
        return;
    }
    if (!brand || !brand[0]) {
        brand = "WiFiLabel";
    }
    if (!model || !model[0]) {
        snprintf(dst, dst_len, "%.*s", 63, brand);
        return;
    }
    char combined[128];
    snprintf(combined, sizeof(combined), "%s %s", brand, model);
    if (strlen(combined) <= 63) {
        snprintf(dst, dst_len, "%s", combined);
        return;
    }
    int keep = 63 - 1 - (int)strlen(brand);
    if (keep < 1) {
        keep = 1;
    }
    snprintf(dst, dst_len, "%s %.*s", brand, keep, model);
}
