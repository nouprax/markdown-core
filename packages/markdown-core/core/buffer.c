#include <stdarg.h>
#include <string.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>

#include "config.h"
#include "markdown_core_ctype.h"
#include "buffer.h"

/* Used as default value for markdown_core_strbuf->ptr so that people can always
 * assume ptr is non-NULL and zero terminated even for new markdown_core_strbufs.
 */
const unsigned char markdown_core_strbuf__initbuf[1] = {0};

#ifndef MIN
#define MIN(x, y) ((x < y) ? x : y)
#endif

void markdown_core_strbuf_init(markdown_core_mem *mem, markdown_core_strbuf *buf, bufsize_t initial_size) {
    buf->mem = mem;
    buf->asize = 0;
    buf->size = 0;
    buf->oom = 0;
    /* The cast drops const and nothing writes through it: `asize` is 0 exactly
     * while `ptr` is this sentinel, and every write path either grows first or
     * is guarded by `asize > 0`. */
    buf->ptr = (unsigned char *)markdown_core_strbuf__initbuf;

    if (initial_size > 0) {
        markdown_core_strbuf_grow(buf, initial_size);
    }
}

/* `bufsize_t` is int32_t, so `buf->size + add` is undefined behaviour once the
 * sum passes INT32_MAX -- and the wrapped result is NEGATIVE, which
 * markdown_core_strbuf_grow used to read as "already big enough". The caller
 * then wrote `add` bytes past the end of a buffer that had not grown. Test
 * against the room the cap leaves, BEFORE adding, so the sum never happens. */
static MARKDOWN_CORE_INLINE void S_strbuf_grow_by(markdown_core_strbuf *buf, bufsize_t add) {
    if (add < 0 || add > (bufsize_t)(INT32_MAX / 2) - buf->size) {
        buf->oom = 1;
        return;
    }
    markdown_core_strbuf_grow(buf, buf->size + add);
}

void markdown_core_strbuf_grow(markdown_core_strbuf *buf, bufsize_t target_size) {
    /* A non-positive target is a caller error, and it must poison rather than
     * assert: the assert compiles out under NDEBUG, and the `target_size <
     * buf->asize` test below then answers a negative target with "nothing to
     * do" -- silently, with the buffer unchanged and its failure bit clear. */
    if (target_size <= 0) {
        buf->oom = 1;
        return;
    }

    if (buf->oom || target_size < buf->asize) {
        return;
    }

    /* Both the size cap and allocator failure poison the buffer instead of
     * aborting; existing contents stay valid and later writes are no-ops. */
    if (target_size > (bufsize_t)(INT32_MAX / 2)) {
        buf->oom = 1;
        return;
    }

    /* Oversize the buffer by 50% to guarantee amortized linear time
     * complexity on append operations. */
    bufsize_t new_size = target_size + target_size / 2;
    new_size += 1;
    new_size = (new_size + 7) & ~7;

    unsigned char *new_ptr = (unsigned char *)buf->mem->realloc(buf->asize ? buf->ptr : NULL, new_size);
    if (!new_ptr) {
        buf->oom = 1;
        return;
    }
    buf->ptr = new_ptr;
    buf->asize = new_size;
}

void markdown_core_strbuf_free(markdown_core_strbuf *buf) {
    if (!buf) {
        return;
    }

    if (buf->ptr != markdown_core_strbuf__initbuf) {
        buf->mem->free(buf->ptr);
    }

    markdown_core_strbuf_init(buf->mem, buf, 0);
}

