#ifndef MARKDOWN_CORE_MAP_H
#define MARKDOWN_CORE_MAP_H

#include "chunk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* AN INDEX OF BYTE KEYS: open addressing with linear probing, a key's
 * bytes borrowed from what its value owns. */
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
    markdown_core_key_index_slot *slots;
    size_t capacity;
    size_t size;
} markdown_core_key_index;

/* Reuses caller-owned scratch; returns false for empty labels or OOM. */
int normalize_map_label_into(markdown_core_strbuf *normalized, markdown_core_chunk *ref);
unsigned char *normalize_map_label(markdown_core_chunk *ref, int *lost);
int markdown_core_key_index_init(markdown_core_key_index *index, size_t expected_size);
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
/* Removes `key`, which the index holds. */
void markdown_core_key_index_remove(markdown_core_key_index *index, const unsigned char *key, bufsize_t key_len);

#ifdef __cplusplus
}
#endif

#endif
