#ifndef MARKDOWN_CORE_DIALECT_H
#define MARKDOWN_CORE_DIALECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "node.h"

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

/* The block-start hook families, in the order a line consults them.
 *
 * They are separate families rather than one list because the ORDER BETWEEN
 * them is grammar: every `scan` owner wins over every `open` owner on the same
 * line, and `interrupt` runs between the two so a dash-led table can take a
 * line a thematic break or list already matched. Merging them would change
 * which element claims an ambiguous line. */
typedef enum {
    MARKDOWN_CORE_BLOCK_HOOK_SCAN,
    MARKDOWN_CORE_BLOCK_HOOK_INTERRUPT,
    MARKDOWN_CORE_BLOCK_HOOK_OPEN,
    MARKDOWN_CORE_BLOCK_HOOK_PARAGRAPH,
    MARKDOWN_CORE_BLOCK_HOOK_PROBE,
    MARKDOWN_CORE_BLOCK_HOOK_COUNT
} markdown_core_block_hook;

/* THE INLINE-CONTENT HOOK FAMILIES, projected for the same reason and with one
 * difference: a block hook family is asked per LINE and can be gated on the
 * line's first byte, while these three are asked once per inline-content NODE
 * and have nothing to gate on. So they get the projection and not the gate
 * maps.
 *
 * Each of the three was a scan of every attached element looking for the few
 * that declare the hook, run per inline-content node: `init` from
 * `markdown_core_inline_state_from_buf`, `finish` from
 * `markdown_core_inline_finish_inlines`, `dispose` from
 * `markdown_core_inline_clear_inlines`. With thirty core elements and one
 * declarer for `init` and for `finish`, that is ninety iterations per node to
 * find six calls.
 *
 * Order inside a family is descriptor order, which is what the scan gave, so a
 * projected family calls the same hooks on the same states in the same
 * sequence. */
typedef enum {
    MARKDOWN_CORE_INLINE_HOOK_INIT,
    MARKDOWN_CORE_INLINE_HOOK_FINISH,
    MARKDOWN_CORE_INLINE_HOOK_DISPOSE,
    MARKDOWN_CORE_INLINE_HOOK_COUNT
} markdown_core_inline_hook;

/* THE FINISH STEPS, projected by EVENT and KIND.
 *
 * The finish walk delivers two events per node, and a step declares the kinds
 * it is ASKED AT (their EXIT, once the subtree is complete) and the kinds
 * whose EXTENT it tracks (their ENTER and EXIT), so the natural key of the
 * dispatch is (event, kind): a Text's EXIT reaches autolink, a Paragraph's
 * EXIT reaches formula, a Link's ENTER and EXIT reach autolink, and a Text's
 * ENTER or a List's EXIT reach nothing. The projection is one table with a
 * pointer per key to a terminated list of steps in descriptor order, NULL for
 * a key nothing declared, built once when the dialect is sealed, beside the
 * block and inline-content families, and gated, once the tree is complete, on the kinds
 * the parse produced (element.h, `finish_acts_on_kinds`). One load decides
 * the common case.
 *
 * Kinds are indexed by class then ordinal, so a block and an inline kind that
 * collide once masked keep separate keys. A kind outside the table -- an
 * extension kind numbered at or past MARKDOWN_CORE_NODE_KIND_COUNT -- shares
 * one key, and registration refuses a step declared at such a kind, so that
 * key is never written and such a node's events dispatch to nothing. */
#define MARKDOWN_CORE_FINISH_KIND_COUNT (2 * MARKDOWN_CORE_NODE_KIND_COUNT)
#define MARKDOWN_CORE_FINISH_KEY_COUNT (2 * (MARKDOWN_CORE_FINISH_KIND_COUNT + 1))

static inline size_t markdown_core_finish_kind_index(markdown_core_node_type kind) {
    size_t ordinal = (size_t)kind & MARKDOWN_CORE_NODE_VALUE_MASK;
    if (ordinal >= MARKDOWN_CORE_NODE_KIND_COUNT) {
        return MARKDOWN_CORE_FINISH_KIND_COUNT;
    }
    return MARKDOWN_CORE_NODE_TYPE_INLINE_P(kind) ? MARKDOWN_CORE_NODE_KIND_COUNT + ordinal : ordinal;
}

static inline size_t markdown_core_finish_key(markdown_core_event_type event, markdown_core_node_type kind) {
    return 2 * markdown_core_finish_kind_index(kind) + (event == MARKDOWN_CORE_EVENT_EXIT);
}

