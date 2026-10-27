#ifndef MARKDOWN_CORE_LSP_TEXT_H
#define MARKDOWN_CORE_LSP_TEXT_H

#include "markdown_core_lsp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A TEXT the engine edits: `size` bytes at `bytes`, in a buffer of
 * `capacity`. Text editing is the engine's work and parsing the parser's
 * (plan, section 6), so this is all the engine knows of a document's bytes:
 * where its lines end, and how many units each scalar counts. */
typedef struct lsp_text {
    uint8_t *bytes;
    size_t size;
    size_t capacity;
} lsp_text;

/* Where a protocol position stands in `text`: `*offset` receives the byte
 * it stands before and `*cursor` the parser's position of that place, a
 * line and a column counted from 1 in `unit`, which a context query takes.
 * The protocol's rules for a position past a line's end, inside a scalar or
 * past the last line apply (markdown_core_lsp.h). */
void lsp_text_locate(const lsp_text *text, markdown_core_lsp_position position, markdown_core_text_unit unit,
                     size_t *offset, markdown_core_position *cursor);

/* Sets `text` to `size` bytes of `bytes`; false when it cannot grow. */
bool lsp_text_assign(lsp_text *text, const uint8_t *bytes, size_t size);

/* Replaces the bytes from `start` up to `end` of `text` with `size` bytes of
 * `bytes`; false when it cannot grow. */
bool lsp_text_splice(lsp_text *text, size_t start, size_t end, const uint8_t *bytes, size_t size);

void lsp_text_release(lsp_text *text);

/* The text of the document open at `uri` in `server`, or NULL when none is:
 * what the sync oracle compares with its own model of the edits. */
const lsp_text *lsp_server_text(const markdown_core_lsp_server *server, const uint8_t *uri, size_t uri_size);

#endif
