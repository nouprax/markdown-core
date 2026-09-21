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
/* Which element defines the structure of a node KIND -- distinct from
 * `node->element`, which is the element that created that node INSTANCE.
 *
 * This is a pure function of the kind: two loads from a constant table. It
 * used to live behind a call in core-elements.c, out of line and in another
 * translation unit, 17 instructions asked once per element-descriptor field
 * access. On the 65 same-job documents that was about 5.7 million calls;
 * `block-list-flat` spent 5.48% of its whole parse inside it, and cmark has
 * no counterpart at all.
 *
 * So the tables are declared here and the projection is what it always was,
 * an array index, at every call site. It is NOT cached on the node: a cached
 * copy is a second answer to "which element defines this kind" that has to be
 * rewritten at every kind change and can be wrong in between. Both tables sit
 * in the same shared object as their callers, which are built with hidden
 * visibility, so the index resolves PC-relative with no indirection. */
extern const markdown_core_element *const markdown_core_block_structure[MARKDOWN_CORE_NODE_KIND_COUNT];
extern const markdown_core_element *const markdown_core_inline_structure[MARKDOWN_CORE_NODE_KIND_COUNT];

static inline const markdown_core_element *markdown_core_structure_for_kind(markdown_core_node_type kind) {
    unsigned index = (unsigned)kind & MARKDOWN_CORE_NODE_VALUE_MASK;
    if (index >= MARKDOWN_CORE_NODE_KIND_COUNT) {
        return NULL;
    }
    return MARKDOWN_CORE_NODE_TYPE_INLINE_P(kind) ? markdown_core_inline_structure[index]
                                                  : markdown_core_block_structure[index];
}

static inline const markdown_core_element *markdown_core_node_structure(const markdown_core_node *node) {
    return node ? markdown_core_structure_for_kind((markdown_core_node_type)node->kind) : NULL;
}

/* What the block dispatcher may know about a hook's grammar without entering
 * it: the COMPLETE set of first non-space bytes that can lead the hook to claim
 * a line. NULL means ungated -- the hook is asked about every line, which is
 * what every hook did before gates existed, so an element that declares nothing
 * keeps exactly its old behaviour.
 *
 * A byte wrongly left out of a declared set does not make the parser slower, it
 * makes it WRONG: the construct is silently never recognised. So the set
 * belongs to the element that owns the grammar, and api_test checks it against
 * that grammar rather than against a fixture.
 *
 * A gate is a statement about ONE LINE. A grammar decided by a later line --
 * a Pandoc simple table, whose prose header is only a table because the NEXT
 * line is dashes -- cannot be expressed here and must not be gated.
 *
 * A gate speaks about a line that is NOT indented code. Four columns of
 * indentation are a block start of their own, decided by no byte, so the scan
 * and interrupt families ask an indented line only of the owners whose
 * `maximum_block_indent` reaches it, whatever their gate says, and ask a
 * non-indented line only of the owners whose gate admits its first byte. The
 * two descriptor facts compose; neither is a branch on the input. */
typedef struct markdown_core_block_gate {
    const char *bytes;
} markdown_core_block_gate;

/* How many elements one registry holds at most. The block-start projection
 * lists a family's owners by byte -- a count and then owner indices -- so a
 * family may have at most 255 owners and an owner index at most 254; the
 * attachment API refuses the element that would break that, leaving the
 * registry as it was, rather than the projection wrapping a byte. */
