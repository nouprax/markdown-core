#ifndef MARKDOWN_CORE_MAP_H
#define MARKDOWN_CORE_MAP_H

#include "chunk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A definition map keeps EVERY definition it is given (duplicates included).
 * Lookups resolve a label to its winner: the entry with the minimum document
 * order for that label. Entries may be added and removed at any time; there
 * is no freeze after the first lookup. */
struct markdown_core_map_entry {
    struct markdown_core_map_entry *next;        /* every live entry, newest first */
    struct markdown_core_map_entry *bucket_next; /* same-label chain, ascending document order */
    unsigned char *label;
    uint64_t order; /* document-order key; the minimum per label wins lookups */
    uint64_t owner; /* owning document-child id (0 = whole document) */
    size_t size;    /* reference expansion accounting */
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

typedef void (*markdown_core_map_free_func)(struct markdown_core_map *, markdown_core_map_entry *);

struct markdown_core_map {
    markdown_core_mem *mem;
    markdown_core_map_entry *refs;    /* every live entry, newest first */
    markdown_core_map_entry **sorted; /* fallback path: all entries by (label, order) */
    markdown_core_key_index index;    /* hash path: label -> bucket head (winner) */
    size_t size;                      /* live entry count, duplicates included */
    uint64_t next_order;              /* monotonic document-order allocator */
    uint64_t pending_owner;           /* stamped onto entries at add time */
    size_t ref_size;
    size_t max_ref_size;
    int prepared;
    int indexed;
    /* Sticky flag: a definition or lookup structure was lost to allocation
     * failure; the owning parser reports the parse as failed. */
    int oom;
    markdown_core_map_free_func free;
};

typedef struct markdown_core_map markdown_core_map;

unsigned char *markdown_core_map_normalize_label(markdown_core_mem *mem, markdown_core_chunk *ref, int *lost);
int markdown_core_key_index_init(markdown_core_key_index *index, markdown_core_mem *mem, size_t expected_size);
void markdown_core_key_index_free(markdown_core_key_index *index);
int markdown_core_key_index_insert(markdown_core_key_index *index, const unsigned char *key, bufsize_t key_len,
                                   void *value, int replace, void **existing);
void *markdown_core_key_index_lookup(const markdown_core_key_index *index, const unsigned char *key, bufsize_t key_len);
/* Removes a key via backward-shift deletion. Returns 1 when the key was
 * present. Never violates the probe-window invariant: shifting only moves
 * entries closer to their home slot. */
int markdown_core_key_index_remove(markdown_core_key_index *index, const unsigned char *key, bufsize_t key_len);
markdown_core_map *markdown_core_map_new(markdown_core_mem *mem, markdown_core_map_free_func free);
void markdown_core_map_free(markdown_core_map *map);
markdown_core_map_entry *markdown_core_map_lookup(markdown_core_map *map, markdown_core_chunk *label);
/* Links a freshly created entry (label/size filled by the caller) into the
 * map: stamps the next document order and the pending owner, pushes it onto
 * the live chain, and keeps any prepared lookup structure coherent. */
void markdown_core_map_add(markdown_core_map *map, markdown_core_map_entry *entry);
/* Removes and frees every entry stamped with `owner`. Winners for the
 * affected labels are re-elected automatically. */
void markdown_core_map_remove_owned(markdown_core_map *map, uint64_t owner);
/* Removes and frees every entry newer than `until` (the head run of the live
 * chain added since the caller snapshotted map->refs; NULL empties the map).
 * Winners for the affected labels are re-elected automatically, and no path
 * through here allocates, so the removal itself cannot fail. */
void markdown_core_map_remove_until(markdown_core_map *map, markdown_core_map_entry *until);
/* Returns a calloc'd array of the winning entry per distinct label (array
 * order unspecified) or NULL with map->oom set on allocation failure.
 * *count receives the number of distinct labels. */
markdown_core_map_entry **markdown_core_map_winners(markdown_core_map *map, size_t *count);

#ifdef __cplusplus
}
#endif

#endif