void markdown_core_strbuf_clear(markdown_core_strbuf *buf) {
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

void markdown_core_strbuf_set(markdown_core_strbuf *buf, const unsigned char *data, bufsize_t len) {
    if (len <= 0 || data == NULL) {
        markdown_core_strbuf_clear(buf);
    } else {
        if (data != buf->ptr) {
            if (len >= buf->asize) {
                markdown_core_strbuf_grow(buf, len);
            }
            if (buf->oom || len >= buf->asize) {
                return;
            }
            memmove(buf->ptr, data, len);
        }
        buf->size = len;
        buf->ptr[buf->size] = '\0';
    }
}

void markdown_core_strbuf_sets(markdown_core_strbuf *buf, const char *string) {
    markdown_core_strbuf_set(buf, (const unsigned char *)string, string ? (bufsize_t)strlen(string) : 0);
}

void markdown_core_strbuf_putc(markdown_core_strbuf *buf, int c) {
    S_strbuf_grow_by(buf, 1);
    if (buf->oom) {
        return;
    }
    buf->ptr[buf->size++] = (unsigned char)(c & 0xFF);
    buf->ptr[buf->size] = '\0';
}

void markdown_core_strbuf_put(markdown_core_strbuf *buf, const unsigned char *data, bufsize_t len) {
    if (len <= 0) {
        return;
    }

    S_strbuf_grow_by(buf, len);
    if (buf->oom) {
        return;
    }
    memmove(buf->ptr + buf->size, data, len);
    buf->size += len;
    buf->ptr[buf->size] = '\0';
}

void markdown_core_strbuf_puts(markdown_core_strbuf *buf, const char *string) {
    markdown_core_strbuf_put(buf, (const unsigned char *)string, (bufsize_t)strlen(string));
}

unsigned char *markdown_core_strbuf_detach(markdown_core_strbuf *buf) {
    unsigned char *data = buf->ptr;

    /* A poisoned buffer has lost content; hand the loss to the caller as
     * NULL instead of a silently truncated string. */
    if (buf->oom) {
        markdown_core_strbuf_free(buf);
        return NULL;
    }

    if (buf->asize == 0) {
        /* return an empty string; NULL reports allocation failure */
        return (unsigned char *)buf->mem->calloc(1, 1);
    }

    markdown_core_strbuf_init(buf->mem, buf, 0);
    return data;
}

void markdown_core_strbuf_truncate(markdown_core_strbuf *buf, bufsize_t len) {
    if (len < 0) {
        len = 0;
    }

    if (len < buf->size) {
        buf->size = len;
        buf->ptr[buf->size] = '\0';
    }
}

void markdown_core_strbuf_drop(markdown_core_strbuf *buf, bufsize_t n) {
    if (n > 0) {
        if (n > buf->size) {
            n = buf->size;
        }
        buf->size = buf->size - n;
        if (buf->size) {
            memmove(buf->ptr, buf->ptr + n, buf->size);
        }

        buf->ptr[buf->size] = '\0';
    }
}

static void markdown_core_strbuf_rtrim(markdown_core_strbuf *buf) {
    if (!buf->size) {
        return;
    }

    while (buf->size > 0) {
        if (!markdown_core_isspace(buf->ptr[buf->size - 1])) {
            break;
        }

        buf->size--;
    }

    buf->ptr[buf->size] = '\0';
}

void markdown_core_strbuf_trim(markdown_core_strbuf *buf) {
    bufsize_t i = 0;

    if (!buf->size) {
        return;
    }

    while (i < buf->size && markdown_core_isspace(buf->ptr[i])) {
        i++;
    }

    markdown_core_strbuf_drop(buf, i);

    markdown_core_strbuf_rtrim(buf);
}

// Destructively modify string, collapsing consecutive
// space and newline characters into a single space.
void markdown_core_strbuf_normalize_whitespace(markdown_core_strbuf *s) {
    bool last_char_was_space = false;
    bufsize_t r, w;

    for (r = 0, w = 0; r < s->size; ++r) {
        if (markdown_core_isspace(s->ptr[r])) {
            if (!last_char_was_space) {
                s->ptr[w++] = ' ';
                last_char_was_space = true;
            }
        } else {
            s->ptr[w++] = s->ptr[r];
            last_char_was_space = false;
        }
    }

    markdown_core_strbuf_truncate(s, w);
}

// Destructively unescape a string: remove backslashes before punctuation chars.
extern void markdown_core_strbuf_unescape(markdown_core_strbuf *buf) {
    bufsize_t r, w;

    for (r = 0, w = 0; r < buf->size; ++r) {
        if (buf->ptr[r] == '\\' && markdown_core_ispunct(buf->ptr[r + 1])) {
            r++;
        }

        buf->ptr[w++] = buf->ptr[r];
    }

    markdown_core_strbuf_truncate(buf, w);
}

/* The frozen buffer (#153); the contract lives at the declaration. Freeze
 * reuses the strbuf's allocation -- the header wraps the detached bytes, so
 * freezing is O(1) and copies nothing. */
markdown_core_buf *markdown_core_buf_freeze(markdown_core_strbuf *buf) {
    markdown_core_mem *mem = buf->mem;
    unsigned char *bytes;
    bufsize_t size = buf->size;
    markdown_core_buf *frozen;

    bytes = markdown_core_strbuf_detach(buf);
    if (!bytes) {
        return NULL;
    }
    frozen = (markdown_core_buf *)mem->calloc(1, sizeof(*frozen));
    if (!frozen) {
        mem->free(bytes);
        return NULL;
    }
    frozen->mem = mem;
    frozen->bytes = bytes;
    frozen->size = size;
    markdown_core_atomic_init(&frozen->refs, 1);
    return frozen;
}

markdown_core_buf *markdown_core_buf_adopt(markdown_core_mem *mem, unsigned char *bytes, bufsize_t size) {
    markdown_core_buf *frozen = (markdown_core_buf *)mem->calloc(1, sizeof(*frozen));
    if (!frozen) {
        return NULL;
    }
    frozen->mem = mem;
    frozen->bytes = bytes;
    frozen->size = size;
    markdown_core_atomic_init(&frozen->refs, 1);
    return frozen;
}

void markdown_core_buf_retain(markdown_core_buf *buf) {
    if (buf) {
        markdown_core_atomic_increment(&buf->refs);
    }
}

void markdown_core_buf_release(markdown_core_buf *buf) {
    if (buf && markdown_core_atomic_decrement(&buf->refs) == 0) {
        buf->mem->free(buf->bytes);
        buf->mem->free(buf);
    }
}
