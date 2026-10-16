#ifndef MARKDOWN_CORE_REGISTRY_H
#define MARKDOWN_CORE_REGISTRY_H

#include <stdbool.h>
#include <stdint.h>
#include "map.h"
#include "node.h"

#ifdef __cplusplus
extern "C" {
#endif

/* THE REGISTRY: everything a parse declares that a later lookup asks for, as
 * one append-only log and one index over it.
 *
 * A record states a kind, a label in that kind's normal form, and the source
 * offset where the declaring definition starts. The kinds are lookup spaces:
 *
 *   REFERENCE  the label of a link reference definition, and the label a
 *              heading's text declares -- one space, which a reference
 *              occurrence resolves against;
 *   FOOTNOTE   the label of a footnote definition;
 *   SPECIMEN   the label of a specimen definition;
 *   ANCHOR     an explicit anchor, reserved where completion meets it.
 *
 * Reference and footnote labels are in the reference-label normal form
 * (markdown_core_registry_normal_label); a specimen label and an anchor are
 * their bytes as written.
 *
 * The index maps a kind and a label to the key's latest record, and each
 * record names the key's record before it and the earliest start among the
 * records up to it. So a lookup answers whether the key is declared at all
 * and whether some declaration of it starts before a position, and a
 * rollback pops records back to a mark, each pop pointing the index at what
 * it pointed at before that record was taken. A declaration only appends to
 * the log: a lookup of a kind has the index take that kind's records beyond
 * it, in log order, room for all of them made at once. So the declarations
 * of a kind a parse makes before its first lookup of that kind are indexed
 * together, a kind no lookup asks for is never indexed, and a record rolled
 * back before a lookup of its kind never is. The log holds no node: a
 * record's label is the registry's own copy. */
typedef enum {
    MARKDOWN_CORE_REGISTRY_REFERENCE = 1,
    MARKDOWN_CORE_REGISTRY_FOOTNOTE,
    MARKDOWN_CORE_REGISTRY_SPECIMEN,
    MARKDOWN_CORE_REGISTRY_ANCHOR
} markdown_core_registry_kind;

typedef struct markdown_core_registry_record {
    /* Once the index has taken the record: the key's record before this
     * one, as its position in the log plus one, zero when this is the key's
     * first; and the earliest start among this key's records up to this
     * one. */
    size_t previous;
    /* Where the declaring definition starts, and the earliest start. */
    uint32_t start, earliest;
    /* The key: the kind's byte, then the label, then NUL. */
    bufsize_t key_len;
    unsigned char key[];
} markdown_core_registry_record;

/* Storage records are carved from, newest first (registry.c). */
typedef struct markdown_core_registry_block markdown_core_registry_block;

/* One kind's records in the log. */
typedef struct {
    /* How many records are of the kind: a kind none declares answers every
     * lookup without its key being assembled. */
    size_t records;
    /* How many of them the index has taken: every one before `cursor` in
     * the log, and none after it. */
    size_t indexed, cursor;
} markdown_core_registry_space;

typedef struct {
    markdown_core_registry_record **records;
    size_t count, capacity;
    markdown_core_registry_space spaces[MARKDOWN_CORE_REGISTRY_ANCHOR + 1];
    /* Each key's latest record among the records the index has taken, as
     * its position in the log. The index borrows the key bytes from that
     * record. */
    markdown_core_key_index index;
    markdown_core_registry_block *blocks;
    size_t block_used, block_size;
    /* Where a lookup assembles its key. */
    markdown_core_strbuf key;
} markdown_core_registry;

/* A point the log can roll back to. */
typedef struct {
    size_t count;
    markdown_core_registry_block *blocks;
    size_t block_used, block_size;
} markdown_core_registry_mark;

void markdown_core_registry_init(markdown_core_registry *registry);
void markdown_core_registry_free(markdown_core_registry *registry);

/* Append a record of `kind` for `label`, already in the kind's normal form,
 * declared by a definition starting at `start`. An empty label declares
 * nothing. False when storage could not be had. */
bool markdown_core_registry_declare(markdown_core_registry *registry, markdown_core_registry_kind kind,
                                    const unsigned char *label, bufsize_t length, uint32_t start);

/* The latest record of `kind` for `label`, in the kind's normal form, or NULL
 * when nothing declares it. `*failed` is set when the key could not be
 * assembled or the index could not take the log, and the answer is then
 * NULL. */
const markdown_core_registry_record *markdown_core_registry_find(markdown_core_registry *registry,
                                                                 markdown_core_registry_kind kind,
                                                                 const unsigned char *label, bufsize_t length,
                                                                 bool *failed);

/* The latest record of `kind` for the reference-label normal form of `label`,
 * as written, or NULL when nothing declares it; a label that normalizes to
 * nothing, or is longer than a link label may be, is declared by nothing.
 * `*failed` as above. */
const markdown_core_registry_record *markdown_core_registry_find_label(markdown_core_registry *registry,
                                                                       markdown_core_registry_kind kind,
                                                                       const markdown_core_chunk *label, bool *failed);

/* `label`'s reference-label normal form, in storage from `pool` the node
 * that holds it owns (markdown_core_node_pool_bytes), NUL-terminated; empty
 * when it normalizes to nothing. False when storage could not be had. */
bool markdown_core_registry_normal_label(markdown_core_registry *registry, markdown_core_node_pool *pool,
                                         const markdown_core_chunk *label, markdown_core_chunk *normal);

/* The record's label, borrowed from the registry. */
static inline markdown_core_chunk markdown_core_registry_label(const markdown_core_registry_record *record) {
    markdown_core_chunk label = {(unsigned char *)record->key + 1, record->key_len - 1, 0};
    return label;
}

/* Whether some declaration of the record's key, up to the record, starts
 * before `position`. */
static inline bool markdown_core_registry_starts_before(const markdown_core_registry_record *record,
                                                        uint32_t position) {
    return record->earliest < position;
}

/* The point the log is at now. */
markdown_core_registry_mark markdown_core_registry_mark_now(const markdown_core_registry *registry);
/* Pop every record appended after `mark`, last first, and release the
 * storage they were carved from. */
void markdown_core_registry_rollback(markdown_core_registry *registry, markdown_core_registry_mark mark);

#ifdef __cplusplus
}
#endif

#endif
