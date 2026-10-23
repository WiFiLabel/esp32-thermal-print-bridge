#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Shared document shell for the status page and the setup portal.
 * web_ui_open writes the doctype, the stylesheet, and the opening card.
 * Callers then write the page body. web_ui_close writes the site link and the end of the document.
 * The stylesheet is one copy in flash. No outside fonts, images, or scripts. */

typedef struct {
    char *cursor;
    size_t left;
    bool full;
} web_ui_out_t;

void web_ui_out_init(web_ui_out_t *out, char *storage, size_t storage_len);
void web_ui_text(web_ui_out_t *out, const char *s);
void web_ui_html(web_ui_out_t *out, const char *s);
void web_ui_open(web_ui_out_t *out);
void web_ui_close(web_ui_out_t *out);
void web_ui_row(web_ui_out_t *out, const char *label, const char *value);
