#ifndef MARKDOWN_CORE_REFERENCES_H
#define MARKDOWN_CORE_REFERENCES_H

#include "map.h"
#include "node.h"

#ifdef __cplusplus
extern "C" {
#endif

/* THE DEFINITION SETS. Both maps hold normalized labels; the reference map
 * also holds, per label, THE RESOURCE its winning definition stated (M2).
 *
 * The reference map answers TWO questions -- is this label defined, and what
 * destination and title does it name -- and it is the only thing that can
 * answer them while the inline phase is running. The resource lives here ONCE,
 * and every occurrence that resolves to the label shares it, which is what
 * deletes D9: resolving a reference used to COPY the definition's destination
 * and title into the node, so one definition with a long destination
 * referenced many times turned a small document into a large tree, and the
 * running expansion budget that bounded it made WHETHER A REFERENCE RESOLVES
 * depend on how many resolved before it. A reference that SHARES its
 * definition's resource costs nothing to resolve, so there is nothing to
 * charge and no budget to break resolution.
 *
 * Neither map holds a NODE: a map that owns a node is how a definition nested
 * inside another came to be freed while the tree still pointed at it (D11).
 * Two definitions of one label are two records, and indexing keeps the first
 * in source order, which is the inherited rule; the loser's resource is freed
 * with the map, unshared. The footnote map holds labels and nothing else. */
markdown_core_map *markdown_core_reference_map_new(markdown_core_mem *mem);
/* Takes ownership of `resource` -- one holder -- and keeps it on the record
 * for `label`, or releases it when the label defines nothing or the record
 * could not be made. `mem` frees it on those paths, since `map` may be NULL
 * once parser construction has poisoned the parse. */
void markdown_core_reference_create(markdown_core_mem *mem, markdown_core_map *map, markdown_core_chunk *label,
                                    struct markdown_core_resource *resource);
markdown_core_map *markdown_core_footnote_definition_map_new(markdown_core_mem *mem);
void markdown_core_footnote_definition_create(markdown_core_map *map, markdown_core_chunk *label);

#ifdef __cplusplus
}
#endif

#endif
