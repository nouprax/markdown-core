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
 * line engine and the finish walk alike. A fact a hook answers per node is a
 * flag saying the element declares the hook, which is then asked:
 *
 * - INLINES / INLINES_ASK: `inline_content` / `contains_inlines_func` -- the
 *   kind may hold inline content (PARSES, either one), which the walk parses
 *   at its ENTER; DEFERRED, `deferred_inlines`, it was parsed before the walk
 *   (a heading's, by the document's preparation);
 * - LINES / LINES_ASK: the kind takes lines as content, a LITERAL
 *   `content_mode` / `accepts_lines_func`; PROSE, a PROSE `content_mode`, it
 *   takes a text line as prose; IS_PARAGRAPH, `paragraph`;
 * - BLANK_OPAQUE, BLANK_ASK (`blank_line`), BLANK_RUNS and BLANK_PROPAGATES
 *   (`propagates_child_blank`): what a blank line means inside it;
 * - REOPENS: the kind is one of its structure's `reopen_kinds`;
 * - FIELDS: the kind can own a field root through its own record
 *   (`markdown_core_kind_owns_fields`, element.h -- a subtree an element owns
 *   is found through the node's `element`, which the walk tests beside this).
 *
 * `complete` is the structure's `complete_inline`, NULL when it declares
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
    MARKDOWN_CORE_KIND_REOPENS = 1u << 12
};
typedef struct markdown_core_kind_record {
    const markdown_core_element_instance *structure;
    void (*complete)(const markdown_core_element_instance *, struct markdown_core_parser *, markdown_core_node *, int);
    uint16_t flags;
} markdown_core_kind_record;

/* The keys a block-start family's gate lists are indexed by: each first
 * non-space byte, a line with no such byte, and an indented line (see
 * S_gate_candidates in blocks.c). */
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
 * It answers markdown_core_parser_instance -- one element reading another's
 * state. The engine and an element's own hooks never search it: the
 * projections hold instances. */
