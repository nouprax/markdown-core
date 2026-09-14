#include <stdint.h>
#include <string.h>

#include "map.h"
#include "arena.h"
#include "utf8.h"
#include "parser.h"

#define KEY_INDEX_MIN_CAPACITY 1

/* A compressed binary radix tree over length-delimited bytes. Each byte has
 * a presence bit before its eight value bits, so a prefix differs from its
 * extension even when that extension is NUL. Branch tests strictly advance
 * (byte, descending mask): at most 9 * key_len + 1 tests per search, regardless
 * of insertion order or common prefixes. No hash, entropy, or collision path.
 *
 * References encode (record index + 1) << 1, with the low bit marking leaves.
 * Each inserted key owns one leaf and (except the first) one branch in the
 * same dense allocation. Moving that allocation does not change references.
 */
static size_t leaf_ref(size_t position) { return ((position + 1) << 1) | 1; }
static size_t branch_ref(size_t position) { return (position + 1) << 1; }
static size_t ref_position(size_t ref) { return (ref >> 1) - 1; }

/* The bit a branch tests, read without branching on either the key's length
 * or the presence bit: a byte the key has reads as its value above a set
 * presence bit, and a byte past its end reads as nothing at all. Mask 256
 * therefore selects presence and every smaller mask selects a value bit,
 * which is what the two tests this replaced said in two branches. */
static unsigned key_direction(const unsigned char *key, bufsize_t length, bufsize_t byte, uint16_t mask) {
    unsigned bits = byte < length ? (unsigned)key[byte] | 0x100u : 0u;
    return (bits & mask) != 0;
}

static size_t find_leaf(markdown_core_key_index *index, const unsigned char *key, bufsize_t length) {
    size_t ref = index->root;
    MARKDOWN_CORE_DIAGNOSTIC(index->operations++;)
    while (ref && !(ref & 1)) {
        size_t position = ref_position(ref);
        const markdown_core_key_index_node *node = &index->nodes[position];
        MARKDOWN_CORE_DIAGNOSTIC(index->branch_visits++;)
        /* All keys below this branch share the preceding bytes. A shorter
         * query cannot occur here; use its resident leaf for the final check
         * (and for the insertion split) without walking an unrelated suffix. */
        if (node->byte > length || (node->byte == length && node->mask < 256)) {
            return leaf_ref(position);
        }
        ref = node->children[key_direction(key, length, node->byte, node->mask)];
    }
    return ref;
}

static int reserve_key_index(markdown_core_key_index *index, size_t needed) {
    if (needed <= index->capacity) {
        return 1;
    }
    size_t capacity = index->capacity ? index->capacity : KEY_INDEX_MIN_CAPACITY;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            return 0;
        }
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(*index->nodes)) {
        return 0;
    }
    markdown_core_key_index_node *nodes = index->mem->realloc(index->nodes, capacity * sizeof(*nodes));
    if (!nodes) {
        return 0;
    }
    index->nodes = nodes;
    index->capacity = capacity;
    return 1;
}

int markdown_core_key_index_init(markdown_core_key_index *index, markdown_core_mem *mem, size_t expected_size) {
    memset(index, 0, sizeof(*index));
    index->mem = mem;
    return reserve_key_index(index, expected_size);
}

void markdown_core_key_index_free(markdown_core_key_index *index) {
    if (index->nodes) {
        index->mem->free(index->nodes);
    }
    memset(index, 0, sizeof(*index));
}

