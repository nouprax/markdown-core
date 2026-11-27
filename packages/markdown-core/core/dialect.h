#ifndef MARKDOWN_CORE_DIALECT_H
#define MARKDOWN_CORE_DIALECT_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "node.h"
#include "element.h"
#include "follow.h"

#ifdef __cplusplus
extern "C" {
#endif

struct markdown_core_parser;

/* THE DIALECT OF ONE PARSER INSTANCE: ITS ELEMENTS AND WHAT IS PROJECTED FROM THEM.
 *
 * Each construct's grammar is its element; the engine writes none of its
 * own. What an instance parses with is the list of elements, in order, and
 * the tables projected from their descriptors so that a line or a byte
 * reaches its owners without asking every element. All of it is a function
 * of the element list alone.
 *
 * A dialect has two states with two types. Setup sees a BUILDER: it starts
 * from the core dialect, and `markdown_core_dialect_builder_attach` extends
 * it under the registration rule. The transaction then SEALS the builder into
 * a `markdown_core_dialect`, validating nothing further and projecting every
 * table once. A parser holds only a `const` pointer to the sealed dialect:
 * nothing reachable from an instance can register an element or write a
 * projection, so "the dialect is fixed for the instance's lifetime" is a fact
 * of the types rather than a convention. Instances in one process may seal
 * different dialects; each owns its own.
 *
 * Sealing is two steps, MEASURE and SEAL, because the sealed dialect does not
 * choose its own storage: it begins and ends with its instance, so it lives
 * in the instance's allocation (blocks.c, markdown_core_instance), and the
 * size of that allocation is known only once the dialect is measured.
 */

/* THE INLINE-CONTENT HOOK FAMILIES, projected for the same reason: the
 * elements whose run records begin (`init`) and are let go (`dispose`) with
 * each inline run, in descriptor order, so a run calls the few that declare
 * them and scans no others. */
typedef enum {
    MARKDOWN_CORE_INLINE_HOOK_INIT,
    MARKDOWN_CORE_INLINE_HOOK_DISPOSE,
    MARKDOWN_CORE_INLINE_HOOK_COUNT
} markdown_core_inline_hook;

/* THE FINISH STEPS, projected by EVENT and KIND.
 *
 * Completion's walk delivers two events per node, and a step declares the
 * kinds it is ASKED AT (their EXIT, once the subtree is complete) and the
 * inline kinds whose EXTENT it tracks (their ENTER and EXIT), so the natural
 * key of the dispatch is (event, kind): a Text's EXIT reaches autolink, a
 * CodeBlock's EXIT reaches formula, a Link's ENTER and EXIT reach autolink,
 * and a Text's ENTER or a Paragraph's EXIT reach nothing. The projection is one table with a pointer
 * per key to a terminated list of steps in descriptor order, NULL for a key
 * nothing declared, built once when the dialect is sealed, beside the block
 * and inline-content families, and gated at the event on the kinds the parse
 * has produced (element.h, markdown_core_finish_ops, `acts`). One load decides the
 * common case.
 *
 * Kinds are indexed by class then ordinal, so a block and an inline kind that
 * collide once masked keep separate keys. A kind outside the table -- an
 * extension kind numbered at or past MARKDOWN_CORE_NODE_KIND_COUNT -- shares
 * one key, and registration refuses a step declared at such a kind, so that
 * key is never written and such a node's events dispatch to nothing. */
#define MARKDOWN_CORE_FINISH_KIND_COUNT (2 * MARKDOWN_CORE_NODE_KIND_COUNT)
#define MARKDOWN_CORE_FINISH_KEY_COUNT (2 * (MARKDOWN_CORE_FINISH_KIND_COUNT + 1))

/* The kind index of a kind of no inline class, as of a block: its ordinal,
 * or the out-of-table index. The block parser reads the records of the blocks
 * it holds by it, their class known. */
static inline size_t markdown_core_block_kind_index(markdown_core_node_type kind) {
    const size_t ordinal = (size_t)kind & MARKDOWN_CORE_NODE_VALUE_MASK;
    return ordinal < MARKDOWN_CORE_NODE_KIND_COUNT ? ordinal : MARKDOWN_CORE_FINISH_KIND_COUNT;
}

/* The kind index of any kind: an inline kind's follows the block kinds'. */
static inline size_t markdown_core_finish_kind_index(markdown_core_node_type kind) {
    const size_t index = markdown_core_block_kind_index(kind);
    return MARKDOWN_CORE_NODE_TYPE_INLINE_P(kind) && index < MARKDOWN_CORE_NODE_KIND_COUNT
               ? MARKDOWN_CORE_NODE_KIND_COUNT + index
               : index;
}

static inline size_t markdown_core_finish_key(markdown_core_event_type event, markdown_core_node_type kind) {
    return 2 * markdown_core_finish_kind_index(kind) + (event == MARKDOWN_CORE_EVENT_EXIT);
}

/* One projected step: the element, and which of the walk's per-root state
 * words is its own. An element that declared several kinds appears under each
 * of them with the same slot, so its state is one fact per root. A list ends
 * at an entry whose element is NULL.
 *
 * THE GATE IS READ AT THE EVENT. A step that declares the kinds it acts on is
 * asked at an EXIT of a kind it declared it is asked at only once the parse
 * has produced one of the kinds it acts on, read when the event comes,
 * because completion parses each block's inline content as it reaches the
 * block, and a kind's first node may be made after completion began.
 * `acts_on` is that declaration as a set, and `gated` says whether the test
 * is worth making: it is false for an entry at a kind the step acts on (a
 * node of that kind is being exited, so the parse produced one), for a step
 * that declared nothing, and for a scope-kind entry (the ENTER and EXIT that
 * bound an extent are delivered whenever the extent is walked, so the state
 * the step keeps for the extent is always in step). */
typedef struct markdown_core_finish_step_entry {
    const markdown_core_element_instance *instance;
    size_t slot;
    markdown_core_node_kind_set acts_on;
    bool gated;
} markdown_core_finish_step_entry;

/* WHAT THE ENGINE ASKS OF A KIND, answered once per dialect per kind so that
 * no hot path reads a descriptor to learn it. `structure` is the instance of
 * the kind's structure element (element.h, `markdown_core_structure_for_kind`)
 * -- the `self` every structure hook the engine calls on a node of the kind
 * is handed -- or NULL for a kind with none or whose structure element the
 * dialect does not hold, which then has no flag but FIELDS. Each flag is a
 * fact of that element's descriptor (element.h), read with the record by the
 * line engine and completion alike. A fact a hook answers per node is a
 * flag saying the element declares the hook, which is then asked:
 *
 * - INLINES / INLINES_ASK: the content's `inlines` / `holds_inlines` -- the
 *   kind may hold inline content (PARSES, either one), which the walk parses
 *   at its ENTER; DEFERRED, the element's `deferred`, it was parsed before the walk
 *   (a heading's, by the document's preparation);
 * - LINES / LINES_ASK: the kind takes lines as content, a LITERAL
 *   `mode` / `holds_lines` (markdown_core_content_ops); PROSE, a PROSE
 *   `mode`, it takes a text line as prose; IS_PARAGRAPH, the content's
 *   `paragraph`;
 * - BLANK_OPAQUE, BLANK_ASK (the lines' `blank_last`), BLANK_RUNS and BLANK_PROPAGATES
 *   (the lines' `blank_spreads`): what a blank line means inside it;
 * - FOLDS: the kind is one of its fold's `kinds`, and its `child` is asked as a
 *   child settles into a block of it;
 * - FIELDS: the kind's record visits field roots (markdown_core_record_ops,
 *   `visit`), and the kind is not the root, whose record holds the
 *   document's state -- a subtree an element owns is found through the
 *   node's `element`, which the walk tests beside this.
 *
 * `complete` is the structure's `complete`, NULL when it declares
 * none, which the walk calls at every ENTER. The out-of-table index answers
 * nothing. */
enum {
    MARKDOWN_CORE_KIND_INLINES = 1u << 0,
    MARKDOWN_CORE_KIND_INLINES_ASK = 1u << 1,
    MARKDOWN_CORE_KIND_PARSES = MARKDOWN_CORE_KIND_INLINES | MARKDOWN_CORE_KIND_INLINES_ASK,
    MARKDOWN_CORE_KIND_DEFERRED = 1u << 2,
    MARKDOWN_CORE_KIND_FIELDS = 1u << 3,
    MARKDOWN_CORE_KIND_LINES = 1u << 4,
    MARKDOWN_CORE_KIND_LINES_ASK = 1u << 5,
    MARKDOWN_CORE_KIND_PROSE = 1u << 6,
    MARKDOWN_CORE_KIND_IS_PARAGRAPH = 1u << 7,
    MARKDOWN_CORE_KIND_BLANK_OPAQUE = 1u << 8,
    MARKDOWN_CORE_KIND_BLANK_ASK = 1u << 9,
    MARKDOWN_CORE_KIND_BLANK_RUNS = 1u << 10,
    MARKDOWN_CORE_KIND_BLANK_PROPAGATES = 1u << 11,
    MARKDOWN_CORE_KIND_FOLDS = 1u << 12,
    /* A later line may write a block of the kind once it closed
     * (element.h, markdown_core_follow, `closed`). */
    MARKDOWN_CORE_KIND_TRAILED = 1u << 13,
    /* A block after one of the kind may still go on in it (element.h,
     * `awaits`). */
    MARKDOWN_CORE_KIND_AWAITS = 1u << 14
};
typedef struct markdown_core_kind_record {
    const markdown_core_element_instance *structure;
    void (*complete)(const markdown_core_element_instance *, struct markdown_core_parser *, markdown_core_node *, int);
    uint16_t flags;
    /* The element that follows a node of the kind at each time, with its
     * declaration (element.h, markdown_core_follow), or NULL; at a line,
     * the one that follows it once it closed (`closed`). */
    const markdown_core_element_instance *follower[MARKDOWN_CORE_FOLLOW_TIMES];
    const struct markdown_core_follow *follow[MARKDOWN_CORE_FOLLOW_TIMES];
    /* The follower that makes a node of the kind of a leader of another, and
     * its declaration (`makes`, THE FORMER KIND), or NULL. */
    const markdown_core_element_instance *maker;
    const struct markdown_core_follow *made;
} markdown_core_kind_record;

/* The keys the starts are listed by (`starts`): each first non-space
 * byte, a line with no such byte, and an indented line (blocks.c,
 * S_gate_key). */
#define MARKDOWN_CORE_BLOCK_GATE_KEY_NONE 256
#define MARKDOWN_CORE_BLOCK_GATE_KEY_INDENTED 257
#define MARKDOWN_CORE_BLOCK_GATE_KEYS 258

/* The alignment every element record begins at: that of the strictest type a
 * record may hold. C99 has no `max_align_t`; the size of a union of the
 * candidates is a multiple of each one's alignment. */
typedef union {
    long double number;
    void *pointer;
    uint64_t integer;
} markdown_core_state_alignment;
#define MARKDOWN_CORE_STATE_ALIGN sizeof(markdown_core_state_alignment)

static inline size_t markdown_core_state_align(size_t size) {
    return (size + MARKDOWN_CORE_STATE_ALIGN - 1) / MARKDOWN_CORE_STATE_ALIGN * MARKDOWN_CORE_STATE_ALIGN;
}

/* THE INSTANCES BY DESCRIPTOR: an open-addressed table of the dialect's
 * instances keyed by their descriptor's address, at least twice as large as
 * the elements it holds, so a lookup ends at the element or at an empty entry.
 * A table of 1 << `bits` entries takes the top `bits` bits of the address
 * times 2^64 / phi with its high half folded into its low, times 2^64 / phi
 * again. Descriptors lie at a fixed stride, and the top bits of one product
 * step by the stride over phi: a stride whose quotient lies near a whole
 * number puts neighbouring descriptors in neighbouring entries. The fold
 * lets every bit of the first product move the second, so descriptors spread
 * over the table at whatever stride they are laid out. It answers
 * markdown_core_parser_instance -- one element reading another's state. The
 * engine and an element's own hooks never search it: the projections hold
 * instances. */
static inline size_t markdown_core_instance_hash(const markdown_core_element *element, unsigned bits) {
    uint64_t product = (uint64_t)(uintptr_t)element * UINT64_C(0x9E3779B97F4A7C15);
    product ^= product >> 32;
    return (size_t)((product * UINT64_C(0x9E3779B97F4A7C15)) >> (64 - bits));
}

/* The dialect as setup extends it. The core dialect is borrowed; the first
 * attachment copies it into `element_allocation`, which the builder owns
 * until it is disposed. */
struct markdown_core_dialect_builder {
    const markdown_core_element *const *elements;
    const markdown_core_element **element_allocation;
    size_t element_count, element_capacity;
};

/* A sealed dialect. Every list it points to, its instances included,
 * lives in the storage it was sealed into, after the struct, so it owns
 * nothing apart from that storage; readers see `const` views only.
 *
 * `document_structure` owns the document lifecycle -- the last registered
 * element that declares it, as `text_structure` is the last that declares
 * how the core's data compiles (`data`) and `text_block_structure` the last
 * that declares an opening's `content`. The core dialect has one of each; a setup that registers
 * another replaces the earlier one for its instance. */

typedef struct markdown_core_dialect {
    /* The elements, as one instance each in element order (markdown-core-
     * element-api.h, "AN ELEMENT AS ONE PARSE HOLDS IT"). Every projection
     * below names instances, so a hook is dispatched with its `self` in hand. */
    const markdown_core_element_instance *instances;
    size_t element_count;
    const markdown_core_element_instance *document_structure, *text_structure, *text_block_structure;
    /* The ROLE KINDS: the kind of the root, as `document_structure`
     * declares it, and of plain text, as `text_structure` declares it --
     * the kinds the engine acts on by role (text consolidation, the literal
     * runs, the root it makes) without naming them. `plain_index` is the
     * plain kind's finish index, the one kind index the engine's own step,
     * text consolidation, acts at. */
    markdown_core_node_type root, plain;
    size_t plain_index;
    /* THE STARTS (element.h, markdown_core_block_construct): the elements'
     * block constructs, each listed under every key it may succeed at --
     * a first non-space byte, a blank line (KEY_NONE), an indented line
     * (KEY_INDENTED) -- in the order they are tried: starts[start_offsets[k]
     * .. start_offsets[k + 1]) for key k. */
    const markdown_core_block_construct *const *starts;
    uint16_t start_offsets[MARKDOWN_CORE_BLOCK_GATE_KEYS + 1];
    /* The elements that cut items off the front of a block's text
     * (element.h, markdown_core_open_ops `item`), in descriptor order. */
    const markdown_core_element_instance *const *cutters;
    size_t cutter_count;
    /* Every byte a container continuation may strip ahead of a line's own
     * first byte: indentation, and each element's lines' `prefix_bytes`.
     * A question asked of a later line from raw source walks these to land on
     * the byte the stripped line would show first (see
     * definition_next_lines_admit). */
    bool container_prefix[256];
    /* Every first byte a cut owner reads an item from (element.h, its
     * `item`'s `bytes`), one bit each: a block whose text starts with no such
     * byte keeps its front. */
    uint8_t fronts[32];
    /* The inline-content families, in descriptor order. */
    const markdown_core_element_instance *const *inline_hooks[MARKDOWN_CORE_INLINE_HOOK_COUNT];
    size_t inline_hook_counts[MARKDOWN_CORE_INLINE_HOOK_COUNT];
    /* THE TEXT (plan symbols-cleanroom.md 23): the elements' constructs by
     * the byte they start at, and compile's handlers by token kind,
     * `token_kinds` of them, the core's kinds without any. */
    markdown_core_constructs text;
    uint16_t text_offsets[257];
    const markdown_core_compile_ops *const *compiles;
    size_t token_kinds;
    /* The flanking-transparent bytes of the registered elements
     * (element.h, markdown_core_inline_ops `transparent`). */
    int8_t skip_chars[256];
    /* The finish steps by key (see `markdown_core_finish_key`): a list
     * terminated by a NULL element, or NULL when nothing declared the key.
     * `finish_step_slots` is how many state words the walk keeps per root:
     * one per element that declares a step. */
    const markdown_core_finish_step_entry *finish_dispatch[MARKDOWN_CORE_FINISH_KEY_COUNT];
    size_t finish_step_slots;
    /* Each kind's record, by kind index (markdown_core_finish_kind_index). */
    markdown_core_kind_record kinds[MARKDOWN_CORE_FINISH_KIND_COUNT + 1];
    /* The instances by descriptor, 1 << `instance_bits` entries
     * (markdown_core_instance_hash), and the bytes of one inline run's block
     * of records. */
    const markdown_core_element_instance *const *instance_table;
    unsigned instance_bits;
    size_t run_state_size;
    /* The dialect's rules for closing a label (element.h,
     * markdown_core_close_rule): first the `shared` rules every label no
     * `mine` rule claims meets, by precedence and then descriptor order,
     * then each element's `mine` rules, in its range (the instance's
     * `mine_first`, `mine_count`), by precedence. */
    const struct markdown_core_closer *closers;
    size_t closer_count, shared;
    /* THE LABEL SPACES the elements name (element.h, markdown_core_space),
     * by number: `space_count` of them, the first two NULL, no
     * kind's and the engine's ANCHOR. */
    const markdown_core_space *const *spaces;
    size_t space_count;
    /* THE FACETS of the kinds the elements show (element.h,
     * markdown_core_facet), by public kind: the last registered element's
     * that declares one, NULL for a kind none does. */
    const markdown_core_facet *facets[MARKDOWN_CORE_FACET_ROOM];
    /* How names are spelled (element.h, `spelling`), or NULL. */
    const struct markdown_core_name_spelling *spelling;
    /* The instances whose elements ask the names service for names
     * (element.h, `names`), in element order, ending at NULL. */
    const markdown_core_element_instance *const *namers;
    /* The instances whose elements take part in the parse's stages
     * (element.h, `stages`), in element order, ending at NULL. */
    const markdown_core_element_instance *const *staged;
} markdown_core_dialect;

/* A rule for closing a label and the instance whose rule it is. */
typedef struct markdown_core_closer {
    const markdown_core_element_instance *owner;
    const struct markdown_core_close_rule *rule;
    /* The rule's `tail` with the owner as its context, its `start` NULL
     * when the rule has none. */
    markdown_core_construct tail;
    unsigned char byte;
} markdown_core_closer;

/* The record of `kind`, and the instance of the structure element of a node's
 * kind -- NULL for no node, as for a kind with none. */
static inline const markdown_core_kind_record *markdown_core_dialect_kind(const markdown_core_dialect *dialect,
                                                                          markdown_core_node_type kind) {
    return &dialect->kinds[markdown_core_finish_kind_index(kind)];
}

/* The same of a kind of no inline class (markdown_core_block_kind_index). */
static inline const markdown_core_kind_record *markdown_core_dialect_block_kind(const markdown_core_dialect *dialect,
                                                                                markdown_core_node_type kind) {
    assert(!MARKDOWN_CORE_NODE_TYPE_INLINE_P(kind));
    return &dialect->kinds[markdown_core_block_kind_index(kind)];
}

static inline const markdown_core_element_instance *
markdown_core_dialect_structure(const markdown_core_dialect *dialect, const markdown_core_node *node) {
    return node ? markdown_core_dialect_kind(dialect, (markdown_core_node_type)node->kind)->structure : NULL;
}

/* The facet the dialect of `self` holds of public kind `kind`, or NULL: what
 * another element's node shows, asked without naming the element. */
static inline const markdown_core_facet *markdown_core_instance_facet(const markdown_core_element_instance *self,
                                                                      markdown_core_node_kind kind) {
    return (size_t)kind < MARKDOWN_CORE_FACET_ROOM ? self->dialect->facets[kind] : NULL;
}

/* The instance of `element` in `dialect`, or NULL when it holds none. Sealing
 * resolves kinds through it, and introspection asks it; parse-time
 * code never does, since every instance a hook reaches is already resolved. */
static inline const markdown_core_element_instance *
markdown_core_dialect_instance(const markdown_core_dialect *dialect, const markdown_core_element *element) {
    const size_t mask = ((size_t)1 << dialect->instance_bits) - 1;
    for (size_t at = markdown_core_instance_hash(element, dialect->instance_bits);; at = (at + 1) & mask) {
        const markdown_core_element_instance *instance = dialect->instance_table[at];
        if (!instance || instance->element == element) {
            return instance;
        }
    }
}

/* The number the dialect gave `space` (element.h,
 * markdown_core_space), or 0, no kind's, when no element of it names the
 * space: a space no label has, which every lookup answers with none. */
static inline unsigned markdown_core_dialect_space_kind(const markdown_core_dialect *dialect,
                                                        const markdown_core_space *space) {
    for (size_t kind = 0; kind < dialect->space_count; kind++) {
        if (dialect->spaces[kind] && strcmp(dialect->spaces[kind]->name, space->name) == 0) {
            return (unsigned)kind;
        }
    }
    return 0;
}

/* What sealing a builder takes, counted from its element list alone: how
 * many finish step entries, projected instance pointers, instance-table
 * entries, resolved needs and gate-table bytes follow the struct beside one instance per
 * element, and the counts that place each projection among them. */
typedef struct markdown_core_dialect_sizes {
    size_t cutters;
    size_t inline_totals[MARKDOWN_CORE_INLINE_HOOK_COUNT];
    size_t finish_key_counts[MARKDOWN_CORE_FINISH_KEY_COUNT];
    size_t pointers, steps;
    /* The instance table holds 1 << `instance_bits` entries. */
    unsigned instance_bits;
    /* The bytes the elements' parse records take, and one run's records. */
    size_t state_bytes, run_state_bytes;
    /* The services the elements need, all together, and their rules for
     * closing a label; the spaces they name, each once, and their
     * namings, all together. */
    size_t needs, closers, spaces, namings;
    /* The elements' constructs and their entries by byte, and their token
     * kinds. */
    size_t constructs, text_entries, token_kinds;
    /* The elements' block starts and their entries by key. */
    size_t starts, start_entries;
} markdown_core_dialect_sizes;

/* Begin a builder from the `count` elements of `elements`, which it borrows. */
void markdown_core_dialect_builder_init(markdown_core_dialect_builder *builder,
                                        const markdown_core_element *const *elements, size_t count);

/* Measure what sealing `builder` takes into `sizes`. Returns the bytes that
 * must follow the struct in the storage it is sealed into. */
size_t markdown_core_dialect_measure(const markdown_core_dialect_builder *builder, markdown_core_dialect_sizes *sizes);

/* Seal `builder`, as `sizes` measured it, into `dialect`: zeroed storage
 * aligned for the struct and followed by the measured bytes. `state` is the
 * zeroed, aligned storage of `sizes->state_bytes` the instances' parse
 * records are laid out in. It cannot fail and copies what it keeps, so the
 * builder is unchanged and still owns what it did. */
void markdown_core_dialect_seal(const markdown_core_dialect_builder *builder, const markdown_core_dialect_sizes *sizes,
                                markdown_core_dialect *dialect, unsigned char *state);

/* Release whatever the builder owns. */
void markdown_core_dialect_builder_dispose(markdown_core_dialect_builder *builder);

/* Every field root `node` holds (element.h, THE FIELD ROOTS OF A NODE): its
 * kind's record's, the root's excepted, then its element's. Completion,
 * which holds the kind's record, asks the two halves itself. */
static inline int markdown_core_visit_inline_subtrees(const markdown_core_dialect *dialect, markdown_core_node *node,
                                                      markdown_core_owned_subtree_visitor visitor, void *context) {
    const markdown_core_record_ops *record = markdown_core_record_for_kind((markdown_core_node_type)node->kind);
    return (!record->visit || node->kind == dialect->root || record->visit(node, visitor, context)) &&
           markdown_core_visit_element_subtrees(node, visitor, context);
}

#ifdef __cplusplus
}
#endif

#endif