static inline size_t markdown_core_instance_hash(const markdown_core_element *element, size_t mask) {
    return (size_t)(((uint64_t)(uintptr_t)element * UINT64_C(0x9E3779B97F4A7C15)) >> 32) & mask;
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
 * `parse_text`, `text_block_structure` the last that declares
 * `open_text_block` and each delimiter rule belongs to the last element that
 * declares it. The core dialect has one of each; a setup that registers
 * another replaces the earlier one for its instance. */
/* The class of a byte that ends a run of inline text, in the dialect's
 * `special_chars` table. */
enum { MARKDOWN_CORE_TEXT_END = 1 };

typedef struct markdown_core_dialect {
    /* The elements, as one instance each in element order (markdown-core-
     * element-api.h, "AN ELEMENT AS ONE PARSE HOLDS IT"). Every projection
     * below names instances, so a hook is dispatched with its `self` in hand. */
    const markdown_core_element_instance *instances;
    size_t element_count;
    const markdown_core_element_instance *document_structure, *text_structure, *text_block_structure;
    const markdown_core_element_instance *delimiter_owners[MARKDOWN_CORE_DELIM_RULE_COUNT];
    markdown_core_delimiter_rule delimiter_chars[256];
    /* Each block-start hook family, in descriptor order. Order inside a
     * family IS the grammar: the first owner that claims a line wins it,
     * which is why heading precedes thematic break (setext `---`) and
     * thematic break precedes list (`***`). A line asks four of these
     * families in turn, so without the projection it would pay the whole
     * dialect four times to reach the one to four owners that can answer. */
    const markdown_core_element_instance *const *block_hooks[MARKDOWN_CORE_BLOCK_HOOK_COUNT];
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
    const markdown_core_element_instance *const *inline_hooks[MARKDOWN_CORE_INLINE_HOOK_COUNT];
    size_t inline_hook_counts[MARKDOWN_CORE_INLINE_HOOK_COUNT];
    /* The elements that say whether a later line may write a closed block
     * (`writes_below`, E2), in descriptor order: a checkpoint asks them of
     * the block below its spine. */
    const markdown_core_element_instance *const *closed_writers;
    size_t closed_writer_count;
    /* Whether any element declares a postprocess pass: a pass reads the
     * whole finished tree, so its parse restarts at the document's start. */
    bool passes_declared;
    /* Each byte's inline owners, by precedence and then descriptor order:
     * `inline_dispatch[inline_dispatch_offsets[c] .. inline_dispatch_offsets[c + 1])`. */
    size_t inline_dispatch_offsets[257];
    const markdown_core_element_instance *const *inline_dispatch;
    /* The inline byte tables: the text terminators, flanking-transparent
     * bytes and the instance whose start predicate a byte asks, of the
     * registered inline elements. A text terminator has the class
     * MARKDOWN_CORE_TEXT_END, which the text scan stops at
     * (markdown_core_scan_to_class). */
    const markdown_core_element_instance *inline_start_owners[256];
    uint8_t special_chars[256];
    int8_t skip_chars[256];
    /* The finish steps by key (see `markdown_core_finish_key`): a list
     * terminated by a NULL element, or NULL when nothing declared the key.
     * `finish_step_slots` is how many state words the walk keeps per root:
     * one per element that declares a step. */
    const markdown_core_finish_step_entry *finish_dispatch[MARKDOWN_CORE_FINISH_KEY_COUNT];
    size_t finish_step_slots;
    /* Each kind's record, by kind index (markdown_core_finish_kind_index). */
    markdown_core_kind_record kinds[MARKDOWN_CORE_FINISH_KIND_COUNT + 1];
    /* The instances by descriptor, `instance_mask + 1` entries
     * (markdown_core_instance_hash), and the bytes of one inline run's block
     * of records. */
    const markdown_core_element_instance *const *instance_table;
    size_t instance_mask;
    size_t run_state_size;
} markdown_core_dialect;

/* The record of `kind`, and the instance of the structure element of a node's
 * kind -- NULL for no node, as for a kind with none. */
static inline const markdown_core_kind_record *markdown_core_dialect_kind(const markdown_core_dialect *dialect,
                                                                          markdown_core_node_type kind) {
    return &dialect->kinds[markdown_core_finish_kind_index(kind)];
}

static inline const markdown_core_element_instance *
markdown_core_dialect_structure(const markdown_core_dialect *dialect, const markdown_core_node *node) {
    return node ? markdown_core_dialect_kind(dialect, (markdown_core_node_type)node->kind)->structure : NULL;
}

/* The instance of `element` in `dialect`, or NULL when it holds none. Sealing
 * resolves kinds and peers through it, and introspection asks it; parse-time
 * code never does, since every instance a hook reaches is already resolved. */
static inline const markdown_core_element_instance *
markdown_core_dialect_instance(const markdown_core_dialect *dialect, const markdown_core_element *element) {
    size_t mask = dialect->instance_mask;
    for (size_t at = markdown_core_instance_hash(element, mask);; at = (at + 1) & mask) {
        const markdown_core_element_instance *instance = dialect->instance_table[at];
        if (!instance || instance->element == element) {
            return instance;
        }
    }
}

/* What sealing a builder takes, counted from its element list alone: how
 * many finish step entries, projected instance pointers, instance-table
 * entries, resolved peers and gate-table bytes follow the struct beside one instance per
 * element, and the counts that place each projection among them. */
typedef struct markdown_core_dialect_sizes {
    size_t block_totals[MARKDOWN_CORE_BLOCK_HOOK_COUNT];
    bool gated[MARKDOWN_CORE_BLOCK_HOOK_COUNT];
    size_t inline_totals[MARKDOWN_CORE_INLINE_HOOK_COUNT];
    size_t inline_dispatch_offsets[257];
    size_t finish_key_counts[MARKDOWN_CORE_FINISH_KEY_COUNT];
    size_t closed_writers;
    size_t pointers, steps, instance_slots, gate_bytes;
    /* The bytes the elements' parse records take, and one run's records. */
    size_t state_bytes, run_state_bytes;
    /* The peers the elements declare, all together. */
    size_t peers;
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

#ifdef __cplusplus
}
#endif

#endif
