#ifndef MARKDOWN_CORE_MAP_H
#define MARKDOWN_CORE_MAP_H

#include "chunk.h"

#ifdef __cplusplus
extern "C" {
#endif

struct markdown_core_map_entry {
    struct markdown_core_map_entry *next;
    unsigned char *label;
    size_t age;
    size_t size;
};

typedef struct markdown_core_map_entry markdown_core_map_entry;

struct markdown_core_map;

typedef void (*markdown_core_map_free_f)(struct markdown_core_map *, markdown_core_map_entry *);

struct markdown_core_map {
    markdown_core_mem *mem;
    markdown_core_map_entry *refs;
    markdown_core_map_entry **sorted;
    size_t size;
    size_t ref_size;
    size_t max_ref_size;
    markdown_core_map_free_f free;
};

typedef struct markdown_core_map markdown_core_map;

unsigned char *normalize_map_label(markdown_core_mem *mem, markdown_core_chunk *ref);
markdown_core_map *markdown_core_map_new(markdown_core_mem *mem, markdown_core_map_free_f free);
void markdown_core_map_free(markdown_core_map *map);
markdown_core_map_entry *markdown_core_map_lookup(markdown_core_map *map, markdown_core_chunk *label);

#ifdef __cplusplus
}
#endif

#endif
