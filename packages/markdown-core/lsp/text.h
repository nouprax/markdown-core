#ifndef MARKDOWN_CORE_LSP_TEXT_H
#define MARKDOWN_CORE_LSP_TEXT_H

#include "markdown_core_lsp.h"

#include <stddef.h>
#include <stdint.h>

/* Where a protocol position stands in the text of `session`, a flushed
 * session: `*offset` receives the byte it stands before and `*cursor` the parser's position of that place, a line
 * and a column counted from 1 in `unit`, which a session's edit and locate
 * take. Editing and parsing the text are the session's work (plan, section
 * 6), so this is all the engine knows of a document's bytes: the line the
 * session lends, and how many units each scalar counts. It reads that one
 * line and no other, so its cost is the line's, not the text's. The
 * protocol's rules for a position past a line's end, inside a scalar or
 * past the last line apply (markdown_core_lsp.h). */
void lsp_text_locate(const markdown_core_session *session, markdown_core_lsp_position position,
                     markdown_core_text_unit unit, size_t *offset, markdown_core_position *cursor);

/* The session of the document open at `uri` in `server`, or NULL when none
 * is: what the sync oracle compares with its own model of the edits. */
const markdown_core_session *lsp_server_session(const markdown_core_lsp_server *server, const uint8_t *uri,
                                                size_t uri_size);

#endif
