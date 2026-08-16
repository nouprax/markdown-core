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
    markdown_core_key_index index;    /* hash path: label -> winner */
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
int markdown_core_key_index_remove(
    markdown_core_key_index *index,
    const unsigned char *key,
    markdown_core_bufsize key_len
);
markdown_core_map *markdown_core_map_new(markdown_core_mem *mem, markdown_core_map_free_func free);
void markdown_core_map_free(markdown_core_map *map);
markdown_core_map_entry *markdown_core_map_lookup(markdown_core_map *map, markdown_core_chunk *label);

/** The lookup, plus what it asked: `*hash` receives the hash of the
 * normalized label (0 when the label could never match), so the unit doing
 * the asking can remember the question and be asked again when a definition
 * for it arrives — a lookup's answer is document-wide and can change. */
markdown_core_map_entry *markdown_core_map_lookup_probe(
    markdown_core_map *map,
    markdown_core_chunk *label,
    uint64_t *hash
);

/** The hash a probe records for a normalized label; the same function the
 * definition side uses, so the two meet. */
uint64_t markdown_core_map_label_hash(const unsigned char *label);

/** Whether `entry`, just added, is its label's winner — the first definition
 * of that label — and so changes what a lookup answers. */
int markdown_core_map_entry_wins(markdown_core_map *map, markdown_core_map_entry *entry);

/** Drops the newest entries until `size` remain — what a speculative close
 * harvested, taken back — and forgets any prepared lookup structure. */
void markdown_core_map_truncate(markdown_core_map *map, size_t size);
/* Links a freshly created entry (label filled by the caller) into the
 * map: stamps the next document order, pushes it onto the live chain, and
 * keeps any prepared lookup structure coherent. */
void markdown_core_map_add(markdown_core_map *map, markdown_core_map_entry *entry);

/* --- THE PROBE INDEX: the tables' inverse ------------------------------- */

/* Every unit's inline parse asks the definition tables about labels — hits
 * and misses alike — and when a definition for one of those labels arrives
 * later, the units whose answer it changes are exactly the ones that asked.
 * A unit's PROBES are those labels, as hashes, and they are what the parser
 * would otherwise walk the whole tree to find; so they are threaded, label
 * by label, through one index: a link per (unit, label), on a chain of every
 * link with that label's hash, and a definition finds the units it flips in
 * the size of that chain and nothing else. The chains are intrusive and
 * doubly linked, so a unit's probes leave the index in the size of the
 * probes when they are freed — from any code, with no parser in hand — and
 * the index outlives the parser that made it for as long as any link
 * remains (a one-shot parse hands its tree on and dies first). */
struct markdown_core_node;
struct markdown_core_probe_index;

struct markdown_core_probe_link {
    uint64_t hash;
    struct markdown_core_node *unit;
    struct markdown_core_probe_link *next;
    struct markdown_core_probe_link **pprev; /* the pointer that points here: a bucket, or a link's `next` */
};

struct markdown_core_probes {
    struct markdown_core_probe_index *index;
    size_t count;
    struct markdown_core_probe_link links[1];
};

typedef struct markdown_core_probe_index {
    markdown_core_mem *mem;
    struct markdown_core_probe_link **buckets;
    size_t bucket_count;
    size_t link_count;
    /* The parser that owned this index is gone; the last link to leave frees
     * it. */
    int orphaned;
} markdown_core_probe_index;

markdown_core_probe_index *markdown_core_probe_index_new(markdown_core_mem *mem);

/* The parser lets go: freed now if no link remains, else by the last one. */
void markdown_core_probe_index_release(markdown_core_probe_index *index);

/* Threads `count` labels into the index as `unit`'s probes and hands the
 * probes back for the unit to own; NULL on allocation failure (nothing was
 * threaded). */
struct markdown_core_probes *markdown_core_probes_attach(
    markdown_core_probe_index *index,
    struct markdown_core_node *unit,
    const uint64_t *hashes,
    size_t count
);

/* Unthreads and frees a unit's probes; tolerates NULL. */
void markdown_core_probes_free(struct markdown_core_probes *probes);

/* Every unit that asked about `hash`, appended to `*units` (grown with
 * `mem`; a unit that asked more than once appears once per ask — callers
 * dedupe). False on allocation failure. */
int markdown_core_probe_index_units(
    const markdown_core_probe_index *index,
    uint64_t hash,
    markdown_core_mem *mem,
    struct markdown_core_node ***units,
    size_t *count,
    size_t *capacity
);

#ifdef __cplusplus
}
#endif

#endif
