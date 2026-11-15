#ifndef MARKDOWN_CORE_MAP_H
#define MARKDOWN_CORE_MAP_H

#include "chunk.h"
#include "measure.h"

#ifdef __cplusplus
extern "C" {
#endif

/* AN OPEN-ADDRESSED INDEX FROM BYTE KEYS TO A WORD AND A 32-BIT NUMBER, by
 * linear probing. It borrows its keys: a key's bytes stay where the caller
 * keeps them for as long as the key is in the index, and the caller may
 * point an entry at other bytes that hold the same key. The number takes the
 * room the slot's alignment leaves after the key's length. A vacant entry
 * holds zero in both. */
typedef struct markdown_core_key_index_slot {
    uint64_t hash;
    const unsigned char *key;
    bufsize_t key_len;
    uint32_t extra;
    size_t value;
} markdown_core_key_index_slot;

typedef struct markdown_core_key_index {
    markdown_core_key_index_slot *slots;
    size_t capacity;
    size_t size;
} markdown_core_key_index;

/* The hash a name is filed under: its measure (measure.h), mixed so
 * that keys that differ in their last bytes alone, whose sums differ in
 * their low bits alone, spread over the table. A key's hash follows from
 * the measures of its parts (markdown_core_measure_join), so a key read in
 * parts is found as its bytes whole are. Never zero. */
static MARKDOWN_CORE_INLINE uint64_t markdown_core_key_index_hash(markdown_core_measure measure) {
    uint64_t hash = measure.sum ^ (uint64_t)measure.size * UINT64_C(0x9e3779b97f4a7c15);
    hash ^= hash >> 33;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    hash ^= hash >> 33;
    hash *= UINT64_C(0xc4ceb9fe1a85ec53);
    hash ^= hash >> 33;
    return hash ? hash : 1;
}

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
/* KEYS UNDER A HASH THEIR USER GIVES. An index files a key under one hash
 * for its whole life: a byte key under markdown_core_key_hash, which the
 * calls above give, or under one its user computes, which the calls below
 * take: a name kept in pieces under a hash of its parts' bytes read in
 * turn (markdown_core_key_stream), or under its measure's
 * (markdown_core_key_index_hash), which follows from the measures of its
 * parts without reading them; either way it is found as its bytes whole
 * would be. The calls that take a
 * `probe` and `holds` tell an occupied slot of the hash and length holds the
 * key by asking `holds`; such a slot's `key` need only not be NULL. */
typedef bool (*markdown_core_key_holds)(const void *probe, const markdown_core_key_index_slot *slot);
uint64_t markdown_core_key_hash(const unsigned char *key, bufsize_t key_len);
/* A KEY READ IN PARTS: the hash of the bytes of the parts one after
 * another, the parts' bounds aside, eight bytes at a time as they come and
 * the last few held until the end (markdown_core_key_stream_end). A stream
 * copied stands for its bytes so far, so that one key and several endings
 * are read once for the key. Not markdown_core_key_hash's value: an index
 * files its keys under one of the two. */
typedef struct {
    uint64_t hash;
    size_t size;
    /* The bytes past the last whole eight, the first lowest, the rest of
     * the word zero. */
    uint64_t rest;
    unsigned held;
} markdown_core_key_stream;

/* Eight bytes as one number, the first lowest, as the bytes a stream holds
 * one by one make it: a key read in other parts reads the same words. */
static MARKDOWN_CORE_INLINE uint64_t markdown_core_key_stream_word(const unsigned char *bytes) {
    uint64_t word;
    memcpy(&word, bytes, sizeof(word));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    word = __builtin_bswap64(word);
#endif
    return word;
}

static MARKDOWN_CORE_INLINE uint64_t markdown_core_key_stream_mix(uint64_t hash, uint64_t word) {
    hash = (hash ^ word) * UINT64_C(0x9e3779b97f4a7c15);
    return hash ^ hash >> 32;
}

static MARKDOWN_CORE_INLINE void markdown_core_key_stream_start(markdown_core_key_stream *stream) {
    stream->hash = UINT64_C(1469598103934665603);
    stream->size = 0;
    stream->rest = 0;
    stream->held = 0;
}

/* Fewer than eight bytes as one number, the first lowest: read in two
 * halves or three bytes that overlap, which put the bytes both read in one
 * place. */
static MARKDOWN_CORE_INLINE uint64_t markdown_core_key_stream_few(const unsigned char *bytes, size_t size) {
    if (size >= 4) {
        uint32_t front, back;
        memcpy(&front, bytes, sizeof(front));
        memcpy(&back, bytes + size - 4, sizeof(back));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        front = __builtin_bswap32(front);
        back = __builtin_bswap32(back);
#endif
        return (uint64_t)front | (uint64_t)back << (8 * (size - 4));
    }
    if (!size) {
        return 0;
    }
    return (uint64_t)bytes[0] | (uint64_t)bytes[size / 2] << (8 * (size / 2)) |
           (uint64_t)bytes[size - 1] << (8 * (size - 1));
}

static MARKDOWN_CORE_INLINE void markdown_core_key_stream_add(markdown_core_key_stream *stream,
                                                              const unsigned char *bytes, size_t size) {
    stream->size += size;
    unsigned held = stream->held;
    uint64_t hash = stream->hash, rest = stream->rest;
    if (held) {
        const size_t take = size < 8 - held ? size : 8 - held;
        rest |= markdown_core_key_stream_few(bytes, take) << (8 * held);
        held += (unsigned)take;
        bytes += take;
        size -= take;
        if (held == 8) {
            hash = markdown_core_key_stream_mix(hash, rest);
            rest = 0;
            held = 0;
        }
    }
    for (; size >= 8; bytes += 8, size -= 8) {
        hash = markdown_core_key_stream_mix(hash, markdown_core_key_stream_word(bytes));
    }
    if (size) {
        /* Past the whole eights, so none is held. */
        rest = markdown_core_key_stream_few(bytes, size);
        held = (unsigned)size;
    }
    stream->hash = hash;
    stream->rest = rest;
    stream->held = held;
}

static MARKDOWN_CORE_INLINE uint64_t markdown_core_key_stream_end(const markdown_core_key_stream *stream) {
    uint64_t hash = stream->hash;
    if (stream->held) {
        hash = markdown_core_key_stream_mix(hash, stream->rest);
    }
    hash ^= (uint64_t)stream->size * UINT64_C(0x9e3779b97f4a7c15);
    hash ^= hash >> 33;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    hash ^= hash >> 33;
    hash *= UINT64_C(0xc4ceb9fe1a85ec53);
    hash ^= hash >> 33;
    return hash ? hash : 1;
}
markdown_core_key_index_slot *markdown_core_key_index_entry_hashed(markdown_core_key_index *index, uint64_t hash,
                                                                   const unsigned char *key, bufsize_t key_len);
markdown_core_key_index_slot *markdown_core_key_index_find_hashed(const markdown_core_key_index *index, uint64_t hash,
                                                                  const unsigned char *key, bufsize_t key_len);
markdown_core_key_index_slot *markdown_core_key_index_place(markdown_core_key_index *index, uint64_t hash,
                                                            bufsize_t key_len, markdown_core_key_holds holds,
                                                            const void *probe);
markdown_core_key_index_slot *markdown_core_key_index_seek(const markdown_core_key_index *index, uint64_t hash,
                                                           bufsize_t key_len, markdown_core_key_holds holds,
                                                           const void *probe);
/* Remove an occupied entry. Every other entry stays findable; entries the
 * index holds may move, so no entry pointer survives the call. */
void markdown_core_key_index_remove(markdown_core_key_index *index, markdown_core_key_index_slot *entry);

/* A KEYED ENTRY: a byte key, a 32-bit number and a pointer. Keys are in
 * byte order, a shorter key first on a common prefix. The first eight bytes,
 * big-endian and padded with zero bytes, are the `head`, which orders two
 * keys whose heads differ; equal heads leave it to the lengths, when either
 * key is all in its head, or to the bytes past the eighth. A key of at most
 * eight bytes is all in its head, so its bytes are never read: a number is
 * the key of its big-endian bytes (markdown_core_key_number). */
typedef struct markdown_core_key_entry {
    uint64_t head;
    const unsigned char *key;
    bufsize_t key_len;
    uint32_t extra;
    const void *value;
} markdown_core_key_entry;

static inline uint64_t markdown_core_key_head(const unsigned char *key, bufsize_t key_len) {
    const bufsize_t bytes = key_len < 8 ? key_len : 8;
    uint64_t head = 0;
    for (bufsize_t i = 0; i < bytes; i++) {
        head = head << 8 | key[i];
    }
    /* The padding: a shift by 64 is undefined, and no bytes is a zero head. */
    return bytes ? head << 8 * (8 - bytes) : 0;
}

/* The entry of key `number`, its four big-endian bytes. */
static inline markdown_core_key_entry markdown_core_key_number(uint32_t number) {
    const markdown_core_key_entry entry = {(uint64_t)number << 32, NULL, 4, number, NULL};
    return entry;
}

/* The order of two keys with equal heads. */
static inline int markdown_core_key_tie_order(const markdown_core_key_entry *a, const markdown_core_key_entry *b) {
    if (a->key_len <= 8 || b->key_len <= 8) {
        return (a->key_len > b->key_len) - (a->key_len < b->key_len);
    }
    const bufsize_t common = (a->key_len < b->key_len ? a->key_len : b->key_len) - 8;
    const int order = memcmp(a->key + 8, b->key + 8, (size_t)common);
    return order ? order : (a->key_len > b->key_len) - (a->key_len < b->key_len);
}

/* Whether `a`'s key sorts before `b`'s, and whether the two keys are the
 * same. */
static inline bool markdown_core_key_before(const markdown_core_key_entry *a, const markdown_core_key_entry *b) {
    return a->head != b->head ? a->head < b->head : markdown_core_key_tie_order(a, b) < 0;
}

static inline bool markdown_core_key_same(const markdown_core_key_entry *a, const markdown_core_key_entry *b) {
    return a->head == b->head && markdown_core_key_tie_order(a, b) == 0;
}

#ifdef __cplusplus
}
#endif

#endif