/* The kind index of the one kind the engine's own step, text consolidation,
 * acts at. A constant, so the walk compares the index it computed anyway. */
#define MARKDOWN_CORE_FINISH_TEXT_INDEX                                                                                \
    (MARKDOWN_CORE_NODE_KIND_COUNT + ((size_t)MARKDOWN_CORE_NODE_TEXT & MARKDOWN_CORE_NODE_VALUE_MASK))

/* One projected step: the element, and which of the walk's per-root state
 * words is its own. An element that declared several kinds appears under each
 * of them with the same slot, so its state is one fact per root. A list ends
 * at an entry whose element is NULL.
 *
 * THE GATE IS READ AT THE EVENT. A step that declares the kinds it acts on is
 * asked at an EXIT of a kind it declared it is asked at only once the parse
 * has produced one of the kinds it acts on -- the same fact a pass is gated
 * on, read when the event comes rather than before the walk, because the walk
 * parses inline content as it goes and a kind's first node may be made after
 * the walk began. `acts_on` is that declaration as a set, and `gated` says
 * whether the test is worth making: it is false for an entry at a kind the
 * step acts on (a node of that kind is being exited, so the parse produced
 * one), for a step that declared nothing, and for a scope-kind entry (the
 * ENTER and EXIT that bound an extent are delivered whenever the extent is
 * walked, so the state the step keeps for the extent is always in step). */
typedef struct markdown_core_finish_step_entry {
    const markdown_core_element *element;
    size_t slot;
    markdown_core_node_kind_set acts_on;
    bool gated;
} markdown_core_finish_step_entry;

/* WHAT THE FINISH WALK ASKS OF A KIND, answered once per dialect per kind and
 * read as one record per event (see walk_owned_trees in blocks.c), so that
 * the walk's common path reads no descriptor. Each flag is a fact of the
 * kind's structure element: PARSES, the kind may hold inline content the walk
 * parses at its ENTER (it declares `inline_content`, or a
 * `contains_inlines_func` the walk then asks about the node); DEFERRED, the
 * content was parsed before the walk (a heading's, by the document's
 * preparation); FIELDS, the kind can own a field root through its own record
 * (`markdown_core_kind_owns_fields`, element.h -- a subtree an element owns
 * is found through the node's `element`, which the walk tests beside this).
 * `complete` is the element's `complete_inline`, NULL for a kind whose
 * element declares none. The out-of-table index answers nothing. */
enum {
    MARKDOWN_CORE_FINISH_KIND_PARSES = 1u << 0,
    MARKDOWN_CORE_FINISH_KIND_DEFERRED = 1u << 1,
    MARKDOWN_CORE_FINISH_KIND_FIELDS = 1u << 2
};
typedef struct markdown_core_finish_kind {
    void (*complete)(struct markdown_core_parser *, markdown_core_node *, int);
    uint8_t flags;
} markdown_core_finish_kind;

/* The keys a block-start family's gate lists are indexed by: each first
 * non-space byte, a line with no such byte, and an indented line (see
 * S_gate_candidates in blocks.c). */
#define MARKDOWN_CORE_BLOCK_GATE_KEY_NONE 256
#define MARKDOWN_CORE_BLOCK_GATE_KEY_INDENTED 257
#define MARKDOWN_CORE_BLOCK_GATE_KEYS 258

/* The dialect as setup extends it. The core dialect is borrowed; the first
 * attachment copies it into `element_allocation`, which the builder owns
 * until it is disposed. */
struct markdown_core_dialect_builder {
    const markdown_core_element *const *elements;
    const markdown_core_element **element_allocation;
    size_t element_count, element_capacity;
};

/* A sealed dialect. Every list it points to, its element list included,
 * lives in the storage it was sealed into, after the struct, so it owns
 * nothing apart from that storage; readers see `const` views only.
 *
 * `document_structure` owns the document lifecycle -- the last registered
 * element that declares it, as `text_structure` is the last that declares
 * `parse_text` and each delimiter rule belongs to the last element that
 * declares it. The core dialect has one of each; a setup that registers
 * another replaces the earlier one for its instance. */
