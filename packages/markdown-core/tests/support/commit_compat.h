#ifndef MARKDOWN_CORE_TEST_COMMIT_COMPAT_H
#define MARKDOWN_CORE_TEST_COMMIT_COMPAT_H

#include "../../include/markdown_core.h"
#include <string.h>
#include "commit_compat.h"

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
