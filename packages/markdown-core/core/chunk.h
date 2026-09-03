#ifndef MARKDOWN_CORE_CHUNK_H
#define MARKDOWN_CORE_CHUNK_H

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include "markdown-core.h"
#include "buffer.h"
#include "markdown_core_ctype.h"

#define MARKDOWN_CORE_CHUNK_EMPTY {NULL, 0, 0}

typedef struct markdown_core_chunk {
    unsigned char *data;
    bufsize_t len;
    bufsize_t alloc; // also implies a NULL-terminated string
} markdown_core_chunk;

/* AN OPTIONAL CHUNK, and it is a DIFFERENT TYPE from a chunk on purpose.
 *
 * Requirement 14: `null` means the source did not write this and `""` means
 * the source wrote it and it was empty. Before this type the two shared one
 * representation -- a chunk whose `data` was NULL -- so a write site could
 * store either without saying which it meant, and three separate reads folded
 * one into the other to compensate. A field of this type cannot be assigned a
 * chunk: the write site must name `_present` or `_absent`, and a site that
 * does not say which DOES NOT COMPILE. That is the whole mechanism.
 *
 * `has_value` is not derived from `value.data`. It cannot be, and that is why
 * it is a separate byte rather than a newtype over the old convention: a
 * present-and-empty value whose one-byte buffer could not be allocated has
 * NULL data and is still PRESENT, and under the no-fallback ruling that parse
 * fails rather than quietly reporting absence.
 *
 * It costs eight bytes on `markdown_core_code`, which is the widest arm of
 * `node.as`, and therefore eight bytes on every node in the document. That is
 * measured, not estimated, and section 4.14.14 states the number. */
typedef struct markdown_core_optional_chunk {
    markdown_core_chunk value;
    bool has_value;
} markdown_core_optional_chunk;

static MARKDOWN_CORE_INLINE markdown_core_optional_chunk markdown_core_optional_chunk_absent(void) {
    markdown_core_optional_chunk o;
    o.value.data = NULL;
    o.value.len = 0;
    o.value.alloc = 0;
    o.has_value = false;
    return o;
}

/* The source wrote this. Whether it wrote any BYTES is a different question
 * and this constructor deliberately does not ask it: `c` may be empty. */
static MARKDOWN_CORE_INLINE markdown_core_optional_chunk markdown_core_optional_chunk_present(markdown_core_chunk c) {
    markdown_core_optional_chunk o;
    o.value = c;
    o.has_value = true;
    return o;
}

static MARKDOWN_CORE_INLINE void markdown_core_chunk_free(markdown_core_mem *mem, markdown_core_chunk *c) {
    if (c->alloc) {
        mem->free(c->data);
    }

    c->data = NULL;
    c->alloc = 0;
    c->len = 0;
}

static MARKDOWN_CORE_INLINE void markdown_core_optional_chunk_free(markdown_core_mem *mem,
                                                                   markdown_core_optional_chunk *o) {
    markdown_core_chunk_free(mem, &o->value);
    o->has_value = false;
}

static MARKDOWN_CORE_INLINE void markdown_core_chunk_ltrim(markdown_core_chunk *c) {
    assert(!c->alloc);

    while (c->len && markdown_core_isspace(c->data[0])) {
        c->data++;
        c->len--;
    }
}

static MARKDOWN_CORE_INLINE void markdown_core_chunk_rtrim(markdown_core_chunk *c) {
    assert(!c->alloc);

    while (c->len > 0) {
        if (!markdown_core_isspace(c->data[c->len - 1])) {
            break;
        }

        c->len--;
    }
}

static MARKDOWN_CORE_INLINE void markdown_core_chunk_trim(markdown_core_chunk *c) {
    markdown_core_chunk_ltrim(c);
    markdown_core_chunk_rtrim(c);
}

static MARKDOWN_CORE_INLINE bufsize_t markdown_core_chunk_strchr(markdown_core_chunk *ch, int c, bufsize_t offset) {
    const unsigned char *p = (unsigned char *)memchr(ch->data + offset, c, ch->len - offset);
    return p ? (bufsize_t)(p - ch->data) : ch->len;
}

