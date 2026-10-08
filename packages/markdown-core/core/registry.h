#ifndef MARKDOWN_CORE_REGISTRY_H
#define MARKDOWN_CORE_REGISTRY_H

#include <stdbool.h>
#include <stdint.h>

#include "buffer.h"
#include "chunk.h"

#ifdef __cplusplus
extern "C" {
#endif

struct markdown_core_node;

/* THE REGISTRY (docs/plans/2026-09-29-incremental-parsing.md, 5.7): what a
 * node declares to the whole document, and each question an inline root asks
 * of it, is a FACT. A fact belongs to the node that declares it and lives
 * exactly as long as that node: it is made when its node is made, a node a
 * parse takes keeps its facts, and a node that goes takes its facts with it.
 *
 * Facts are found through their KEY, a group and a label. A key holds its
 * declaring facts and its reverse index, the inline roots that asked it, hit
 * or miss. A key that gains or loses a fact is MARKED for the edit, and
 * remembers whether it was defined when it was first marked, so resolution
 * reads off which answers changed. The registry is the session's: it lives
 * in the node pool beside the slots of the nodes that hold its facts, and a
 * fresh parse fills an empty one. */
typedef enum {
    /* A reference label: Reference nodes and heading targets declare it. */
    MARKDOWN_CORE_KEY_REFERENCE,
    /* A footnote label. */
    MARKDOWN_CORE_KEY_FOOTNOTE,
    /* A specimen label. */
    MARKDOWN_CORE_KEY_SPECIMEN,
    /* An anchor family: the spellings with the same stem after stripping
     * trailing `-N` groups (markdown_core_anchor_stem). */
    MARKDOWN_CORE_KEY_FAMILY,
} markdown_core_key_group;

typedef enum {
    /* Defines its key: a Reference, a heading's target, a Footnote or a
     * Specimen definition. */
    MARKDOWN_CORE_FACT_DECLARE,
    /* Reserves its spelling in its family: an explicit anchor. */
    MARKDOWN_CORE_FACT_RESERVE,
    /* A heading's anchor base in its family: the heading takes the first
     * spelling of the base the family leaves free. */
    MARKDOWN_CORE_FACT_BASE,
    /* An inline root asked its key. */
    MARKDOWN_CORE_FACT_LOOKUP,
} markdown_core_fact_role;

typedef struct markdown_core_fact markdown_core_fact;
typedef struct markdown_core_key markdown_core_key;
typedef struct markdown_core_registry markdown_core_registry;

/* AN ORDER LABEL (docs/plans/2026-09-29-incremental-parsing.md, 5.7): where
 * a node that declares a fact, holds an inline root or holds blocks read
 * from its content lies in tree order. The registry keeps the orders of the
 * session's tree in one list in tree order, and each carries a label that
 * grows along it, so two compare in O(1) (Dietz and Sleator's order
 * maintenance): a new one gets a label between its neighbours', and when
 * none is free the labels of the fewest orders around it that leave room
 * are spread again. The orders a parse makes form a sequence in tree order,
 * linked by `next`, that joins the list as the document completes
 * (markdown_core_order_join); in it `before` names the first old order the
 * tree holds after it, or is NULL when it holds none. */
typedef struct markdown_core_order {
    uint64_t label;
    struct markdown_core_order *prev, *next, *before;
    struct markdown_core_node *node;
} markdown_core_order;

struct markdown_core_fact {
    markdown_core_key *key;
    struct markdown_core_node *node;
    /* The key's declaring facts, or its lookups, are a doubly linked list. */
    markdown_core_fact *prev, *next;
    /* The next fact its node holds. */
    markdown_core_fact *sibling;
    /* The parse that made it (markdown_core_registry_begin). */
    uint64_t edit;
    uint8_t role;
    /* A spelling, for a reservation or a base. */
    uint32_t length;
    unsigned char text[];
};

struct markdown_core_key {
    markdown_core_registry *registry;
    /* The next key of its bucket, and of the marked keys. */
    markdown_core_key *chain, *marked_next;
    uint64_t hash;
    markdown_core_fact *facts, *lookups;
    /* How many of its facts declare it. */
    size_t declared;
    /* The edit that marked it, 0 when none has, and whether it was defined
     * when that edit first marked it. */
    uint64_t marked;
    bool was;
    uint8_t group;
    uint32_t length;
    unsigned char label[];
};

struct markdown_core_registry {
    /* The head of the list of orders: its `next` is the first in tree
     * order, its `prev` the last. */
    markdown_core_order orders;
    markdown_core_key **buckets;
    size_t capacity, count;
    /* The keys this edit marked, last first. */
    markdown_core_key *marked;
    /* The edit being parsed: each parse is one more, from 1. A session parses
     * far fewer than 2^64 times, so 0 never comes again. */
    uint64_t edit;
    /* Where a label is normalized before it is asked or declared. */
    markdown_core_strbuf scratch;
};

/* A parse begins: the facts it makes are this edit's. */
void markdown_core_registry_begin(markdown_core_registry *registry);
/* Resolution is done: no key is marked, and a key no fact holds goes. */
void markdown_core_registry_settle(markdown_core_registry *registry);
/* Drops every key and every fact; the nodes that held them hold none. */
void markdown_core_registry_dispose(markdown_core_registry *registry);

/* `node` declares the key (`group`, `label`) with `role`, and `text`, a
 * spelling of `text_length` bytes for a reservation or a base. Its key is
 * marked. NULL when the storage could not be had. */
markdown_core_fact *markdown_core_registry_declare(markdown_core_registry *registry, struct markdown_core_node *node,
                                                   markdown_core_key_group group, markdown_core_fact_role role,
                                                   const unsigned char *label, uint32_t length,
                                                   const unsigned char *text, uint32_t text_length);
/* The inline root `holder` asks the key (`group`, `label`): whether a fact
 * defines it, with the key in `*key`. The question joins the key's reverse
 * index. False with `*key` NULL when the storage could not be had. */
bool markdown_core_registry_ask(markdown_core_registry *registry, struct markdown_core_node *holder,
                                markdown_core_key_group group, const unsigned char *label, uint32_t length,
                                const markdown_core_key **key, bool *failed);
/* `label` in its normal form (cmark's link label rule), read into the
 * registry's scratch: NULL when it normalizes to nothing or the scratch could
 * not grow, with `*failed` for the latter. */
const markdown_core_strbuf *markdown_core_registry_normalize(markdown_core_registry *registry,
                                                             const markdown_core_chunk *label, bool *failed);

/* Every fact `node` holds leaves its key, which is marked. */
void markdown_core_registry_unlink(struct markdown_core_node *node);
/* Every question the inline root `node` asked leaves its key's reverse
 * index; the key is marked. */
void markdown_core_registry_unask(struct markdown_core_node *node);
/* `to` takes the facts and the order of `from`, the node a parse made equal
 * to it or a copy of it, and drops its own. */
void markdown_core_registry_move(struct markdown_core_node *from, struct markdown_core_node *to);
/* The facts of every node of `root` that only the old tree holds leave: the
 * nodes the parse did not take. False when the walk could not allocate its
 * stack. */
bool markdown_core_registry_retire(struct markdown_core_node *root);

/* A new order for `node`, in no list; NULL when it could not be had. */
markdown_core_order *markdown_core_order_new(struct markdown_core_node *node);
/* `order` leaves its list, if it is in one, and goes. */
void markdown_core_order_free(markdown_core_order *order);
/* Whether `order` is in the registry's list. */
static inline bool markdown_core_order_joined(const markdown_core_order *order) { return order->prev != NULL; }
/* The sequence of new orders from `first` joins the list: each right
 * before its `before`, and one with none at the end. */
void markdown_core_order_join(markdown_core_registry *registry, markdown_core_order *first);

/* Whether the key is defined: a fact declares it. */
static inline bool markdown_core_key_defined(const markdown_core_key *key) { return key->declared > 0; }

/* The stem of an anchor spelling: what is left after stripping its trailing
 * `-N` groups. */
markdown_core_chunk markdown_core_anchor_stem(const unsigned char *spelling, uint32_t length);

#ifdef __cplusplus
}
#endif

#endif
