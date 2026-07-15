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

#ifdef __cplusplus
}
#endif

#endif
