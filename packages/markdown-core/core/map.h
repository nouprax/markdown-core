#ifndef MARKDOWN_CORE_MAP_H
#define MARKDOWN_CORE_MAP_H

#include "chunk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* AN OPEN-ADDRESSED INDEX FROM BYTE KEYS TO A WORD, by linear probing. It
 * borrows its keys: a key's bytes stay where the caller keeps them for as
 * long as the key is in the index. */
typedef struct markdown_core_key_index_slot {
    uint64_t hash;
    const unsigned char *key;
    bufsize_t key_len;
    size_t value;
} markdown_core_key_index_slot;

typedef struct markdown_core_key_index {
    markdown_core_key_index_slot *slots;
    size_t capacity;
    size_t size;
} markdown_core_key_index;

int markdown_core_key_index_init(markdown_core_key_index *index, size_t expected_size);
/* Make room for `size` keys in all, so that taking that many grows the index
 * no further. Zero means allocation failure. */
int markdown_core_key_index_reserve(markdown_core_key_index *index, size_t size);
void markdown_core_key_index_free(markdown_core_key_index *index);
/* Find an occupied or vacant entry, growing only for a new key. NULL means
 * allocation failure. The entry is borrowed until the next insertion; a
 * vacant entry must be committed before another index operation. */
markdown_core_key_index_slot *markdown_core_key_index_entry(markdown_core_key_index *index, const unsigned char *key,
                                                            bufsize_t key_len);
void markdown_core_key_index_commit(markdown_core_key_index *index, markdown_core_key_index_slot *entry,
                                    const unsigned char *key);
/* The occupied entry for `key`, or NULL; never grows the index. */
markdown_core_key_index_slot *markdown_core_key_index_find(const markdown_core_key_index *index,
                                                           const unsigned char *key, bufsize_t key_len);
/* Remove an occupied entry. Every other entry stays findable; entries the
 * index holds may move, so no entry pointer survives the call. */
void markdown_core_key_index_remove(markdown_core_key_index *index, markdown_core_key_index_slot *entry);

#ifdef __cplusplus
}
#endif

#endif
