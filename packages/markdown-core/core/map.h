#ifndef MARKDOWN_CORE_MAP_H
#define MARKDOWN_CORE_MAP_H

#include "chunk.h"

#ifdef __cplusplus
extern "C" {
#endif

struct markdown_core_resource;

/* A record is a normalized LABEL and, for a link reference definition, the
 * RESOURCE the definition stated -- destination and title -- owned once, here,
 * and shared by every occurrence that resolves to it (M2). It used to carry a
 * `size`, which was the number of bytes resolving against it copied into a
 * node -- the quantity D9's expansion budget charged. A reference that shares
 * its definition's resource copies nothing, so there is nothing to charge and
 * no field to carry it. A footnote definition's record has no resource. */
struct markdown_core_map_record {
    struct markdown_core_map_record *next;
    struct markdown_core_resource *resource;
    unsigned char label[];
};

typedef struct markdown_core_map_record markdown_core_map_record;

typedef struct markdown_core_key_index_slot {
    uint64_t hash;
    const unsigned char *key;
    bufsize_t key_len;
    union {
        void *pointer;
        size_t counter;
    } value;
} markdown_core_key_index_slot;

typedef struct markdown_core_key_index {
    markdown_core_mem *mem;
    markdown_core_key_index_slot *slots;
    size_t capacity;
    size_t size;
} markdown_core_key_index;

struct markdown_core_map {
    markdown_core_mem *mem;
    markdown_core_map_record *records;
    markdown_core_key_index index;
    size_t size;
    int prepared;
    markdown_core_strbuf label_buffer;
    /* Sticky flag: any allocation failure is terminal for the owning parse. */
    int oom;
};

typedef struct markdown_core_map markdown_core_map;

/* Reuses caller-owned scratch; returns false for empty labels or OOM. */
int normalize_map_label_into(markdown_core_strbuf *normalized, markdown_core_chunk *ref);
unsigned char *normalize_map_label(markdown_core_mem *mem, markdown_core_chunk *ref, int *lost);
int markdown_core_key_index_init(markdown_core_key_index *index, markdown_core_mem *mem, size_t expected_size);
void markdown_core_key_index_free(markdown_core_key_index *index);
/* Find an occupied or vacant entry, growing only for a new key. NULL means
 * allocation failure. The entry is borrowed until the next insertion; a
 * vacant entry must be committed before another index operation. */
markdown_core_key_index_slot *markdown_core_key_index_entry(markdown_core_key_index *index, const unsigned char *key,
                                                            bufsize_t key_len);
void markdown_core_key_index_commit(markdown_core_key_index *index, markdown_core_key_index_slot *entry,
                                    const unsigned char *key);
int markdown_core_key_index_insert(markdown_core_key_index *index, const unsigned char *key, bufsize_t key_len,
                                   void *value, int replace, void **existing);
void *markdown_core_key_index_lookup(const markdown_core_key_index *index, const unsigned char *key, bufsize_t key_len);
markdown_core_map *markdown_core_map_new(markdown_core_mem *mem);
void markdown_core_map_free(markdown_core_map *map);
markdown_core_map_record *markdown_core_map_lookup(markdown_core_map *map, markdown_core_chunk *label);

#ifdef __cplusplus
}
#endif

#endif