static MARKDOWN_CORE_INLINE const char *markdown_core_chunk_to_cstr(markdown_core_mem *mem, markdown_core_chunk *c) {
    unsigned char *str;

    if (c->alloc) {
        return (char *)c->data;
    }
    str = (unsigned char *)mem->calloc(c->len + 1, 1);
    /* NULL reports allocation failure; the chunk keeps its borrowed bytes. */
    if (!str) {
        return NULL;
    }
    if (c->len > 0) {
        memcpy(str, c->data, c->len);
    }
    str[c->len] = 0;
    c->data = str;
    c->alloc = 1;

    return (char *)str;
}

/* Returns 0 when the copy could not be allocated; the chunk then keeps its
 * previous value. */
static MARKDOWN_CORE_INLINE int markdown_core_chunk_set_cstr(markdown_core_mem *mem, markdown_core_chunk *c,
                                                             const char *str) {
    unsigned char *old = c->alloc ? c->data : NULL;
    if (str == NULL) {
        c->len = 0;
        c->data = NULL;
        c->alloc = 0;
    } else {
        bufsize_t len = (bufsize_t)strlen(str);
        unsigned char *copy = (unsigned char *)mem->calloc((size_t)len + 1, 1);
        if (!copy) {
            return 0;
        }
        c->len = len;
        c->data = copy;
        c->alloc = 1;
        memcpy(c->data, str, (size_t)len + 1);
    }
    if (old != NULL) {
        mem->free(old);
    }
    return 1;
}

static MARKDOWN_CORE_INLINE markdown_core_chunk markdown_core_chunk_literal(const char *data) {
    bufsize_t len = data ? (bufsize_t)strlen(data) : 0;
    markdown_core_chunk c = {(unsigned char *)data, len, 0};
    return c;
}

static MARKDOWN_CORE_INLINE markdown_core_chunk markdown_core_chunk_dup(const markdown_core_chunk *ch, bufsize_t pos,
                                                                        bufsize_t len) {
    markdown_core_chunk c = {ch->data ? ch->data + pos : NULL, len, 0};
    return c;
}

static MARKDOWN_CORE_INLINE markdown_core_chunk markdown_core_chunk_buf_detach(markdown_core_strbuf *buf) {
    markdown_core_chunk c;

    c.len = buf->size;
    c.data = markdown_core_strbuf_detach(buf);
    c.alloc = 1;
    /* A poisoned or empty-and-unallocatable buffer detaches to NULL; the
     * chunk reports the loss as empty with NULL data. */
    if (!c.data) {
        c.len = 0;
        c.alloc = 0;
    }

    return c;
}

/* trim_new variants are to be used when the source chunk may or may not be
 * allocated; forces a newly allocated chunk. */
static MARKDOWN_CORE_INLINE markdown_core_chunk markdown_core_chunk_ltrim_new(markdown_core_mem *mem,
                                                                              markdown_core_chunk *c) {
    markdown_core_chunk r = markdown_core_chunk_dup(c, 0, c->len);
    markdown_core_chunk_ltrim(&r);
    if (!markdown_core_chunk_to_cstr(mem, &r)) {
        /* Callers rely on an owned copy; report the loss as empty instead of
         * handing back a borrowed pointer. */
        markdown_core_chunk empty = MARKDOWN_CORE_CHUNK_EMPTY;
        return empty;
    }
    return r;
}

static MARKDOWN_CORE_INLINE markdown_core_chunk markdown_core_chunk_rtrim_new(markdown_core_mem *mem,
                                                                              markdown_core_chunk *c) {
    markdown_core_chunk r = markdown_core_chunk_dup(c, 0, c->len);
    markdown_core_chunk_rtrim(&r);
    if (!markdown_core_chunk_to_cstr(mem, &r)) {
        markdown_core_chunk empty = MARKDOWN_CORE_CHUNK_EMPTY;
        return empty;
    }
    return r;
}

#endif
