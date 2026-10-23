#include "web_ui.h"

#include <string.h>

static const char WEB_UI_HEAD[] =
    "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>WiFiLabel</title><style>";

static const char WEB_UI_CSS[] =
    ":root{--bg:#f3f1eb;--ink:#1c1915;--muted:#4a453c;--line:#ddd6c8;--card:#fffcf7;"
    "--accent:#0f5132;--on:#f4f7f4;--ok:#0f5132;--okbg:#e7f2ec;--warn:#6a3d09;--warnbg:#f6ead8}"
    "@media (prefers-color-scheme:dark){:root{--bg:#1c1915;--ink:#f4f7f4;--muted:#c8c0b4;--line:#3d362e;"
    "--card:#28241e;--accent:#3d9a6a;--on:#07140e;--ok:#c6ead6;--okbg:#1c3328;--warn:#f0d7b0;--warnbg:#3a2e1c}}"
    "*{box-sizing:border-box}"
    "html{color-scheme:light dark}"
    "html,body{margin:0;background:var(--bg);color:var(--ink)}"
    "body{font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",Roboto,Helvetica,Arial,sans-serif;"
    "font-size:16px;line-height:1.45}"
    ".wrap{max-width:28rem;margin:0 auto;padding:1.5rem 1rem 2.5rem}"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:1.25rem 1.25rem 1.4rem}"
    "h1{margin:0 0 .2rem;font-size:1.5rem;font-weight:600;letter-spacing:-.02em}"
    ".sub{margin:0 0 1.1rem;color:var(--muted);font-size:.95rem}"
    ".pill{display:inline-block;margin:0 0 1.1rem;padding:.28rem .7rem;border-radius:999px;"
    "background:var(--okbg);color:var(--ok);font-size:.85rem;font-weight:600}"
    ".pill.warn{background:var(--warnbg);color:var(--warn)}"
    ".row{display:flex;justify-content:space-between;gap:1rem;padding:.7rem 0;border-top:1px solid var(--line)}"
    ".row span{color:var(--muted)}.row b{font-weight:600;text-align:right;overflow-wrap:anywhere;min-width:0}"
    ".foot{margin:1rem 0 0;padding-top:.85rem;border-top:1px solid var(--line);text-align:center}"
    ".foot a{display:inline-block;padding:.35rem .5rem;color:var(--accent);font-weight:600;text-decoration:none}"
    "label{display:block;margin:0 0 .85rem;font-weight:600}"
    "input,button{-webkit-appearance:none;appearance:none}"
    "input{display:block;width:100%;margin-top:.35rem;min-height:44px;padding:.55rem .75rem;"
    "border:1px solid var(--line);border-radius:10px;background:var(--bg);color:var(--ink);font:inherit}"
    "button{display:block;width:100%;min-height:44px;margin-top:.25rem;border:0;border-radius:10px;"
    "background:var(--accent);color:var(--on);font:inherit;font-weight:600;cursor:pointer}"
    ".hint{margin:0 0 1rem;color:var(--muted);font-size:.95rem}"
    ".banner{margin:0 0 1rem;padding:.7rem .8rem;border-radius:10px;background:var(--warnbg);color:var(--warn)}"
    "p{margin:0 0 .8rem}"
    ":focus-visible{outline:2px solid var(--accent);outline-offset:3px}";

static const char WEB_UI_CARD[] = "</style></head><body><main class=\"wrap\"><section class=\"card\">";

static const char WEB_UI_END_TAIL[] = "\" target=\"_blank\" rel=\"noopener noreferrer\">wifilabel.com</a></p>"
                                       "</section></main></body></html>";

void web_ui_out_init(web_ui_out_t *out, char *storage, size_t storage_len)
{
    out->cursor = storage;
    out->left = storage_len;
    out->full = !storage || storage_len == 0;
    if (!out->full) {
        storage[0] = '\0';
    }
}

void web_ui_text(web_ui_out_t *out, const char *s)
{
    if (!out || out->full || !s) {
        return;
    }
    size_t n = strlen(s);
    if (n >= out->left) {
        out->full = true;
        return;
    }
    memcpy(out->cursor, s, n);
    out->cursor += n;
    out->left -= n;
    *out->cursor = '\0';
}

void web_ui_html(web_ui_out_t *out, const char *s)
{
    char one[2] = {0, 0};
    if (!s) {
        return;
    }
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 32) {
            continue;
        }
        if (c == '&') {
            web_ui_text(out, "&amp;");
        } else if (c == '<') {
            web_ui_text(out, "&lt;");
        } else if (c == '>') {
            web_ui_text(out, "&gt;");
        } else if (c == '"') {
            web_ui_text(out, "&quot;");
        } else {
            one[0] = (char)c;
            web_ui_text(out, one);
        }
    }
}

void web_ui_open(web_ui_out_t *out)
{
    web_ui_text(out, WEB_UI_HEAD);
    web_ui_text(out, WEB_UI_CSS);
    web_ui_text(out, WEB_UI_CARD);
}

void web_ui_close(web_ui_out_t *out, const char *page)
{
    if (!page || !page[0]) {
        page = "device";
    }
    web_ui_text(out, "<p class=\"foot\"><a href=\"https://wifilabel.com/?utm_source=bridge&amp;utm_medium=page&amp;utm_content=");
    web_ui_html(out, page);
    web_ui_text(out, WEB_UI_END_TAIL);
}

void web_ui_row(web_ui_out_t *out, const char *label, const char *value)
{
    if (!value || !value[0]) {
        return;
    }
    web_ui_text(out, "<div class=\"row\"><span>");
    web_ui_html(out, label);
    web_ui_text(out, "</span><b>");
    web_ui_html(out, value);
    web_ui_text(out, "</b></div>");
}