markdown_core_key_index_slot *markdown_core_key_index_entry(markdown_core_key_index *index, const unsigned char *key,
                                                            bufsize_t key_len) {
    /* A prepared edge borrows the vector. Reject reentry before overwriting
     * that edge or reallocating it, also in Release builds. Checking only in
     * commit cannot distinguish two entries that reuse nodes[size]. */
    if (index->pending_link) {
        abort();
    }
    size_t ref = find_leaf(index, key, key_len);
    bufsize_t byte = 0;
    uint16_t mask = 256;
    if (ref) {
        markdown_core_key_index_slot *found = &index->nodes[ref_position(ref)].slot;
        bufsize_t limit = (key_len < found->key_len ? key_len : found->key_len);
        /* Both keys hold at least `limit` bytes, so whole words of the shared
         * prefix are compared as words; the loop after this one settles the
         * word that differs and the bytes below one. */
        while (limit - byte >= (bufsize_t)sizeof(uint64_t)) {
            uint64_t left, right;
            memcpy(&left, key + byte, sizeof(left));
            memcpy(&right, found->key + byte, sizeof(right));
            if (left != right) {
                break;
            }
            byte += (bufsize_t)sizeof(uint64_t);
        }
        while (byte < limit && key[byte] == found->key[byte]) {
            byte++;
        }
        if (byte == limit && key_len == found->key_len) {
            return found;
        }
        if (byte < limit) {
            unsigned difference = key[byte] ^ found->key[byte];
            mask = 128;
            while (!(difference & mask)) {
                mask >>= 1;
            }
        }
    }
    if (!reserve_key_index(index, index->size + 1)) {
        return NULL;
    }
    markdown_core_key_index_node *added = &index->nodes[index->size];
    memset(added, 0, sizeof(*added));
    added->slot.key_len = key_len;
    added->byte = byte;
    added->mask = mask;

    /* Find the incoming edge at the first differing bit. Reserve before
     * borrowing this edge: growth may relocate all of the existing nodes.
     * Publication is deferred until commit, including the first leaf. */
    size_t *link = &index->root;
    while (*link && !(*link & 1)) {
        markdown_core_key_index_node *node = &index->nodes[ref_position(*link)];
        if (node->byte > byte || (node->byte == byte && node->mask <= mask)) {
            break;
        }
        link = &node->children[key_direction(key, key_len, node->byte, node->mask)];
    }
    unsigned direction = key_direction(key, key_len, byte, mask);
    added->children[direction] = leaf_ref(index->size);
    added->children[!direction] = *link;
    index->pending_link = link;
    return &added->slot;
}

void markdown_core_key_index_commit(markdown_core_key_index *index, markdown_core_key_index_slot *entry,
                                    const unsigned char *key) {
    if (!index->pending_link || entry != &index->nodes[index->size].slot || entry->key || !key) {
        abort();
    }
    entry->key = key;
    *index->pending_link = index->size ? branch_ref(index->size) : leaf_ref(index->size);
    index->pending_link = NULL;
    index->size++;
}

int markdown_core_key_index_insert(markdown_core_key_index *index, const unsigned char *key, bufsize_t key_len,
                                   void *value, int replace, void **existing) {
    markdown_core_key_index_slot *slot = markdown_core_key_index_entry(index, key, key_len);
    if (existing) {
        *existing = slot && slot->key ? slot->value.pointer : NULL;
    }
    if (!slot) {
        return 0;
    }
    if (!slot->key) {
        markdown_core_key_index_commit(index, slot, key);
        slot->value.pointer = value;
    } else if (replace) {
        slot->value.pointer = value;
    }
    return 1;
}

void *markdown_core_key_index_lookup(markdown_core_key_index *index, const unsigned char *key, bufsize_t key_len) {
    if (!index || !index->root) {
        return NULL;
    }
    size_t ref = find_leaf(index, key, key_len);
    const markdown_core_key_index_slot *slot = &index->nodes[ref_position(ref)].slot;
    return slot->key_len == key_len && !memcmp(slot->key, key, (size_t)key_len) ? slot->value.pointer : NULL;
}

// normalize map label:  collapse internal whitespace to single space,
// remove leading/trailing whitespace, case fold
// Return NULL if the label is actually empty (i.e. composed solely from
// whitespace)
int normalize_map_label_into(markdown_core_strbuf *normalized, markdown_core_chunk *ref) {
    markdown_core_strbuf_clear(normalized);
    if (!ref || !ref->len) {
        return 0;
    }
    /* An ASCII label folds, trims and collapses its whitespace in one pass
     * over its bytes into room reserved once: only a capital letter changes
     * and only the ASCII whitespace bytes collapse, so no byte needs
     * decoding to know what becomes of it. That pass is also what finds the
     * first byte above ASCII, which abandons what it wrote and sends the
     * whole label through the scalar-aware passes -- a label is read once
     * either way, where the ASCII test used to be a pass of its own. */
    markdown_core_strbuf__grow_by(normalized, ref->len);
    if (normalized->oom) {
        return 0;
    }
    {
        unsigned char *out = normalized->ptr;
        bufsize_t written = 0, at = 0;
        bool pending_space = false;
        for (; at < ref->len; at++) {
            unsigned char byte = ref->data[at];
            if (byte >= 0x80) {
                break;
            }
            if (markdown_core_isspace((char)byte)) {
                pending_space = written > 0;
                continue;
            }
            if (pending_space) {
                out[written++] = ' ';
                pending_space = false;
            }
            out[written++] = (unsigned char)(byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte);
        }
        if (at == ref->len) {
            normalized->size = written;
            out[written] = '\0';
            return written > 0;
        }
    }
    markdown_core_strbuf_clear(normalized);
    markdown_core_utf8proc_case_fold(normalized, ref->data, ref->len);
    markdown_core_strbuf_trim(normalized);
    markdown_core_strbuf_normalize_whitespace(normalized);
    return normalized->size && !normalized->oom;
}

