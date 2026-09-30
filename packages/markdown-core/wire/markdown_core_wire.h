#ifndef MARKDOWN_CORE_WIRE_H
#define MARKDOWN_CORE_WIRE_H

#include <stddef.h>
#include <stdint.h>

#include "markdown_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The one encoder of MCB3, the byte message every binding that cannot hold C
 * node handles builds its value tree from. The format is specified in
 * docs/architecture/wire-format.md; this encoder reads the tree only through
 * the installed facade, include/markdown_core.h.
 *
 * Parses `source` and returns one caller-owned message: a document, or a parse
 * failure encoded as an error message carrying its markdown_core_status. A
 * result too large to encode is an ALLOCATION_FAILED error message. NULL means not even an error message could be
 * allocated. The message borrows nothing; release it with
 * markdown_core_wire_free. */
uint8_t *markdown_core_wire_parse(const uint8_t *source, size_t length);

/* SESSIONS, for the same bindings. Each step answers with one caller-owned
 * message of the document it published, or of its failure's status, as
 * markdown_core_wire_parse does; the session itself is the C session, which
 * the binding holds and releases with markdown_core_session_free.
 *
 * markdown_core_wire_session_new makes `*session` from `length` bytes of
 * `source`, offsets and columns counted in `unit`; `*session` is NULL unless
 * the message is its document. markdown_core_wire_session_edit applies `count`
 * edits, each three sizes in `edits` -- start, end and the size of its text,
 * as markdown_core_text_edit has them -- whose texts follow one another in
 * `texts` in the order listed. markdown_core_wire_session_append appends
 * `size` bytes of `text`. */
uint8_t *markdown_core_wire_session_new(const uint8_t *source, size_t length, markdown_core_text_unit unit,
                                        markdown_core_session **session);
uint8_t *markdown_core_wire_session_edit(markdown_core_session *session, const size_t *edits, size_t count,
                                         const uint8_t *texts);
uint8_t *markdown_core_wire_session_append(markdown_core_session *session, const uint8_t *text, size_t size);

/* Releases a message returned by any function above. */
void markdown_core_wire_free(uint8_t *message);

#ifdef __cplusplus
}
#endif

#endif
