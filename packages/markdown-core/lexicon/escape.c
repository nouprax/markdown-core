#include "escape.h"
#include "markdown_core_ctype.h"

void markdown_core_strbuf_unescape(markdown_core_strbuf *buf) {
    bufsize_t r, w;

    markdown_core_strbuf_unsettle(buf, 0);
    for (r = 0, w = 0; r < buf->size; ++r) {
        if (buf->ptr[r] == '\\' && markdown_core_ispunct(buf->ptr[r + 1])) {
            r++;
        }

        buf->ptr[w++] = buf->ptr[r];
    }

    markdown_core_strbuf_truncate(buf, w);
}