unsigned char *normalize_map_label(markdown_core_mem *mem, markdown_core_chunk *ref, int *lost) {
    markdown_core_strbuf normalized = MARKDOWN_CORE_BUF_INIT(mem);
    if (!normalize_map_label_into(&normalized, ref)) {
        if (normalized.oom && lost) {
            *lost = 1;
        }
        markdown_core_strbuf_free(&normalized);
        return NULL;
    }
    return markdown_core_strbuf_detach(&normalized);
}

static int index_map(markdown_core_map *map) {
    markdown_core_map_record *record;
    if (!markdown_core_key_index_init(&map->index, map->mem, map->size)) {
        return 0;
    }
    /* Construction order is independent of source order for mapped block
     * inputs. Explicit definitions precede implicit heading declarations;
     * within either class the first authored occurrence wins. */
    for (record = map->records; record; record = record->next) {
        bufsize_t length = (bufsize_t)strlen((char *)record->label);
        markdown_core_key_index_slot *slot = markdown_core_key_index_entry(&map->index, record->label, length);
        if (!slot) {
            markdown_core_key_index_free(&map->index);
            return 0;
        }
        markdown_core_map_record *existing = slot->key ? slot->value.pointer : NULL;
        if (!existing || (existing->implicit && !record->implicit) ||
            (existing->implicit == record->implicit && record->source_key <= existing->source_key)) {
            if (!existing) {
                markdown_core_key_index_commit(&map->index, slot, record->label);
            }
            slot->value.pointer = record;
        }
    }
    map->size = map->index.size;
    map->prepared = 1;
    return 1;
}

/* Normalization trims leading whitespace and folds case, and folds an ASCII
 * letter to its lowercase byte, so a label's folded first significant byte is
 * its key's first byte. When no key begins with that byte the label cannot
 * match, and the lookup ends here without folding a thing. A label whose
 * first significant scalar is not ASCII takes the full normalization. */
static int map_may_hold(const markdown_core_map *map, const markdown_core_chunk *label) {
    const unsigned char *at = label->data, *end = label->data + label->len;
    while (at < end && markdown_core_isspace((char)*at)) {
        at++;
    }
    if (at == end) {
        return 0;
    }
    unsigned char first = *at;
    if (first >= 0x80) {
        return 1;
    }
    if (first >= 'A' && first <= 'Z') {
        first = (unsigned char)(first + ('a' - 'A'));
    }
    return (map->first_bytes[first >> 6] >> (first & 63)) & 1;
}

markdown_core_map_record *markdown_core_map_lookup(markdown_core_map *map, markdown_core_chunk *label) {
    if (label->len < 1 || label->len > MAX_LINK_LABEL_LENGTH || !map || !map->size || map->oom ||
        !map_may_hold(map, label)) {
        return NULL;
    }
    MARKDOWN_CORE_DIAGNOSTIC(map->fold_work += (size_t)label->len;)
    if (!normalize_map_label_into(&map->label_buffer, label)) {
        map->oom = map->label_buffer.oom;
        return NULL;
    }
    if (!map->prepared && !index_map(map)) {
        map->oom = 1;
        return NULL;
    }
    return markdown_core_key_index_lookup(&map->index, map->label_buffer.ptr, map->label_buffer.size);
}

void markdown_core_map_free(markdown_core_map *map) {
    markdown_core_map_record *record;

    if (map == NULL) {
        return;
    }

    for (record = map->records; record; record = record->next) {
        /* The map's holder goes; a resource some node still reads through
         * stays with that node, which is how the tree outlives the parser. */
        markdown_core_resource_release(map->mem, record->resource);
    }
    markdown_core_arena_free(map->arena);

    markdown_core_key_index_free(&map->index);
    markdown_core_strbuf_free(&map->label_buffer);
    map->mem->free(map);
}

void *markdown_core_map_allocate(markdown_core_map *map, size_t size) {
    if (!map->arena) {
        map->arena = markdown_core_arena_new(map->mem);
        if (!map->arena) {
            return NULL;
        }
    }
    return markdown_core_arena_alloc(map->arena, size);
}

markdown_core_map *markdown_core_map_new(markdown_core_mem *mem) {
    markdown_core_map *map = (markdown_core_map *)mem->calloc(1, sizeof(markdown_core_map));
    if (!map) {
        return NULL;
    }
    map->mem = mem;
    map->label_buffer = (markdown_core_strbuf)MARKDOWN_CORE_BUF_INIT(mem);
    return map;
}