typedef struct markdown_core_dialect {
    const markdown_core_element *const *elements;
    size_t element_count;
    const markdown_core_element *document_structure, *text_structure;
    const markdown_core_element *delimiter_owners[MARKDOWN_CORE_DELIM_RULE_COUNT];
    markdown_core_delimiter_rule delimiter_chars[256];
    /* Each block-start hook family, in descriptor order. Order inside a
     * family IS the grammar: the first owner that claims a line wins it,
     * which is why heading precedes thematic break (setext `---`) and
     * thematic break precedes list (`***`). A line asks four of these
     * families in turn, so without the projection it would pay the whole
     * dialect four times to reach the one to four owners that can answer. */
    const markdown_core_element *const *block_hooks[MARKDOWN_CORE_BLOCK_HOOK_COUNT];
    size_t block_hook_counts[MARKDOWN_CORE_BLOCK_HOOK_COUNT];
    /* Each family's declared gates as one list of owners per key, in the
     * family's own order: a count, then owner indices. NULL when the family
     * declared no gate and every owner is asked, the behaviour a gate
     * replaces. */
    const uint8_t *block_gate_lists[MARKDOWN_CORE_BLOCK_HOOK_COUNT];
    /* Every byte a container continuation may strip ahead of a line's own
     * first byte: indentation, and each element's `container_prefix_bytes`.
     * A question asked of a later line from raw source walks these to land on
     * the byte the stripped line would show first (see
     * definition_next_lines_admit). */
    bool container_prefix[256];
    /* The inline-content families, in descriptor order. */
    const markdown_core_element *const *inline_hooks[MARKDOWN_CORE_INLINE_HOOK_COUNT];
    size_t inline_hook_counts[MARKDOWN_CORE_INLINE_HOOK_COUNT];
    /* Each byte's inline owners, by precedence and then descriptor order:
     * `inline_dispatch[inline_dispatch_offsets[c] .. inline_dispatch_offsets[c + 1])`. */
    size_t inline_dispatch_offsets[257];
    const markdown_core_element *const *inline_dispatch;
    /* The inline byte tables: the text terminators, flanking-transparent
     * bytes and start predicates of the registered inline elements. */
    bool (*inline_start_predicates[256])(markdown_core_inline_state *, bufsize_t);
    int8_t special_chars[256];
    int8_t skip_chars[256];
    /* The finish steps by key (see `markdown_core_finish_key`): a list
     * terminated by a NULL element, or NULL when nothing declared the key.
     * `finish_step_slots` is how many state words the walk keeps per root:
     * one per element that declares a step. */
    const markdown_core_finish_step_entry *finish_dispatch[MARKDOWN_CORE_FINISH_KEY_COUNT];
    size_t finish_step_slots;
    markdown_core_finish_kind finish_kinds[MARKDOWN_CORE_FINISH_KIND_COUNT + 1];
} markdown_core_dialect;

/* What sealing a builder takes, counted from its element list alone: how
 * many element pointers, finish step entries and gate-table bytes follow the
 * struct, and the counts that place each projection among them. */
typedef struct markdown_core_dialect_layout {
    size_t block_totals[MARKDOWN_CORE_BLOCK_HOOK_COUNT];
    bool gated[MARKDOWN_CORE_BLOCK_HOOK_COUNT];
    size_t inline_totals[MARKDOWN_CORE_INLINE_HOOK_COUNT];
    size_t inline_dispatch_offsets[257];
    size_t finish_key_counts[MARKDOWN_CORE_FINISH_KEY_COUNT];
    size_t pointers, steps, gate_bytes;
} markdown_core_dialect_layout;

/* Begin a builder from the `count` elements of `elements`, which it borrows. */
void markdown_core_dialect_builder_init(markdown_core_dialect_builder *builder,
                                        const markdown_core_element *const *elements, size_t count);

/* Measure what sealing `builder` takes into `layout`. Returns the bytes that
 * must follow the struct in the storage it is sealed into. */
size_t markdown_core_dialect_measure(const markdown_core_dialect_builder *builder,
                                     markdown_core_dialect_layout *layout);

/* Seal `builder`, as `layout` measured it, into `dialect`: storage aligned
 * for the struct and followed by the measured bytes. Sealing defines every
 * one of those bytes, so the storage need not be initialized and all the work
 * of establishing the dialect happens inside this call. It cannot fail and
 * copies what it keeps, so the builder is unchanged and still owns what it
 * did. */
void markdown_core_dialect_seal(const markdown_core_dialect_builder *builder,
                                const markdown_core_dialect_layout *layout, markdown_core_dialect *dialect);

/* Release whatever the builder owns. */
void markdown_core_dialect_builder_dispose(markdown_core_dialect_builder *builder);

#ifdef __cplusplus
}
#endif

#endif
