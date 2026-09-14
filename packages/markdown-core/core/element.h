#ifndef MARKDOWN_CORE_ELEMENT_H
#define MARKDOWN_CORE_ELEMENT_H

#include "markdown-core.h"
#include "markdown-core-element-api.h"
#include "config.h"
#include "chunk.h"
#include "delimiter.h"
#include "node.h"

/* A speculative opener reads following lines through its caller's source
 * view. Captured and streaming inputs therefore use the same grammar without
 * nesting parser transactions or changing tree ownership. */
typedef struct markdown_core_block_reader {
    void *context;
    int (*next)(void *context, markdown_core_chunk *input, int *first, int *indent);
} markdown_core_block_reader;

typedef int (*markdown_core_probe_block_func)(markdown_core_parser *parser, markdown_core_chunk *input, int first,
                                              int indent, markdown_core_block_reader *reader);

/* Node-valued fields are independent child-tree roots. This internal hook
 * exposes their owning slots only to parser phases; it does not change the
 * public child iterator or make a field a parent/child edge. Destruction
 * transfers these roots to the shared iterative walk and clears their slots
 * before opaque_free_func releases the element payload. That callback must
 * not recursively destroy node-valued fields. Kind conversion preserves the
 * element payload and these roots. */
typedef int (*markdown_core_visit_owned_subtrees_func)(const markdown_core_element *element, markdown_core_node *node,
                                                       markdown_core_owned_subtree_visitor visitor, void *context);

struct markdown_core_block_start_context;
struct markdown_core_block_start;

typedef enum {
    MARKDOWN_CORE_INLINE_TOKEN = -1,
    MARKDOWN_CORE_INLINE_DEFAULT,
    MARKDOWN_CORE_INLINE_FALLBACK
} markdown_core_inline_precedence;

typedef enum {
    MARKDOWN_CORE_CONTENT_CONTAINER,
    MARKDOWN_CORE_CONTENT_PROSE,
    MARKDOWN_CORE_CONTENT_LITERAL
} markdown_core_content_mode;
/* The immutable kind -> structure projections, indexed by the kind's value
 * bits. Defined with the element registry; looked up inline everywhere. */
extern const markdown_core_element *const markdown_core_block_structure[];
extern const markdown_core_element *const markdown_core_inline_structure[];
extern const size_t markdown_core_block_structure_count, markdown_core_inline_structure_count;

/* The value bits of a kind: its index among the block or the inline kinds. */
static MARKDOWN_CORE_INLINE unsigned markdown_core_kind_value(markdown_core_node_type kind) {
    return (unsigned)kind & MARKDOWN_CORE_NODE_VALUE_MASK;
}

static MARKDOWN_CORE_INLINE const markdown_core_element *
markdown_core_structure_for_kind(markdown_core_node_type kind) {
    unsigned index = markdown_core_kind_value(kind);
    if (MARKDOWN_CORE_NODE_TYPE_INLINE_P(kind)) {
        return index < markdown_core_inline_structure_count ? markdown_core_inline_structure[index] : NULL;
    }
    return index < markdown_core_block_structure_count ? markdown_core_block_structure[index] : NULL;
}
static MARKDOWN_CORE_INLINE const markdown_core_element *markdown_core_node_structure(const markdown_core_node *node) {
    return node ? markdown_core_structure_for_kind((markdown_core_node_type)node->kind) : NULL;
}

struct markdown_core_element {
    /* Negative/zero/positive precedence separates protected tokens, ordinary
     * alternatives, and literal fallbacks without a second dispatch algorithm. */
    markdown_core_inline_precedence inline_precedence;
    markdown_core_node *(*parse_text)(markdown_core_parser *, markdown_core_inline_state *, bufsize_t);
    void (*init_inline)(markdown_core_inline_state *);
    void (*begin_inline)(markdown_core_parser *, markdown_core_inline_state *, markdown_core_node *);
    bool (*claim_inline_tail)(markdown_core_inline_state *, markdown_core_node *);
    void (*finish_inline)(markdown_core_inline_state *);
    void (*dispose_inline)(markdown_core_inline_state *);
    void (*complete_inline)(markdown_core_parser *, markdown_core_node *, int);
    /* complete_inline -- and a document structure's observe_inline -- is
     * delivered by a walk over an inline root once its delimiters and
     * brackets are resolved. An element that sets this asks for that walk
     * itself, through markdown_core_inline_request_completion, for the roots
     * that need it; one that does not makes every root of the document pay
     * the walk. */
    bool complete_inline_on_request;
    markdown_core_node *(*open_lazy)(markdown_core_parser *, markdown_core_node *);
    bool (*accepts_lazy)(markdown_core_parser *, markdown_core_node *);
    unsigned speculative_flags;
    markdown_core_content_mode content_mode;
    bool inline_content, deferred_inlines, paragraph, blank_opaque, blank_runs, propagates_child_blank, pending_close;
    int maximum_block_indent;
    void (*init_document)(markdown_core_parser *);
    void (*dispose_parser)(markdown_core_parser *);
    void (*dispose_document)(markdown_core_parser *);
    size_t (*read_document_prefix)(markdown_core_parser *, const unsigned char *, size_t);
    void (*prepare_document)(markdown_core_parser *);
    void (*finish_document)(markdown_core_parser *);
    void (*observe_inline)(markdown_core_parser *, markdown_core_node *);
    markdown_core_node *(*open_text_block)(markdown_core_parser *, markdown_core_node *, markdown_core_chunk *);
    markdown_core_node *(*try_interrupting_block)(markdown_core_parser *, markdown_core_node *, markdown_core_chunk *,
                                                  bool);
    bool interrupts_paragraph;

