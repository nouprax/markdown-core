#ifndef MARKDOWN_CORE_WIRE_H
#define MARKDOWN_CORE_WIRE_H

#include <stddef.h>
#include <stdint.h>

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

/* Releases a message returned by markdown_core_wire_parse. */
void markdown_core_wire_free(uint8_t *message);

#ifdef __cplusplus
}
#endif

#endif
