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
    /* Storage the buffer does not own (a parse transaction's arena): never
     * freed or reallocated by the buffer, which copies its bytes out to
     * storage of its own the first time it has to grow, and copies them out
     * when detached. */
    bool borrowed;
} markdown_core_strbuf;

extern const unsigned char markdown_core_strbuf__initbuf[];

#define MARKDOWN_CORE_BUF_INIT(mem)                                                                                    \
    { mem, (unsigned char *)markdown_core_strbuf__initbuf, 0, 0, 0, false }

/**
 * Grow the buffer to hold at least `target_size` bytes.
 */
MARKDOWN_CORE_EXPORT
void markdown_core_strbuf_grow(markdown_core_strbuf *buf, bufsize_t target_size);

/* THE PRIMITIVES EVERY HOT PATH TOUCHES ARE DEFINED HERE, so each unit
 * inlines them instead of calling across the archive; growth, the slow
 * path, is the one call that remains. */

/* `bufsize_t` is int32_t, so `buf->size + add` is undefined behaviour once the
 * sum passes INT32_MAX -- and the wrapped result is NEGATIVE, which
 * markdown_core_strbuf_grow used to read as "already big enough". The caller
 * then wrote `add` bytes past the end of a buffer that had not grown. Test
 * against the room the cap leaves, BEFORE adding, so the sum never happens. */
static MARKDOWN_CORE_INLINE void markdown_core_strbuf__grow_by(markdown_core_strbuf *buf, bufsize_t add) {
    if (add < 0 || add > (bufsize_t)(INT32_MAX / 2) - buf->size) {
        buf->oom = 1;
        return;
    }
    markdown_core_strbuf_grow(buf, buf->size + add);
}

/**
 * Initialize a markdown_core_strbuf structure.
 *
 * For the cases where MARKDOWN_CORE_BUF_INIT cannot be used to do static
 * initialization.
 */
static MARKDOWN_CORE_INLINE void markdown_core_strbuf_init(markdown_core_mem *mem, markdown_core_strbuf *buf,
                                                           bufsize_t initial_size) {
    buf->mem = mem;
    buf->asize = 0;
    buf->size = 0;
    buf->oom = 0;
    buf->borrowed = false;
    /* The cast drops const and nothing writes through it: `asize` is 0 exactly
     * while `ptr` is this sentinel, and every write path either grows first or
     * is guarded by `asize > 0`. */
    buf->ptr = (unsigned char *)markdown_core_strbuf__initbuf;

    if (initial_size > 0) {
        markdown_core_strbuf_grow(buf, initial_size);
    }
}

static MARKDOWN_CORE_INLINE bufsize_t markdown_core_strbuf_len(const markdown_core_strbuf *buf) { return buf->size; }

static MARKDOWN_CORE_INLINE void markdown_core_strbuf_free(markdown_core_strbuf *buf) {
    if (!buf) {
        return;
    }
    if (buf->ptr != markdown_core_strbuf__initbuf && !buf->borrowed) {
        buf->mem->free(buf->ptr);
    }
    markdown_core_strbuf_init(buf->mem, buf, 0);
}

/* Give an empty buffer `capacity` bytes of storage it does not own (see
 * `borrowed`), which outlives the buffer: an arena's. */
static MARKDOWN_CORE_INLINE void markdown_core_strbuf_borrow(markdown_core_strbuf *buf, unsigned char *storage,
                                                             bufsize_t capacity) {
    markdown_core_strbuf_free(buf);
    buf->ptr = storage;
    buf->asize = capacity;
    buf->borrowed = true;
    storage[0] = '\0';
}

static MARKDOWN_CORE_INLINE void markdown_core_strbuf_clear(markdown_core_strbuf *buf) {
    buf->size = 0;

    /* An allocation failure is a fact about the write that failed, not a
     * property the buffer keeps. `oom` says "content was lost"; after a clear
     * there is no content, so there is nothing left for it to say. It used to
     * survive here, and `markdown_core_strbuf_detach` was the only operation
     * that lifted it -- so a buffer cleared and reused across lines silently
     * dropped every later write with the allocator working again. */
    buf->oom = 0;

    if (buf->asize > 0) {
        buf->ptr[0] = '\0';
    }
}

static MARKDOWN_CORE_INLINE void markdown_core_strbuf_putc(markdown_core_strbuf *buf, int c) {
    markdown_core_strbuf__grow_by(buf, 1);
    if (buf->oom) {
        return;
    }
    buf->ptr[buf->size++] = (unsigned char)(c & 0xFF);
    buf->ptr[buf->size] = '\0';
}

static MARKDOWN_CORE_INLINE void markdown_core_strbuf_put(markdown_core_strbuf *buf, const unsigned char *data,
                                                          bufsize_t len) {
    if (len <= 0) {
        return;
    }
    markdown_core_strbuf__grow_by(buf, len);
    if (buf->oom) {
        return;
    }
    memmove(buf->ptr + buf->size, data, len);
    buf->size += len;
    buf->ptr[buf->size] = '\0';
}

static MARKDOWN_CORE_INLINE void markdown_core_strbuf_puts(markdown_core_strbuf *buf, const char *string) {
    markdown_core_strbuf_put(buf, (const unsigned char *)string, (bufsize_t)strlen(string));
}

MARKDOWN_CORE_EXPORT
void markdown_core_strbuf_swap(markdown_core_strbuf *buf_a, markdown_core_strbuf *buf_b);

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
