#ifndef MARKDOWN_CORE_MAP_H
#define MARKDOWN_CORE_MAP_H

#include "chunk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An entry is a normalized LABEL and the IDENTITY of the definition block that
 * registered it -- nothing else. It used to carry a `size` (the byte count
 * D9's expansion budget charged; a reference that names its definition copies
 * nothing) and then an `age` (its position in registration order, the
 * first-wins tiebreak); the identity subsumes the age, because block mints are
 * monotone in document order (D4), so DOCUMENT ORDER IS ON THE VALUE ITSELF
 * and registration order decides nothing at all.
 *
 * `definition` is a value, never a node: a map that owned a node is how a
 * definition nested inside another came to be freed while the tree still
 * pointed at it (D11). Both preparation paths below fold duplicates to the
 * SMALLEST identity, so the entry a lookup answers with carries the
 * first-in-document-order winner's, which is the tiebreak the model
 * specifies. */
struct markdown_core_map_entry {
    struct markdown_core_map_entry *next;
    unsigned char *label;
    uint32_t definition;
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
     * counts every insert, duplicates included -- it sizes the next
     * preparation and answers "is there anything to look up" -- and no
     * preparation rewrites it (§12.4). */
    size_t sorted_size;
    markdown_core_key_index index;
    size_t size;
    /* THE GENERATION (docs/STREAMING.md T4): advanced by every insert, never
     * by a lookup. The other half of the projection cache's key -- a
     * projection taken at one generation resolved against exactly the
     * definitions a projection at the same generation would. Every insert
     * counts, a duplicate label included: the fold that makes the first one
     * win happens at preparation, and a spurious invalidation is a slow feed
     * where a missed one would be a wrong tree. */
    size_t generation;
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
