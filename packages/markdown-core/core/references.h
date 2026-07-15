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

void markdown_core_reference_create(markdown_core_map *map, markdown_core_chunk *label, markdown_core_chunk *url,
                                    markdown_core_chunk *title);
markdown_core_map *markdown_core_reference_map_new(markdown_core_mem *mem);

#ifdef __cplusplus
}
#endif

#endif
