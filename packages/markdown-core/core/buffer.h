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

/* The most bytes a buffer's content may hold. A write that would take the
 * content past it poisons the buffer, whatever room the allocation has. */
#define MARKDOWN_CORE_STRBUF_LIMIT ((bufsize_t)(INT32_MAX / 2))

/* Every buffer carries a sticky `oom` poison bit: when growth fails (either
 * the allocator returned NULL or MARKDOWN_CORE_STRBUF_LIMIT was hit), the bit
 * is set, the previous contents stay valid and NUL-terminated, and every later
 * mutation becomes a no-op.  Consumers observe the loss at the boundaries --
 * markdown_core_strbuf_detach returns NULL for a poisoned buffer -- so
 * allocation failure degrades into a reported parse failure instead of
 * undefined behavior. */
typedef struct {
    unsigned char *ptr;
    bufsize_t asize, size;
    int oom;
} markdown_core_strbuf;

extern const unsigned char markdown_core_strbuf__initbuf[];

#define MARKDOWN_CORE_BUF_INIT() {(unsigned char *)markdown_core_strbuf__initbuf, 0, 0, 0}

/**
 * Initialize a markdown_core_strbuf structure.
 *
 * For the cases where MARKDOWN_CORE_BUF_INIT cannot be used to do static
 * initialization.
 */
MARKDOWN_CORE_EXPORT
void markdown_core_strbuf_init(markdown_core_strbuf *buf, bufsize_t initial_size);

/* THE EMPTY BUFFER, over storage that is ALREADY ZERO.
 *
 * `markdown_core_strbuf_init` writes four fields. Over storage that came from
 * `markdown_core_alloc` -- which is `calloc` -- three of them already hold the
 * value it writes, so the call spends its cost re-establishing zero. The one
 * field a zeroed buffer does not already satisfy is `ptr`: this type's
 * invariant is that `ptr` always addresses readable bytes, the shared
 * one-byte sentinel while `asize == 0`, and a zeroed `ptr` is NULL.
 *
 * Only for storage the caller knows is zeroed. Anything else -- a stack
 * buffer, a reused one, one that has held bytes -- wants
 * `markdown_core_strbuf_init`, which does not assume. */
static inline void markdown_core_strbuf_init_zeroed(markdown_core_strbuf *buf) {
    buf->ptr = (unsigned char *)markdown_core_strbuf__initbuf;
}

/**
 * Grow the buffer to hold at least `target_size` bytes.
 */
MARKDOWN_CORE_EXPORT
void markdown_core_strbuf_grow(markdown_core_strbuf *buf, bufsize_t target_size);

MARKDOWN_CORE_EXPORT
/* Whether the buffer owns storage: a buffer still on the shared initial
 * storage owns nothing. The first test a release makes, shared with the
 * node release that makes it in place before calling. */
static MARKDOWN_CORE_INLINE bool markdown_core_strbuf_owns(const markdown_core_strbuf *buf) {
    return buf->ptr != markdown_core_strbuf__initbuf;
}
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

/* The append that needs growth, or meets a poisoned buffer: the half of
 * `markdown_core_strbuf_put` that is not inline. It grows, poisons, or does
 * nothing, as the buffer's contract says. */
void markdown_core_strbuf_put_grown(markdown_core_strbuf *buf, const unsigned char *data, bufsize_t len);

/* APPEND, WITH THE COMMON CASE INLINE. Growth oversizes by half, so most
 * appends fit the room the buffer already has, and those are a copy and a
 * terminator, here at the call. An append that does not fit, or meets a
 * poisoned buffer, takes the call above. Room never reaches past the content
 * limit -- `markdown_core_strbuf_grow` caps the allocation at the limit and
 * its terminator -- so fitting the room is fitting the limit, and one
 * comparison decides both. */
static MARKDOWN_CORE_INLINE void markdown_core_strbuf_put(markdown_core_strbuf *buf, const unsigned char *data,
                                                          bufsize_t len) {
    if (len <= 0) {
        return;
    }
    if (len < buf->asize - buf->size && !buf->oom) {
        memmove(buf->ptr + buf->size, data, (size_t)len);
        buf->size += len;
        buf->ptr[buf->size] = '\0';
        return;
    }
    markdown_core_strbuf_put_grown(buf, data, len);
}

static MARKDOWN_CORE_INLINE void markdown_core_strbuf_putc(markdown_core_strbuf *buf, int c) {
    unsigned char byte = (unsigned char)(c & 0xFF);
    if (buf->size + 1 < buf->asize && !buf->oom) {
        buf->ptr[buf->size++] = byte;
        buf->ptr[buf->size] = '\0';
        return;
    }
    markdown_core_strbuf_put_grown(buf, &byte, 1);
}

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
void markdown_core_strbuf_unescape(markdown_core_strbuf *s);

#ifdef __cplusplus
}
#endif

#endif
