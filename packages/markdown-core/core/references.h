#ifndef MARKDOWN_CORE_REFERENCES_H
#define MARKDOWN_CORE_REFERENCES_H

#include "map.h"
#include "node.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What a link reference definition was WRITTEN as: borrowed views into the
 * block content it was read from, valid until that content is dropped.
 *
 * The map stores a normalized, owned key because that is what a lookup needs.
 * The node stores what the author typed, because that is what a document is.
 * Both are made from these three chunks, which is why the parse hands them out
 * rather than each caller re-scanning the line. */
typedef struct {
    markdown_core_chunk label;
    markdown_core_chunk url;
    markdown_core_chunk title;
} markdown_core_reference_parts;

/* Build the association a reference or a definition carries, from the label AS
 * WRITTEN. `prefix` is prepended to the IDENTIFIER and not to the label: it is
 * `^` for the two footnote kinds and 0 for the other three, which is how a
 * footnote and a link definition of the same name stay apart in a consumer's
 * single map (see markdown_core_association in node.h).
 *
 * Returns 0 having allocated nothing on failure -- an association with half a
 * value is a node that lies, and there is no honest partial state. */
int markdown_core_association_init(
    markdown_core_mem *mem,
    markdown_core_association *out,
    const markdown_core_chunk *label,
    unsigned char prefix
);
void markdown_core_association_free(markdown_core_mem *mem, markdown_core_association *association);

/* THE DEFINITION SETS. Both maps hold normalized labels and, beside each, the
 * registering definition's block identity (docs/STREAMING.md D4).
 *
 * A map answers TWO questions -- is this label defined, and WHICH definition
 * wins it -- and it is the only thing that can answer them while the inline
 * phase is running. It holds no resource, which is what deletes D9: resolving
 * a reference used to COPY the definition's destination and title into the
 * node, so one definition with a long destination referenced many times turned
 * a small document into a large tree, and the running expansion budget that
 * bounded it made WHETHER A REFERENCE RESOLVES depend on how many resolved
 * before it. A reference that NAMES its definition costs nothing to resolve,
 * so there is nothing to charge and no budget to break resolution.
 *
 * It holds no NODE either: a map that owns a node is how a definition nested
 * inside another came to be freed while the tree still pointed at it (D11).
 * The identity is a VALUE, so D11's shape cannot return through it. The winner
 * between two definitions of one label is first-in-document-order, decided by
 * the entry's `age` at preparation and never by registration timing within a
 * block -- ENTER versus EXIT still decides nothing, because a definition's age
 * is stamped from a count no preparation rewrites (§12.4).
 *
 * `definition` on both `_create` calls is the registering definition block's
 * identity; the caller reads it AFTER any identity handoff the harvest
 * performs (§4 D4 fork 3), so the map carries the id the tree keeps. */
markdown_core_map *markdown_core_reference_map_new(markdown_core_mem *mem);
void markdown_core_reference_create(markdown_core_map *map, markdown_core_chunk *label, uint32_t definition);
markdown_core_map *markdown_core_footnote_definition_map_new(markdown_core_mem *mem);
void markdown_core_footnote_definition_create(markdown_core_map *map, markdown_core_chunk *label, uint32_t definition);

#ifdef __cplusplus
}
#endif

#endif
