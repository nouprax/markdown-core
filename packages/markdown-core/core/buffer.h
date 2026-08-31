#ifndef MARKDOWN_CORE_BUFFER_H
#define MARKDOWN_CORE_BUFFER_H

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include "config.h"
#include "markdown-core.h"
#include "markdown_core_atomic.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Every buffer carries a sticky `oom` poison bit: when growth fails (either
 * the allocator returned NULL or the 2 GiB size limit was hit), the bit is
 * set, the previous contents stay valid and NUL-terminated, and every later
 * mutation becomes a no-op.  Consumers observe the loss at the boundaries --
 * markdown_core_strbuf_detach returns NULL for a poisoned buffer -- so
 * allocation failure degrades into a reported parse failure instead of
 * undefined behavior. */
typedef struct {
    markdown_core_mem *mem;
    unsigned char *ptr;
    bufsize_t asize, size;
    int oom;
} markdown_core_strbuf;

extern const unsigned char markdown_core_strbuf__initbuf[];

#define MARKDOWN_CORE_BUF_INIT(mem) {mem, (unsigned char *)markdown_core_strbuf__initbuf, 0, 0, 0}

/* THE FROZEN BUFFER (#153). A reference-counted, immutable run of bytes:
 * the shape every ownership wound in this repository shares is "immutable
 * bytes shared across lifetimes", and this is the one type that answers it.
 * It is a leaf object -- it points at nothing that points back, so there
 * are no cycles and no weak edges, and freezing is the last write: after
 * `markdown_core_buf_freeze` the bytes never change, only the count does.
 * A `markdown_core_chunk` whose `owner` names one of these keeps it alive
 * (chunk.h); the count is C11-atomic (markdown_core_atomic.h) so a
 * document freed on another thread releases safely. */
typedef struct markdown_core_buf {
    markdown_core_mem *mem;
    markdown_core_atomic_u32 refs;
    unsigned char *bytes;
    bufsize_t size;
} markdown_core_buf;

/* Freeze a strbuf's bytes into a new buffer with one reference. The strbuf
 * is reset to empty either way. Returns NULL when the strbuf was poisoned
 * or the header cannot be allocated -- the caller reports OOM exactly as it
 * would for a failed detach. The bytes keep the strbuf's trailing NUL. */
MARKDOWN_CORE_EXPORT
markdown_core_buf *markdown_core_buf_freeze(markdown_core_strbuf *buf);

MARKDOWN_CORE_EXPORT
void markdown_core_buf_retain(markdown_core_buf *buf);

MARKDOWN_CORE_EXPORT
void markdown_core_buf_release(markdown_core_buf *buf);

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
unsigned char *markdown_core_strbuf_detach(markdown_core_strbuf *buf);

static MARKDOWN_CORE_INLINE const char *markdown_core_strbuf_cstr(const markdown_core_strbuf *buf) {
    return (char *)buf->ptr;
}

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
void markdown_core_strbuf_drop(markdown_core_strbuf *buf, bufsize_t n);

MARKDOWN_CORE_EXPORT
void markdown_core_strbuf_truncate(markdown_core_strbuf *buf, bufsize_t len);

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
