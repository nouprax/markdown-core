#ifndef MARKDOWN_CORE_TEST_COMMIT_COMPAT_H
#define MARKDOWN_CORE_TEST_COMMIT_COMPAT_H

#include "../../include/markdown_core.h"
#include <string.h>

/* TRANSITIONAL. The engine's operation is `commit(markdown)`: it builds a new
 * document from whole text, diffs it against the one it was called on, and
 * supersedes that one. A great many tests are still written as "edit the
 * document, then commit it", so this commits the document's OWN stored text
 * and swaps the caller's handle. It goes when those tests own their string. */
static inline bool mc_commit_compat(
    markdown_core_document **document,
    markdown_core_delta **delta,
    markdown_core_error **error
) {
    markdown_core_commit commit;
    const uint8_t *text;
    size_t length = 0;

    memset(&commit, 0, sizeof(commit));
    if (!document || !*document) {
        return false;
    }
    text = markdown_core_document_text(*document, &length);
    if (!markdown_core_document_commit(document, text, length, &commit, error)) {
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
