#ifndef MARKDOWN_CORE_MAP_H
#define MARKDOWN_CORE_MAP_H

#include "chunk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A definition map keeps EVERY definition it is given (duplicates included).
 * Lookups resolve a label to its winner: the entry with the minimum document
 * order for that label. Entries may be added at any time; there is no freeze
 * after the first lookup. */
struct markdown_core_map_entry {
    struct markdown_core_map_entry *next; /* every live entry, newest first */
    /* Same-label chain, ascending document order, CIRCULAR and doubly
     * linked: the index slot names the head (the winner), so `head->bucket_prev`
     * names the tail without a field of its own. A lone entry links to itself.
     *
     * Circular because both ends are hot and neither insertion is a search.
     * A definition joins its label either as the new minimum (a full index
     * build walks the live chain newest-first) or as the new maximum (a
     * later add stamps the largest order there has ever been), and both
     * are "splice in front of the head" — the second one just does not move
     * the head. */
    struct markdown_core_map_entry *bucket_next;
    struct markdown_core_map_entry *bucket_prev;
    unsigned char *label;
    uint64_t order; /* document-order key; the minimum per label wins lookups */
};

typedef struct markdown_core_map_entry markdown_core_map_entry;

typedef struct markdown_core_key_index_slot {
    uint64_t hash;
    const unsigned char *key;
    markdown_core_bufsize key_len;
    void *value;
} markdown_core_key_index_slot;

typedef struct markdown_core_key_index {
    markdown_core_mem *mem;
    markdown_core_key_index_slot *slots;
    size_t capacity;
    size_t size;
} markdown_core_key_index;

struct markdown_core_map;

typedef void (*markdown_core_map_free_func)(struct markdown_core_map *, markdown_core_map_entry *);

struct markdown_core_map {
    markdown_core_mem *mem;
    markdown_core_map_entry *refs;    /* every live entry, newest first */
    markdown_core_map_entry **sorted; /* fallback path: all entries by (label, order) */
    markdown_core_key_index index;    /* hash path: label -> bucket head (winner) */
    size_t size;                      /* live entry count, duplicates included */
    uint64_t next_order;              /* monotonic document-order allocator */
    int prepared;
    int indexed;
    /* Sticky flag: a definition or lookup structure was lost to allocation
     * failure; the owning parser reports the parse as failed. */
    int oom;
    markdown_core_map_free_func free;
};

typedef struct markdown_core_map markdown_core_map;

unsigned char *markdown_core_map_normalize_label(markdown_core_mem *mem, markdown_core_chunk *ref, int *lost);
/* Initializes an empty index at its minimum capacity. Capacity grows only
 * when a new distinct key requires it; occurrence counts are deliberately
 * not accepted as sizing hints because they do not bound unique cardinality. */
int markdown_core_key_index_init(markdown_core_key_index *index, markdown_core_mem *mem);
void markdown_core_key_index_free(markdown_core_key_index *index);
int markdown_core_key_index_insert(
    markdown_core_key_index *index,
    const unsigned char *key,
    markdown_core_bufsize key_len,
    void *value,
    int replace,
    void **existing
);
void *markdown_core_key_index_lookup(
    const markdown_core_key_index *index,
    const unsigned char *key,
    markdown_core_bufsize key_len
);
/* Removes a key via backward-shift deletion. Returns 1 when the key was
 * present. Never violates the probe-window invariant: shifting only moves
 * entries closer to their home slot. */
int markdown_core_key_index_remove(
    markdown_core_key_index *index,
    const unsigned char *key,
    markdown_core_bufsize key_len
);
markdown_core_map *markdown_core_map_new(markdown_core_mem *mem, markdown_core_map_free_func free);
void markdown_core_map_free(markdown_core_map *map);
markdown_core_map_entry *markdown_core_map_lookup(markdown_core_map *map, markdown_core_chunk *label);
/* Links a freshly created entry (label filled by the caller) into the
 * map: stamps the next document order, pushes it onto the live chain, and
 * keeps any prepared lookup structure coherent. */
void markdown_core_map_add(markdown_core_map *map, markdown_core_map_entry *entry);

#ifdef __cplusplus
}
#endif

#endif
