#ifndef MARKDOWN_CORE_PIECES_H
#define MARKDOWN_CORE_PIECES_H

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* BYTES IN PIECES: a run of bytes read from where they are kept, a few
 * pieces in order -- a content in two parts (markdown_core_split), a
 * heading's base and its suffix, a base an edit is about to change and the
 * bytes it changes. Nothing is copied to read them as one. Two runs that read
 * the same storage at the same place hold the same bytes there, so a
 * comparison skips them: comparing a name with what it was before an edit
 * reads only what the edit changed. */
enum { MARKDOWN_CORE_PIECES_MOST = 8 };

typedef struct {
    const unsigned char *data[MARKDOWN_CORE_PIECES_MOST];
    bufsize_t sizes[MARKDOWN_CORE_PIECES_MOST];
    uint32_t count;
    bufsize_t size;
} markdown_core_pieces;

/* No bytes: the pieces past `count` are not read, so they are left as
 * they are. */
static MARKDOWN_CORE_INLINE void markdown_core_pieces_clear(markdown_core_pieces *pieces) {
    pieces->count = 0;
    pieces->size = 0;
}

static MARKDOWN_CORE_INLINE void markdown_core_pieces_add(markdown_core_pieces *pieces, const unsigned char *data,
                                                          bufsize_t size) {
    if (size <= 0) {
        return;
    }
    assert(pieces->count < MARKDOWN_CORE_PIECES_MOST);
    pieces->data[pieces->count] = data;
    pieces->sizes[pieces->count++] = size;
    pieces->size += size;
}

/* The bytes of `buf`, which may be split, from `start` to `end`. */
static MARKDOWN_CORE_INLINE void markdown_core_pieces_span(markdown_core_pieces *pieces,
                                                           const markdown_core_strbuf *buf, bufsize_t start,
                                                           bufsize_t end) {
    const unsigned char *front, *back;
    bufsize_t front_size, back_size;
    markdown_core_strbuf_span(buf, start, end, &front, &front_size, &back, &back_size);
    markdown_core_pieces_add(pieces, front, front_size);
    markdown_core_pieces_add(pieces, back, back_size);
}

/* The first `size` bytes of `pieces`, at most all of them, in `*head`. */
static MARKDOWN_CORE_INLINE void markdown_core_pieces_head(const markdown_core_pieces *pieces, bufsize_t size,
                                                           markdown_core_pieces *head) {
    markdown_core_pieces_clear(head);
    for (uint32_t i = 0; i < pieces->count && head->size < size; i++) {
        const bufsize_t left = size - head->size;
        markdown_core_pieces_add(head, pieces->data[i], pieces->sizes[i] < left ? pieces->sizes[i] : left);
    }
}

/* Whether `a` and `b` hold the same bytes: of the same size, read piece by
 * piece, a stretch both read at one address taken as equal unread. */
bool markdown_core_pieces_same(const markdown_core_pieces *a, const markdown_core_pieces *b);

/* Whether `pieces` holds the `size` bytes at `data`. */
static MARKDOWN_CORE_INLINE bool markdown_core_pieces_are(const markdown_core_pieces *pieces, const unsigned char *data,
                                                          bufsize_t size) {
    markdown_core_pieces whole;
    markdown_core_pieces_clear(&whole);
    markdown_core_pieces_add(&whole, data, size);
    return pieces->size == size && markdown_core_pieces_same(pieces, &whole);
}

/* The bytes, copied to `to`, which holds `pieces->size` of them. */
static MARKDOWN_CORE_INLINE void markdown_core_pieces_copy(const markdown_core_pieces *pieces, unsigned char *to) {
    for (uint32_t i = 0; i < pieces->count; i++) {
        memcpy(to, pieces->data[i], (size_t)pieces->sizes[i]);
        to += pieces->sizes[i];
    }
}

/* The last `count` bytes, at most all of them, copied to `to`; how many. */
static MARKDOWN_CORE_INLINE bufsize_t markdown_core_pieces_tail(const markdown_core_pieces *pieces, bufsize_t count,
                                                                unsigned char *to) {
    if (count > pieces->size) {
        count = pieces->size;
    }
    bufsize_t left = count;
    for (uint32_t i = pieces->count; i-- > 0 && left;) {
        const bufsize_t take = pieces->sizes[i] < left ? pieces->sizes[i] : left;
        left -= take;
        memcpy(to + left, pieces->data[i] + pieces->sizes[i] - take, (size_t)take);
    }
    return count;
}

#ifdef __cplusplus
}
#endif

#endif
