#ifndef MARKDOWN_CORE_FOOTNOTES_H
#define MARKDOWN_CORE_FOOTNOTES_H

#include "map.h"

#ifdef __cplusplus
extern "C" {
#endif

struct markdown_core_footnote {
    markdown_core_map_entry entry;
    markdown_core_node *node;
    unsigned int ix;
};

typedef struct markdown_core_footnote markdown_core_footnote;

void markdown_core_footnote_create(markdown_core_map *map, markdown_core_node *node);
markdown_core_map *markdown_core_footnote_map_new(markdown_core_mem *mem);

void markdown_core_unlink_footnotes_map(markdown_core_map *map);

struct markdown_core_parser;

/** Footnote projection over the finished block/inline tree, keeping the v1
 * observable semantics: used definitions move to the document tail in
 * first-use order, references renumber to their definition's index, unused
 * definitions are dropped, unresolved references degrade to literal text.
 * Currently a full recompute per parse; the incremental engine re-applies
 * the same projection per commit. */
void markdown_core_process_footnotes(struct markdown_core_parser *parser);

#ifdef __cplusplus
}
#endif

#endif
