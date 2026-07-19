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
/* Immutable by contract: empty buffers borrow this sentinel and every write
 * path grows into owned storage first, so it lives in read-only memory and
 * any accidental store faults loudly. The mutable-pointer spelling in the
 * header keeps strbuf.ptr plumbing unchanged. */
const unsigned char markdown_core_strbuf__initbuf[1] = {0};

#ifndef MIN
#define MIN(x, y) ((x < y) ? x : y)
#endif

void markdown_core_strbuf_init(markdown_core_mem *mem, markdown_core_strbuf *buf, bufsize_t initial_size) {
    buf->mem = mem;
    buf->asize = 0;
    buf->size = 0;
    buf->oom = 0;
    buf->ptr = (unsigned char *)markdown_core_strbuf__initbuf;

    if (initial_size > 0) {
        markdown_core_strbuf_grow(buf, initial_size);
    }
}

static MARKDOWN_CORE_INLINE void S_strbuf_grow_by(markdown_core_strbuf *buf, bufsize_t add) {
    markdown_core_strbuf_grow(buf, buf->size + add);
}

void markdown_core_strbuf_grow(markdown_core_strbuf *buf, bufsize_t target_size) {
    assert(target_size > 0);

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

    unsigned char *new_ptr = (unsigned char *)buf->mem->realloc(buf->mem, buf->asize ? buf->ptr : NULL, new_size);
    if (!new_ptr) {
        buf->oom = 1;
        return;
    }
    buf->ptr = new_ptr;
    buf->asize = new_size;
}

bufsize_t markdown_core_strbuf_len(const markdown_core_strbuf *buf) { return buf->size; }

void markdown_core_strbuf_free(markdown_core_strbuf *buf) {
    if (!buf) {
        return;
    }

    if (buf->ptr != markdown_core_strbuf__initbuf) {
        buf->mem->free(buf->mem, buf->ptr);
    }

    markdown_core_strbuf_init(buf->mem, buf, 0);
}

void markdown_core_strbuf_clear(markdown_core_strbuf *buf) {
    buf->size = 0;

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

void markdown_core_strbuf_copy_cstr(char *data, bufsize_t datasize, const markdown_core_strbuf *buf) {
    bufsize_t copylen;

    assert(buf);
    if (!data || datasize <= 0) {
        return;
    }

    data[0] = '\0';

    if (buf->size == 0 || buf->asize <= 0) {
        return;
    }

    copylen = buf->size;
    if (copylen > datasize - 1) {
        copylen = datasize - 1;
    }
    memmove(data, buf->ptr, copylen);
    data[copylen] = '\0';
}

void markdown_core_strbuf_swap(markdown_core_strbuf *buf_a, markdown_core_strbuf *buf_b) {
    markdown_core_strbuf t = *buf_a;
    *buf_a = *buf_b;
    *buf_b = t;
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
        return (unsigned char *)buf->mem->calloc(buf->mem, 1, 1);
    }

    markdown_core_strbuf_init(buf->mem, buf, 0);
    return data;
}

int markdown_core_strbuf_cmp(const markdown_core_strbuf *a, const markdown_core_strbuf *b) {
    int result = memcmp(a->ptr, b->ptr, MIN(a->size, b->size));
    return (result != 0) ? result : (a->size < b->size) ? -1 : (a->size > b->size) ? 1 : 0;
}

bufsize_t markdown_core_strbuf_strchr(const markdown_core_strbuf *buf, int c, bufsize_t pos) {
    if (pos >= buf->size) {
        return -1;
    }
    if (pos < 0) {
        pos = 0;
    }

    const unsigned char *p = (unsigned char *)memchr(buf->ptr + pos, c, buf->size - pos);
    if (!p) {
        return -1;
    }

    return (bufsize_t)(p - (const unsigned char *)buf->ptr);
}

bufsize_t markdown_core_strbuf_strrchr(const markdown_core_strbuf *buf, int c, bufsize_t pos) {
    if (pos < 0 || buf->size == 0) {
        return -1;
    }
    if (pos >= buf->size) {
        pos = buf->size - 1;
    }

    bufsize_t i;
    for (i = pos; i >= 0; i--) {
        if (buf->ptr[i] == (unsigned char)c) {
            return i;
        }
    }

    return -1;
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

void markdown_core_strbuf_rtrim(markdown_core_strbuf *buf) {
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
