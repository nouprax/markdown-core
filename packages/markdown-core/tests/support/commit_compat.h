#ifndef MARKDOWN_CORE_TEST_COMMIT_COMPAT_H
#define MARKDOWN_CORE_TEST_COMMIT_COMPAT_H

#include "../../include/markdown_core.h"
#include <stdlib.h>
#include <string.h>

/* A string view, spelled once. Every test owns its own bytes and hands the
 * whole of them over, which is what the engine's operation takes. */
static inline markdown_core_string mc_sv(const void *data, size_t length) {
    markdown_core_string view;
    view.data = (const uint8_t *)data;
    view.length = length;
    return view;
}

/* The caller's text. Tests that used to splice the document's buffer splice
 * this instead and hand the whole of it over. */
typedef struct mc_text {
    char *bytes;
    size_t length;
    size_t capacity;
} mc_text;

static inline bool mc_text_splice(mc_text *t, size_t start, size_t end, const void *bytes, size_t length) {
    size_t tail;
    if (start > end || end > t->length) {
        return false;
    }
    if (t->length - (end - start) + length + 1 > t->capacity) {
        size_t want = (t->length - (end - start) + length + 1) * 2;
        char *grown = (char *)realloc(t->bytes, want);
        if (!grown) {
            return false;
        }
        t->bytes = grown;
        t->capacity = want;
    }
    tail = t->length - end;
    memmove(t->bytes + start + length, t->bytes + end, tail);
    if (length) {
        memcpy(t->bytes + start, bytes, length);
    }
    t->length = start + length + tail;
    t->bytes[t->length] = 0;
    return true;
}

static inline void mc_text_free(mc_text *t) {
    free(t->bytes);
    t->bytes = NULL;
    t->length = 0;
    t->capacity = 0;
}

/* Hands `markdown` to the document and swaps the caller's handle. */
static inline bool mc_edit(
    markdown_core_document **document,
    markdown_core_string markdown,
    markdown_core_delta **delta,
    markdown_core_error **error
) {
    markdown_core_commit out;
    markdown_core_document *previous = *document;
    memset(&out, 0, sizeof(out));
    // The engine reads the receiver and takes nothing; this helper keeps the
    // replace-in-place shape every test was written against by doing the
    // release itself. A test that wants two lines of descent calls the
    // engine directly.
    if (!markdown_core_document_edit(previous, markdown, &out, error)) {
        markdown_core_document_free(previous);
        *document = NULL;
        return false;
    }
    markdown_core_document_free(previous);
    *document = out.document;
    if (delta) {
        *delta = out.delta;
    } else {
        markdown_core_delta_free(out.delta);
    }
    return true;
}

#endif
