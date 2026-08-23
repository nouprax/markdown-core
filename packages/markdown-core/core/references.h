#ifndef MARKDOWN_CORE_REFERENCES_H
#define MARKDOWN_CORE_REFERENCES_H

#include "map.h"

#ifdef __cplusplus
extern "C" {
#endif

struct markdown_core_reference {
    markdown_core_map_entry entry;
    markdown_core_chunk url;
    markdown_core_chunk title;
};

typedef struct markdown_core_reference markdown_core_reference;

/* What a link reference definition was WRITTEN as: borrowed views into the
 * block content it was read from, valid until that content is dropped.
 *
 * The map stores a cleaned, folded, owned copy because that is what a lookup
 * needs. The node stores what the author typed, because that is what a
 * document is. Both are made from these three chunks, which is why the parse
 * hands them out rather than each caller re-scanning the line. */
typedef struct {
    markdown_core_chunk label;
    markdown_core_chunk url;
    markdown_core_chunk title;
} markdown_core_reference_parts;

void markdown_core_reference_create(markdown_core_map *map, markdown_core_chunk *label, markdown_core_chunk *url,
                                    markdown_core_chunk *title);
markdown_core_map *markdown_core_reference_map_new(markdown_core_mem *mem);

/* The set of footnote labels the document defines.
 *
 * It answers ONE question -- is this label defined -- and it is the only thing
 * that can answer it while the inline phase is running, which is where a
 * `[^label]` has to decide whether it is a call at all (§5.1). It holds labels
 * and NEVER a node: a map that owns a node is how a definition nested inside
 * another came to be freed while the tree still pointed at it (D11). Because it
 * holds no node and picks no winner between two definitions of one label,
 * registration ORDER decides nothing -- which is measured, not assumed, and is
 * why D11's ENTER-versus-EXIT question does not arise in this shape at all. */
markdown_core_map *markdown_core_footnote_definition_map_new(markdown_core_mem *mem);
void markdown_core_footnote_definition_create(markdown_core_map *map, markdown_core_chunk *label);

#ifdef __cplusplus
}
#endif

#endif
