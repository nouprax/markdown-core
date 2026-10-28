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
 * `source`, columns counted in `unit`; `*session` is NULL unless the message
 * is its document. markdown_core_wire_session_append appends `size` bytes of
 * `text`, and markdown_core_wire_session_finalize ends the session's text. */
uint8_t *markdown_core_wire_session_new(const uint8_t *source, size_t length, markdown_core_text_unit unit,
                                        markdown_core_session **session);
uint8_t *markdown_core_wire_session_append(markdown_core_session *session, const uint8_t *text, size_t size);
uint8_t *markdown_core_wire_session_finalize(markdown_core_session *session);

/* QUERIES of a session (docs/plans/2026-10-23-lsp-completion-engine.md, step
 * 3), for the same bindings. Each answers with one caller-owned message of its
 * answer, or of its failure's status, as above; the answers' formats are in
 * docs/architecture/wire-format.md, Queries. Each asks the session's current
 * document and its text, which the session holds, so no query takes the source.
 *
 * markdown_core_wire_dialect answers the names and the characters the dialect
 * fixes, which no session holds: the metadata field names and the slot cues.
 *
 * markdown_core_wire_locate answers markdown_core_session_locate at the
 * cursor `position` with the session's context as the locate leaves it, the
 * state the binding reads its context from: there is no context to release.
 * OUT_OF_BOUNDS leaves the state as it was; after ALLOCATION_FAILED, which
 * the answer itself may meet, the binding holds no slot of it.
 *
 * markdown_core_wire_spell answers markdown_core_slot_spell of `size` bytes of
 * `value` in slot `slot` of the session's context as it stands.
 *
 * markdown_core_wire_field and markdown_core_wire_markers answer
 * markdown_core_document_field of slot kind `slot` and
 * markdown_core_document_markers for the node with identifier `id` among the
 * nodes that hold the byte at `position`: the binding names a node of its copy
 * of the document by its identifier and a byte it holds. OUT_OF_BOUNDS when
 * no such node holds that byte. */
uint8_t *markdown_core_wire_dialect(void);
uint8_t *markdown_core_wire_locate(markdown_core_session *session, markdown_core_position position);
uint8_t *markdown_core_wire_spell(const markdown_core_session *session, size_t slot, const uint8_t *value, size_t size);
uint8_t *markdown_core_wire_field(const markdown_core_session *session, uint64_t id, markdown_core_position position,
                                  markdown_core_slot_kind slot);
uint8_t *markdown_core_wire_markers(const markdown_core_session *session, uint64_t id, markdown_core_position position);

/* Releases a message returned by any function above. */
void markdown_core_wire_free(uint8_t *message);

#ifdef __cplusplus
}
#endif

#endif
