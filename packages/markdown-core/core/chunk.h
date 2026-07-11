#ifndef MARKDOWN_CORE_CHUNK_H
#define MARKDOWN_CORE_CHUNK_H

#include <string.h>
#include <stdlib.h>
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

static MARKDOWN_CORE_INLINE void markdown_core_chunk_free(markdown_core_mem *mem, markdown_core_chunk *c) {
    if (c->alloc)
        mem->free(c->data);

    c->data = NULL;
    c->alloc = 0;
    c->len = 0;
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
        if (!markdown_core_isspace(c->data[c->len - 1]))
            break;

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
    if (c->len > 0) {
        memcpy(str, c->data, c->len);
    }
    str[c->len] = 0;
    c->data = str;
    c->alloc = 1;

    return (char *)str;
}

static MARKDOWN_CORE_INLINE void markdown_core_chunk_set_cstr(markdown_core_mem *mem, markdown_core_chunk *c,
                                                              const char *str) {
    unsigned char *old = c->alloc ? c->data : NULL;
    if (str == NULL) {
        c->len = 0;
        c->data = NULL;
        c->alloc = 0;
    } else {
        c->len = (bufsize_t)strlen(str);
        c->data = (unsigned char *)mem->calloc(c->len + 1, 1);
        c->alloc = 1;
        memcpy(c->data, str, c->len + 1);
    }
    if (old != NULL) {
        mem->free(old);
    }
}

static MARKDOWN_CORE_INLINE markdown_core_chunk markdown_core_chunk_literal(const char *data) {
    bufsize_t len = data ? (bufsize_t)strlen(data) : 0;
    markdown_core_chunk c = {(unsigned char *)data, len, 0};
    return c;
}

static MARKDOWN_CORE_INLINE markdown_core_chunk markdown_core_chunk_dup(const markdown_core_chunk *ch, bufsize_t pos,
                                                                        bufsize_t len) {
    markdown_core_chunk c = {ch->data + pos, len, 0};
    return c;
}

static MARKDOWN_CORE_INLINE markdown_core_chunk markdown_core_chunk_buf_detach(markdown_core_strbuf *buf) {
    markdown_core_chunk c;

    c.len = buf->size;
    c.data = markdown_core_strbuf_detach(buf);
    c.alloc = 1;

    return c;
}

/* trim_new variants are to be used when the source chunk may or may not be
 * allocated; forces a newly allocated chunk. */
static MARKDOWN_CORE_INLINE markdown_core_chunk markdown_core_chunk_ltrim_new(markdown_core_mem *mem,
                                                                              markdown_core_chunk *c) {
    markdown_core_chunk r = markdown_core_chunk_dup(c, 0, c->len);
    markdown_core_chunk_ltrim(&r);
    markdown_core_chunk_to_cstr(mem, &r);
    return r;
}

static MARKDOWN_CORE_INLINE markdown_core_chunk markdown_core_chunk_rtrim_new(markdown_core_mem *mem,
                                                                              markdown_core_chunk *c) {
    markdown_core_chunk r = markdown_core_chunk_dup(c, 0, c->len);
    markdown_core_chunk_rtrim(&r);
    markdown_core_chunk_to_cstr(mem, &r);
    return r;
}

#endif
