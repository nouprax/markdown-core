#ifndef MARKDOWN_CORE_TEST_COMMIT_COMPAT_H
#define MARKDOWN_CORE_TEST_COMMIT_COMPAT_H

#include "../../include/markdown_core.h"
#include <stdlib.h>
#include <string.h>

/* TRANSITIONAL. The engine's operation is `commit(markdown)`: it builds a new
 * document from whole text, diffs it against the one it was called on, and
 * supersedes that one. A great many tests are still written as "edit the
 * document, then commit it", so this commits the document's OWN stored text
 * and swaps the caller's handle. It goes when those tests own their string. */
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
    memset(&out, 0, sizeof(out));
    if (!markdown_core_document_edit(document, markdown, &out, error)) {
        *document = NULL;
        return false;
    }
    *document = out.document;
    if (delta) {
        *delta = out.delta;
    } else {
        markdown_core_delta_free(out.delta);
    }
    return true;
}

static inline bool mc_commit_compat(
    markdown_core_document **document,
    markdown_core_delta **delta,
    markdown_core_error **error
) {
    markdown_core_commit commit;
    markdown_core_string text;

    memset(&commit, 0, sizeof(commit));
    if (!document || !*document) {
        return false;
    }
    text.data = markdown_core_document_text(*document, &text.length);
    if (!markdown_core_document_edit(document, text, &commit, error)) {
        return false;
    }
    *document = commit.document;
    if (delta) {
        *delta = commit.delta;
    } else {
        markdown_core_delta_free(commit.delta);
    }
    return true;
}

#endif
