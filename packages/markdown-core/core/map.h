#ifndef MARKDOWN_CORE_MAP_H
#define MARKDOWN_CORE_MAP_H

#include "buffer.h"
#include "chunk.h"

#ifdef __cplusplus
extern "C" {
#endif

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

/* `ref` in a link label's normal form, into caller-owned scratch; false for
 * a label that normalizes to nothing or on OOM, which the scratch records. */
int normalize_map_label_into(markdown_core_strbuf *normalized, const markdown_core_chunk *ref);
/* The same, as an allocation the caller frees: NULL when it normalizes to
 * nothing, with `*lost` set when storage could not be had. */
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

#ifdef __cplusplus
}
#endif

#endif