    bool (*continue_container)(markdown_core_parser *, markdown_core_node *, markdown_core_chunk *,
                               const markdown_core_node *, bool *);
    bool (*accepts_blank)(markdown_core_parser *, markdown_core_node *);
    bool (*blank_line)(markdown_core_parser *, markdown_core_node *);
    bool (*ends_block)(markdown_core_parser *, markdown_core_node *, markdown_core_chunk *);
    void (*finalize_block)(markdown_core_parser *, markdown_core_node *);
    void (*complete_block)(markdown_core_parser *, markdown_core_node *);

    /* The bytes at a line's first non-space position at which any of this
     * element's block hooks (scan_block_start, try_interrupting_block,
     * try_opening_block, try_opening_paragraph) can accept. NULL means every
     * byte, including a line end; an element whose grammar begins with a
     * known byte is not consulted for lines that begin otherwise. */
    const char *block_start_bytes;
    bool (*scan_block_start)(markdown_core_parser *, struct markdown_core_block_start_context *,
                             struct markdown_core_block_start *);
    /* Last refusal before an ordinary paragraph, after opaque blocks/tables. */
    markdown_core_open_block_func try_opening_paragraph;
    markdown_core_match_block_func last_block_matches;
    /* The speculative form of `last_block_matches` a block-start lookahead asks
     * (see the typedef); required of an element whose blocks contain blocks. */
    markdown_core_continues_block_func continues_block;
    markdown_core_open_block_func try_opening_block;
    /* Non-consuming recognition before this element's block-opening slot.
     * Shares the producer's grammar; may report allocation failure, but never
     * opens a node or claims source. */
    markdown_core_probe_block_func probe_block;
    /* Parsed delimiter semantics are declared by their element. Pairing,
     * run classification and node construction stay in the shared engine. */
    markdown_core_delimiter_rule delimiter_rule;
    unsigned char delimiter_character;
    delimiter_rule_spec delimiter;
    /* Optional non-consuming candidate predicate. False rules this owner out;
     * true still requires a full match. Text scanning consults the parser's
     * byte-indexed projection: any eligible owner can terminate text, and a
     * missing predicate accepts unconditionally. */
    bool (*can_start)(markdown_core_inline_state *, bufsize_t);
    markdown_core_match_inline_func match_inline;
    markdown_core_inline_from_delim_func insert_inline_from_delim;
    /* THREE byte sets, not one list.
     *
     * `special_inline_chars` was a single `llist` read by five consumers that
     * each meant something different by it: two byte tables were folded out of
     * it, `try_elements` used it for cursor dispatch,
     * `get_element_for_special_char` used it for delimiter-tag OWNERSHIP,
     * `bracket_takes_close_bracket` used it for `]` arbitration, and
     * `handle_backslash` used it to disable a core optimisation. One list
     * cannot say three different things, and D1 and D2 are what happens when it
     * tries: `set_emphasis` folded every byte an element named into
     * `skip_chars`, which killed CommonMark flanking merely by attaching the
     * element, and `'}'` sat in the list dispatching to nothing.
     *
     * Each set is a NUL-terminated byte list; NUL itself is never a member
     * because source normalization replaces it before inlines run. A NULL set
     * is empty. */
    const char *terminates_text;      /* ends a text run: inline_state_find_special_char */
    const char *dispatch;             /* offered to match_inline, and owns a delimiter tag */
    const char *flanking_transparent; /* scan_delims looks through it */
    const char *name;
    markdown_core_get_type_string_func get_type_string_func;
    markdown_core_can_contain_func can_contain_func;
    markdown_core_contains_inlines_func contains_inlines_func;
    markdown_core_accepts_lines_func accepts_lines_func;
    markdown_core_postprocess_func postprocess_func;
    markdown_core_finish_node_func finish_node;
    /* The node kinds `finish_node` acts on, terminated by
     * MARKDOWN_CORE_NODE_NONE: the finishing walk offers the hook only those
     * nodes. NULL offers it every node. */
    const markdown_core_node_type *finish_node_kinds;
    /* Bytes of element payload every node created with this element carries
     * inside its own record, zeroed, with `opaque` pointing at them: a
     * formula's or a directive's fixed-size record costs no allocation of
     * its own. `opaque_free_func` releases what the payload owns, never the
     * payload storage, which is the node's -- in its record, or freed by
     * the core when markdown_core_node_opaque_take had to allocate it. */
    size_t opaque_size;
    markdown_core_opaque_alloc_func opaque_alloc_func;
    markdown_core_opaque_free_func opaque_free_func;
    markdown_core_visit_owned_subtrees_func visit_owned_subtrees_func;
};

/* Whether `node` holds node-valued fields at all: a node a core kind attached
 * a field to (MARKDOWN_CORE_NODE__OWNS_FIELDS), or one of an element that
 * declares a field visitor. Every other node is passed over without a
 * visitor call, and without reading its payload. */
static MARKDOWN_CORE_INLINE bool markdown_core_node_may_own_inline_subtrees(const markdown_core_node *node) {
    return (node->flags & MARKDOWN_CORE_NODE__OWNS_FIELDS) != 0 ||
           (node->element && node->element->visit_owned_subtrees_func);
}

#endif
