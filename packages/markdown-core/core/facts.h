#ifndef MARKDOWN_CORE_FACTS_H
#define MARKDOWN_CORE_FACTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "node.h"

#ifdef __cplusplus
extern "C" {
#endif

/* FACTS AND REGISTRIES (docs/plans/2026-09-29-incremental-parsing.md, 5.1
 * and 5.7). A fact is what one place of the source declares to the whole
 * document, or one question an inline root asked of it: a reference
 * definition, a heading, a footnote or specimen definition, an explicit
 * anchor, or a lookup of a key. A registry is the source-ordered sequence of
 * the facts of one kind, held in the structure every sequence of the engine
 * shares (children.h): a REGISTRY node whose children are FACT entries, each
 * placing its fact at the distance from the fact before it. The edit pass
 * moves a registry as it moves a relation, a parse joins the runs of the
 * facts inside the nodes it took to the facts of the nodes it read, and a
 * position is a prefix sum, in O(log n) wherever it is.
 *
 * A fact is a counted value an entry points to. A parse that joins a run of
 * old entries after other facts than the ones they followed places the
 * run's first fact again, in an entry of its own, and the fact is the same
 * one, so what indexes a fact (the session's label index, registry.c) never
 * follows a move. Facts of one registry are in the order of their ORDER
 * keys, which grow with their positions: two facts compare in O(1), and a
 * fact's position is found from its key in O(log n). */
typedef enum {
    MARKDOWN_CORE_FACT_REFERENCE,
    MARKDOWN_CORE_FACT_HEADING,
    MARKDOWN_CORE_FACT_FOOTNOTE,
    MARKDOWN_CORE_FACT_SPECIMEN,
    MARKDOWN_CORE_FACT_ANCHOR,
    MARKDOWN_CORE_FACT_LOOKUP,
    MARKDOWN_CORE_FACT_KINDS
} markdown_core_fact_kind;

struct markdown_core_registry_entry;

typedef struct markdown_core_fact {
    size_t refs;
    /* The fact's place in its registry's order; 0 until the parse that
     * declared it commits (registry.c). */
    uint64_t order;
    /* While the parse that declared it runs, where it is. */
    uint32_t position;
    uint8_t kind;
    /* Whether the round that committed last dropped it from its lists. */
    bool dropped;
    /* An ANCHOR's spelling, and a HEADING's anchor once it has one. Owned. */
    markdown_core_chunk anchor;
    /* A HEADING's anchor base (heading.c), absent when its anchor is
     * authored. Owned. */
    markdown_core_optional_chunk base;
    /* A REFERENCE's resource, and a HEADING's target. Held. */
    markdown_core_resource *resource;
    /* A FOOTNOTE's or SPECIMEN's definition. Held. */
    struct markdown_core_node *node;
    /* The session's index entries that list the fact (registry.c): the one
     * of its label, and a HEADING's family's; a LOOKUP's key. */
    struct markdown_core_registry_entry *entries[2];
    /* A LOOKUP's neighbours among the lookups of its key. */
    struct markdown_core_fact *previous, *next;
} markdown_core_fact;

/* A new fact of `kind`, held once, or NULL when storage ran out. */
markdown_core_fact *markdown_core_fact_new(markdown_core_fact_kind kind);

static inline markdown_core_fact *markdown_core_fact_retain(markdown_core_fact *fact) {
    fact->refs++;
    return fact;
}

/* Drops a hold on `fact`; the last one gives back what it holds into `pool`
 * (node.c). */
void markdown_core_fact_release(markdown_core_node_pool *pool, markdown_core_fact *fact);

/* The fact a registry entry places. */
static inline markdown_core_fact *markdown_core_fact_of(const struct markdown_core_node *entry) {
    return entry->as.fact_place->fact;
}

/* An empty registry, held once, or NULL when storage ran out. */
struct markdown_core_node *markdown_core_registry_new(markdown_core_node_pool *pool);

/* The number of facts in `registry`. */
static inline size_t markdown_core_registry_count(const struct markdown_core_node *registry) {
    return markdown_core_children_count(registry->children);
}

/* The index of the first fact of `registry` at or after `position`; the
 * count when there is none. */
size_t markdown_core_registry_seek(const struct markdown_core_node *registry, int64_t position);

/* Where the fact at `index`, below the count, is. */
int64_t markdown_core_registry_position(const struct markdown_core_node *registry, size_t index);

/* The index of the first fact of `registry`, whose facts `below` orders,
 * for which `below` is false: the count when it holds for all. `below`
 * answers for `key`, and holds for a prefix of the facts. */
size_t markdown_core_registry_search(const struct markdown_core_node *registry,
                                     bool (*below)(const markdown_core_fact *fact, const void *key), const void *key);

/* The index of `fact` in `registry`, which holds it, found by its order. */
size_t markdown_core_registry_rank(const struct markdown_core_node *registry, const markdown_core_fact *fact);

/* A FACT entry placing `fact`, held once more, at `lead`, or NULL when
 * storage ran out. */
struct markdown_core_node *markdown_core_registry_place(markdown_core_node_pool *pool, markdown_core_fact *fact,
                                                        int64_t lead);

/* A REGISTRY BEING BUILT in source order: new facts and runs of an old
 * registry's facts, each after the last. The new facts take their orders
 * last, between the facts around them, a window of their neighbours
 * renumbered when those leave no room (Dietz and Sleator's order
 * maintenance): O(log n) amortized per fact. */
typedef struct {
    markdown_core_node_pool *pool;
    struct markdown_core_node *registry;
    /* Where the last fact put is. */
    int64_t end;
    /* The runs of new facts put, as [first, first + count) indices. */
    struct {
        size_t first, count;
    } *fresh;
    size_t fresh_count, fresh_capacity;
} markdown_core_registry_builder;

bool markdown_core_registry_build_begin(markdown_core_registry_builder *builder, markdown_core_node_pool *pool);
/* Puts `fact`, which has no order yet, at `position`, holding it once more. */
bool markdown_core_registry_build_put(markdown_core_registry_builder *builder, markdown_core_fact *fact,
                                      int64_t position);
/* Puts the facts [first, first + count) of `old`, the first of which is at
 * `position`, sharing its runs. */
bool markdown_core_registry_build_join(markdown_core_registry_builder *builder, const struct markdown_core_node *old,
                                       size_t first, size_t count, int64_t position);
/* The registry built, sealed, or NULL, with the builder's work given back,
 * when `ok` is false or a step ran out of storage. Its new facts take their
 * orders at `markdown_core_registry_build_order`, which nothing can fail. */
struct markdown_core_node *markdown_core_registry_build_end(markdown_core_registry_builder *builder, bool ok);
/* Orders the new facts of the registry the builder built, renumbering the
 * window of old facts around them that leaves them no room, and ends the
 * builder. A registry's orders change only here, so every sequence that
 * lists its facts in their order keeps it. */
void markdown_core_registry_build_order(markdown_core_registry_builder *builder);
/* Ends a builder after its end without ordering: its registry is not kept,
 * and the caller releases it. */
void markdown_core_registry_build_cancel(markdown_core_registry_builder *builder);

#ifdef __cplusplus
}
#endif

#endif
