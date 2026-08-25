#ifndef MARKDOWN_CORE_MAP_H
#define MARKDOWN_CORE_MAP_H

#include "chunk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An entry is a normalized LABEL and nothing else. It used to carry a `size`,
 * which was the number of bytes resolving against it copied into a node -- the
 * quantity D9's expansion budget charged. A reference that names its definition
 * copies nothing, so there is nothing to charge and no field to carry it. */
struct markdown_core_map_entry {
    struct markdown_core_map_entry *next;
    unsigned char *label;
    size_t age;
};

typedef struct markdown_core_map_entry markdown_core_map_entry;

typedef struct markdown_core_key_index_slot {
    uint64_t hash;
    const unsigned char *key;
    bufsize_t key_len;
    void *value;
} markdown_core_key_index_slot;

typedef struct markdown_core_key_index {
    markdown_core_mem *mem;
    markdown_core_key_index_slot *slots;
    size_t capacity;
    size_t size;
} markdown_core_key_index;

struct markdown_core_map;

typedef void (*markdown_core_map_free_f)(struct markdown_core_map *, markdown_core_map_entry *);

struct markdown_core_map {
    markdown_core_mem *mem;
    markdown_core_map_entry *refs;
    markdown_core_map_entry **sorted;
    /* Entries in `sorted` after the duplicate fold. It is NOT `size`: `size`
     * counts every insert because `entry->age` is stamped from it, and a
     * preparation that overwrote it with the deduped count handed a later
     * definition an age an existing entry already holds -- the first-wins
     * tiebreak then decides between the two by nothing (§12.4). */
    size_t sorted_size;
    markdown_core_key_index index;
    size_t size;
    int prepared;
    int indexed;
    /* Sticky flag: a definition or lookup structure was lost to allocation
     * failure; the owning parser reports the parse as failed. */
    int oom;
    markdown_core_map_free_f free;
};

typedef struct markdown_core_map markdown_core_map;

unsigned char *normalize_map_label(markdown_core_mem *mem, markdown_core_chunk *ref, int *lost);
int markdown_core_key_index_init(markdown_core_key_index *index, markdown_core_mem *mem, size_t expected_size);
void markdown_core_key_index_free(markdown_core_key_index *index);
int markdown_core_key_index_insert(
    markdown_core_key_index *index,
    const unsigned char *key,
    bufsize_t key_len,
    void *value,
    int replace,
    void **existing
);
void *markdown_core_key_index_lookup(const markdown_core_key_index *index, const unsigned char *key, bufsize_t key_len);
markdown_core_map *markdown_core_map_new(markdown_core_mem *mem, markdown_core_map_free_f free);
void markdown_core_map_free(markdown_core_map *map);
markdown_core_map_entry *markdown_core_map_lookup(markdown_core_map *map, markdown_core_chunk *label);

#ifdef __cplusplus
}
#endif

#endif
