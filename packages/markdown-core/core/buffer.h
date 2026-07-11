#ifndef MARKDOWN_CORE_BUFFER_H
#define MARKDOWN_CORE_BUFFER_H

#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include "config.h"
#include "markdown-core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    markdown_core_mem *mem;
    unsigned char *ptr;
    bufsize_t asize, size;
} markdown_core_strbuf;

extern unsigned char markdown_core_strbuf__initbuf[];

#define MARKDOWN_CORE_BUF_INIT(mem) {mem, markdown_core_strbuf__initbuf, 0, 0}

/**
 * Initialize a markdown_core_strbuf structure.
 *
 * For the cases where MARKDOWN_CORE_BUF_INIT cannot be used to do static
 * initialization.
 */
MARKDOWN_CORE_EXPORT
void markdown_core_strbuf_init(markdown_core_mem *mem, markdown_core_strbuf *buf, bufsize_t initial_size);

/**
 * Grow the buffer to hold at least `target_size` bytes.
 */
MARKDOWN_CORE_EXPORT
void markdown_core_strbuf_grow(markdown_core_strbuf *buf, bufsize_t target_size);

MARKDOWN_CORE_EXPORT
void markdown_core_strbuf_free(markdown_core_strbuf *buf);

MARKDOWN_CORE_EXPORT
void markdown_core_strbuf_swap(markdown_core_strbuf *buf_a, markdown_core_strbuf *buf_b);

MARKDOWN_CORE_EXPORT
bufsize_t markdown_core_strbuf_len(const markdown_core_strbuf *buf);

MARKDOWN_CORE_EXPORT
int markdown_core_strbuf_cmp(const markdown_core_strbuf *a, const markdown_core_strbuf *b);

MARKDOWN_CORE_EXPORT
unsigned char *markdown_core_strbuf_detach(markdown_core_strbuf *buf);

MARKDOWN_CORE_EXPORT
void markdown_core_strbuf_copy_cstr(char *data, bufsize_t datasize, const markdown_core_strbuf *buf);

static MARKDOWN_CORE_INLINE const char *markdown_core_strbuf_cstr(const markdown_core_strbuf *buf) {
    return (char *)buf->ptr;
}

#define markdown_core_strbuf_at(buf, n) ((buf)->ptr[n])

MARKDOWN_CORE_EXPORT
void markdown_core_strbuf_set(markdown_core_strbuf *buf, const unsigned char *data, bufsize_t len);

MARKDOWN_CORE_EXPORT
void markdown_core_strbuf_sets(markdown_core_strbuf *buf, const char *string);

MARKDOWN_CORE_EXPORT
void markdown_core_strbuf_putc(markdown_core_strbuf *buf, int c);

MARKDOWN_CORE_EXPORT
void markdown_core_strbuf_put(markdown_core_strbuf *buf, const unsigned char *data, bufsize_t len);

MARKDOWN_CORE_EXPORT
void markdown_core_strbuf_puts(markdown_core_strbuf *buf, const char *string);

MARKDOWN_CORE_EXPORT
void markdown_core_strbuf_clear(markdown_core_strbuf *buf);

MARKDOWN_CORE_EXPORT
bufsize_t markdown_core_strbuf_strchr(const markdown_core_strbuf *buf, int c, bufsize_t pos);

MARKDOWN_CORE_EXPORT
bufsize_t markdown_core_strbuf_strrchr(const markdown_core_strbuf *buf, int c, bufsize_t pos);

MARKDOWN_CORE_EXPORT
void markdown_core_strbuf_drop(markdown_core_strbuf *buf, bufsize_t n);

MARKDOWN_CORE_EXPORT
void markdown_core_strbuf_truncate(markdown_core_strbuf *buf, bufsize_t len);

MARKDOWN_CORE_EXPORT
void markdown_core_strbuf_rtrim(markdown_core_strbuf *buf);

MARKDOWN_CORE_EXPORT
void markdown_core_strbuf_trim(markdown_core_strbuf *buf);

MARKDOWN_CORE_EXPORT
void markdown_core_strbuf_normalize_whitespace(markdown_core_strbuf *s);

MARKDOWN_CORE_EXPORT
void markdown_core_strbuf_unescape(markdown_core_strbuf *s);

#ifdef __cplusplus
}
#endif

#endif
