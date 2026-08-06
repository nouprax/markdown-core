#ifndef MARKDOWN_CORE_TEXT_H
#define MARKDOWN_CORE_TEXT_H

#include <stddef.h>
#include <stdbool.h>

#include "config.h"
#include "markdown-core.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A session's source text: the raw bytes exactly as edited.
 *
 * The store never normalizes its contents. NUL bytes and invalid UTF-8 stay
 * in place and are replaced with U+FFFD during parsing, per line, exactly as
 * the one-shot parse path does. This is what lets a streamed append complete
 * a multi-byte character whose first bytes arrived in an earlier edit.
 *
 * The splice implementation is a plain contiguous buffer for now; the
 * interface is what incremental damage planning will build on, so callers
 * must not assume contiguity beyond `markdown_core_text_bytes`.
 */
typedef struct {
    markdown_core_mem *mem;
    unsigned char *data;
    size_t length;
    size_t alloc;
} markdown_core_text;

void markdown_core_text_init(markdown_core_text *text, markdown_core_mem *mem);

void markdown_core_text_release(markdown_core_text *text);

/** Replaces bytes [start, end) with `bytes[0..length)`.
 * Returns false when the range is invalid or allocation fails; the stored
 * text is unchanged on failure.
 *
 * `bytes_moved` receives the count of already-stored bytes this call had to
 * copy to make room — the suffix shift plus any growth copy, and zero on
 * failure. It is required rather than nullable because it is a gate input,
 * not a diagnostic: the session accumulates it so a test can hold the store
 * to a bound instead of holding prose to it. A contiguous buffer must shift
 * every byte after the edit, so this is proportional to the untouched suffix
 * and therefore to the document; the substrate that replaces it reports the
 * same quantity from its own path copying.
 */
bool markdown_core_text_edit(
    markdown_core_text *text,
    size_t start,
    size_t end,
    const unsigned char *bytes,
    size_t length,
    size_t *bytes_moved
);

static MARKDOWN_CORE_INLINE const unsigned char *markdown_core_text_bytes(const markdown_core_text *text) {
    return text->data;
}

static MARKDOWN_CORE_INLINE size_t markdown_core_text_length(const markdown_core_text *text) { return text->length; }

#ifdef __cplusplus
}
#endif

#endif