#define MARKDOWN_CORE_ELEMENT_LIMIT 255

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
    /* The bytes `continue_container` can strip from a line besides
     * indentation: the COMPLETE set, as a gate's is, and NULL when it strips
     * indentation only. A block-start question asked of a LATER line from raw
     * source (the definition gate) reaches that line's first stripped byte by
     * walking over indentation and these, so a byte left out here does not
     * make the parser slower, it makes it WRONG: the construct inside this
     * container is silently never recognised. Projected with the hooks into
     * `parser->container_prefix`. A declared byte that is also the marker
     * byte of the grammar asking (':' or '~' for a definition) cannot be told
     * from that marker in raw source, so the key hands such a line to the
     * lookahead rather than walking over it. */
    const char *container_prefix_bytes;
    bool (*accepts_blank)(markdown_core_parser *, markdown_core_node *);
    bool (*blank_line)(markdown_core_parser *, markdown_core_node *);
    bool (*ends_block)(markdown_core_parser *, markdown_core_node *, markdown_core_chunk *);
    void (*finalize_block)(markdown_core_parser *, markdown_core_node *);

    bool (*scan_block_start)(markdown_core_parser *, struct markdown_core_block_start_context *,
                             struct markdown_core_block_start *);
    /* What `scan_block_start` needs on a non-indented line before it is worth
     * entering; an indented line reaches it through `maximum_block_indent`. */
    markdown_core_block_gate scan_block_gate;
    /* Last refusal before an ordinary paragraph, after opaque blocks/tables. */
    markdown_core_open_block_func try_opening_paragraph;
    markdown_core_match_block_func last_block_matches;
    /* The speculative form of `last_block_matches` a block-start lookahead asks
     * (see the typedef); required of an element whose blocks contain blocks. */
    markdown_core_continues_block_func continues_block;
    markdown_core_open_block_func try_opening_block;
    /* What `try_opening_block` needs on the line before it is worth entering. */
    markdown_core_block_gate open_block_gate;
    /* What `try_interrupting_block` needs on a non-indented line. */
    markdown_core_block_gate interrupt_block_gate;
    /* Non-consuming recognition before this element's block-opening slot.
     * Shares the producer's grammar; may report allocation failure, but never
     * opens a node or claims source. */
    markdown_core_probe_block_func probe_block;
    /* Parsed delimiter semantics are declared by their element. Pairing,
     * run classification and node construction stay in the shared engine. */
    markdown_core_delimiter_rule delimiter_rule;
    unsigned char delimiter_character;
    delimiter_rule_spec delimiter;
    /* Optional non-consuming token predicate. Text scanning consults the
     * parser's byte-indexed projection; all owners of a shared byte must
     * agree, otherwise that byte always reaches ordinary element dispatch. */
    bool (*is_inline_start)(markdown_core_inline_state *, bufsize_t);
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
    /* A retained built-in payload may only parent these kinds after conversion.
     * NULL uses the core kind domain. Dynamic policy remains a separate decision. */
    const markdown_core_node_type *containment_kinds;
    markdown_core_can_contain_func can_contain_func;
    markdown_core_contains_inlines_func contains_inlines_func;
    markdown_core_accepts_lines_func accepts_lines_func;
    /* The two finish-stage hook shapes; an element declares at most one (the
     * API header states the LOCAL/GLOBAL invariant that separates them, and
     * registration refuses a descriptor that declares both).
     *
     * `finish_step` is asked from inside the one finish walk, at the EXIT of
     * every node whose kind is in `finish_exit_kinds` and at the ENTER and
     * EXIT of every node whose kind is in `finish_scope_kinds`. It costs the
     * document nothing at any other event: the walk projects the steps by
     * (event, kind) once per parse, so a step asked at Text is not so much as
     * looked at when a Paragraph closes, nor when a Text opens.
     * `postprocess_func` is handed each root after that root's walk and walks
     * it again itself. */
    markdown_core_finish_step_func finish_step;
    markdown_core_postprocess_func postprocess_func;
    /* The node kinds the element's finish hook ACTS ON, terminated by
     * MARKDOWN_CORE_NODE_NONE; NULL declares nothing. This is THE GATE, set
     * by a step and by a pass alike and meaning the same for both shapes: the
     * set is intersected with the kinds
     * the parse actually produced, taken when the block tree is complete, and
     * a hook that cannot find anything is skipped -- a pass along with the
     * traversal it would have made, a step at every event it was projected
     * to. A hook whose trigger kind is CREATED by an earlier hook must
     * therefore name that creator kind too.
     *
     * The kinds a hook acts on are not always the kinds it is asked at:
     * formula rewrites a Formula, a CodeBlock and a FormulaBlock, and the
     * Formula's rewrite -- the paragraph that holds nothing else becomes a
     * FormulaBlock -- is decided at the PARAGRAPH's EXIT, where the paragraph
     * may be replaced. So a step says where it is asked separately, below,
     * and the gate stays what makes a document with no formula pay nothing
     * at each of its paragraphs.
     *
     * The kinds are declared as KINDS rather than as a precomputed bit set so
     * that each one keeps its namespace: block and inline values collide once
     * masked, and a Formula written where a block kind belongs has to be
     * detectable rather than silently becoming some unrelated block's bit.
     * The engine projects them per parse. */
    const markdown_core_node_type *finish_acts_on_kinds;
    /* WHERE A FINISH STEP IS ASKED, two lists terminated the same way; NULL
     * declares nothing, and a pass declares neither.
     *
     * `finish_exit_kinds`: the step receives the EXIT of these kinds, the
     * point where the node's subtree is complete and the node may be rewritten
     * or replaced. It names the kind of the node the step is handed --
     * PARAGRAPH for formula's promotion, TEXT for autolink's scan.
     *
     * `finish_scope_kinds`: the kinds whose EXTENT the step tracks. It
     * receives their ENTER and their EXIT and nothing else about them: it acts
     * on nothing there, it learns that the nodes to come are inside one, and
     * it keeps that in its per-root state word. Autolink declares LINK -- an
     * address inside a Link is already a link's text.
     *
     * The two are disjoint: a kind in both would be one event asked twice,
     * and registration refuses the descriptor. It also refuses a step that
     * names no kind in either list, which would never be asked, and a kind
     * the dispatch table cannot index (an ordinal at or past
     * MARKDOWN_CORE_NODE_KIND_COUNT, or a value of neither class). */
    const markdown_core_node_type *finish_exit_kinds;
    const markdown_core_node_type *finish_scope_kinds;
    markdown_core_opaque_alloc_func opaque_alloc_func;
    markdown_core_opaque_free_func opaque_free_func;
    markdown_core_visit_owned_subtrees_func visit_owned_subtrees_func;
};

/* Defined here rather than in node.c because the ANSWER IS NO for almost every
 * node, and the question was costing a cross-translation-unit call to find that
 * out. `walk_owned_trees` asks it once per node of every tree it walks --
 * 2,981,851 times over the 65 same-job benchmark documents, of which 136,500
 * reach an element hook and about 26,800 visit anything at all. Inlined, the
 * common answer is a compare against `kind` and a NULL test on a pointer.
 *
 * It lives in element.h, not beside its declaration in node.h, because it
 * reads through `markdown_core_element`, which node.h only forward-declares.
 *
 * Kept as ONE definition rather than a cheap predicate placed beside the real
 * one: a second copy of "which kinds can own a subtree" drifts from the list
 * below the first time a kind is added to it. */
/* WHICH KINDS CAN OWN A SUBTREE THROUGH THEIR OWN RECORD: the one predicate,
 * read by the visitor below and projected into the finish walk's per-kind
 * record (parser.h, MARKDOWN_CORE_FINISH_KIND_FIELDS), so the walk asks it
 * once per parse per kind rather than three compares per node. An element
 * that owns subtrees through `visit_owned_subtrees_func` is found through
 * the node's `element`, which the walk tests beside the flag. */
static inline bool markdown_core_kind_owns_fields(markdown_core_node_type kind) {
    return kind == MARKDOWN_CORE_NODE_DEFINITION || kind == MARKDOWN_CORE_NODE_CALLOUT ||
           kind == MARKDOWN_CORE_NODE_CITE;
}

static inline int markdown_core_visit_inline_subtrees(markdown_core_node *node,
                                                      markdown_core_owned_subtree_visitor visitor, void *context) {
    if (markdown_core_kind_owns_fields((markdown_core_node_type)node->kind)) {
        if (node->kind == MARKDOWN_CORE_NODE_DEFINITION && node->as.definition->term &&
            !visitor(&node->as.definition->term, context)) {
            return 0;
        }
        if (node->kind == MARKDOWN_CORE_NODE_CALLOUT && node->as.callout->title &&
            !visitor(&node->as.callout->title, context)) {
            return 0;
        }
        if (node->kind == MARKDOWN_CORE_NODE_CITE) {
            for (markdown_core_node *item = node->as.cite->citations; item; item = item->next) {
                if ((item->as.citation->prefix && !visitor(&item->as.citation->prefix, context)) ||
                    (item->as.citation->suffix && !visitor(&item->as.citation->suffix, context))) {
                    return 0;
                }
            }
        }
    }
    const markdown_core_element *element = node->element;
    return !element || !element->visit_owned_subtrees_func ||
           element->visit_owned_subtrees_func(element, node, visitor, context);
}

#endif
