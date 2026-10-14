#ifndef MARKDOWN_CORE_ELEMENT_H
#define MARKDOWN_CORE_ELEMENT_H

#include "node_type.h"
#include "buffer.h"
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

typedef int (*markdown_core_probe_block_func)(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                              markdown_core_chunk *input, int first, int indent,
                                              markdown_core_block_reader *reader);

/* Node-valued fields are independent child-tree roots. This internal hook
 * exposes their owning slots only to parser phases; it does not change the
 * public child iterator or make a field a parent/child edge. Destruction
 * transfers these roots to the shared iterative walk and clears their slots
 * before opaque_free_func releases the element payload. That callback must
 * not recursively destroy node-valued fields. The slots are in the element
 * payload (`node->opaque`), and kind conversion preserves the payload and
 * these roots. */
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
 * This is the definition, a pure function of the kind. Sealing reads it once
 * per kind to project the dialect's kind record (dialect.h,
 * markdown_core_kind_record), and everything the engine asks of a node's kind
 * while parsing it asks that record: the structure's instance and the facts
 * of its descriptor, in one load. It is NOT cached on the node: a cached copy
 * is a second answer to "which element defines this kind" that has to be
 * rewritten at every kind change and can be wrong in between. */
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

/* How many elements one dialect holds at most. The block-start projection
 * lists a family's owners by byte -- a count and then owner indices -- so a
 * family may have at most 255 owners and an owner index at most 254; the
 * attachment API refuses the element that would break that, leaving the
 * dialect as it was, rather than the projection wrapping a byte. */
#define MARKDOWN_CORE_ELEMENT_LIMIT 255

struct markdown_core_inline_root;

struct markdown_core_element {
    /* Negative/zero/positive precedence separates protected tokens, ordinary
     * alternatives, and literal fallbacks without a second dispatch algorithm.
     * A byte's owners are asked in ascending precedence -- any value, not only
     * the named ones -- and equal precedences in descriptor order. */
    markdown_core_inline_precedence inline_precedence;
    markdown_core_draft *(*parse_text)(const markdown_core_element_instance *, markdown_core_parser *,
                                       markdown_core_inline_state *, bufsize_t);
    void (*init_inline)(const markdown_core_element_instance *, markdown_core_inline_state *);
    void (*finish_inline)(const markdown_core_element_instance *, markdown_core_inline_state *);
    void (*dispose_inline)(const markdown_core_element_instance *, markdown_core_inline_state *);
    /* Whether a token the element keeps open in the run -- a bracket, a
     * citation token waiting for its group -- can still change what the run
     * makes of the content that follows (docs/plans/2026-09-29-incremental-
     * parsing.md, 5.6). */
    bool (*holds_inline)(const markdown_core_element_instance *, const markdown_core_inline_state *);
    /* A text piece completes in what encloses it, given the count of the
     * word bodies around it (node.h, markdown_core_piece). */
    void (*complete_inline)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_piece *, int);
    /* A lazy line -- one that did not match every open container's prefix
     * and opened no block -- is offered to the current block. `accepts_lazy`
     * says whether it takes the line; `open_lazy` returns the block the line is
     * added to, from its first non-space byte: the block itself when the line
     * continues it, or a block it opens, which starts where a block the line
     * opened would. NULL means the line was consumed or the parse failed, and
     * leaves the parser's current block where the hook put it. A block that
     * accepts the line takes it as a paragraph takes a lazy line: the starts
     * that refuse a lazy line (`block_start_context`) open no block on it, so
     * the line is lazy exactly when it would be after a paragraph's line. */
    markdown_core_frame *(*open_lazy)(const markdown_core_element_instance *, markdown_core_parser *,
                                      markdown_core_frame *, markdown_core_chunk *);
    bool (*accepts_lazy)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_frame *);
    unsigned speculative_flags;
    markdown_core_content_mode content_mode;
    bool inline_content, paragraph, blank_opaque, blank_runs, propagates_child_blank, pending_close;
    int maximum_block_indent;
    void (*init_document)(const markdown_core_element_instance *, markdown_core_parser *);
    void (*dispose_parser)(const markdown_core_element_instance *, markdown_core_parser *);
    void (*dispose_document)(const markdown_core_element_instance *, markdown_core_parser *);
    void (*read_document_prefix)(const markdown_core_element_instance *, markdown_core_parser *);
    void (*prepare_document)(const markdown_core_element_instance *, markdown_core_parser *);
    void (*finish_document)(const markdown_core_element_instance *, markdown_core_parser *);
    /* The node a frame builds is complete: it numbers the children it has
     * not numbered yet and then its fields, in canonical field order, and
     * gives each the id of the old node it continues or the next, and its
     * extent, measured from `start`, where the node begins
     * (docs/plans/2026-09-29-incremental-parsing.md, 5.8, 5.9). */
    void (*complete_node)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_frame *,
                          uint32_t start);
    /* The node a frame builds takes another child: it numbers the children
     * it holds before it, as far as each has completed (5.8). */
    void (*number_children)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_frame *);
    /* A node holding an inline root is about to read its content, its
     * kind and range final: it decides what old node it continues (5.9),
     * whose content it reads again (5.6). */
    void (*decide_frame)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_frame *);
    /* An inline root's parse begins, its holder's kind and range final: the
     * root decides what old node it continues (5.9), whose content it reads
     * again (5.6) and whose nodes the nodes of its content may continue. */
    void (*open_root)(const markdown_core_element_instance *, markdown_core_parser *,
                      struct markdown_core_inline_root *);
    /* A node of an inline root's content is complete, the nodes it holds
     * complete before it: it numbers what it holds, as `complete_node` does,
     * each taking the old node it continues (5.9). */
    void (*complete_draft)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_draft *);
    /* The root's parse takes an old node of the old root's content whole:
     * its draft continues it (5.6, 5.9). */
    void (*take_old)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_draft *);
    /* A draft that matched an old node is discarded: the old node is one the
     * root's nodes may continue again. */
    void (*forget_draft)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_draft *);
    /* The last step of the parse: every node is complete, and the owner
     * publishes the document (identity, definition tables). */
    void (*publish_document)(const markdown_core_element_instance *, markdown_core_parser *);
    markdown_core_frame *(*open_text_block)(const markdown_core_element_instance *, markdown_core_parser *,
                                            markdown_core_frame *, markdown_core_chunk *);
    /* A start that precedes every scanned one, read in the line's context
     * as a scanned start is (block_start_context). */
    markdown_core_frame *(*try_interrupting_block)(const markdown_core_element_instance *, markdown_core_parser *,
                                                   struct markdown_core_block_start_context *);
    bool interrupts_paragraph;

    /* THE STATE A CONTAINER CARRIES where a child of it can begin
     * (docs/plans/2026-09-29-incremental-parsing.md, 5.4, E3): the word its
     * opening line decided, which its later lines read. An element whose
     * blocks hold blocks that a later parse may take declares it; the cursor
     * reads the old children of such a block again (5.3), and reads a block
     * of an element without it whole. */
    uint32_t (*carry_save)(const markdown_core_element_instance *, const markdown_core_frame *);
    /* WHAT ITS CHILDREN ADD TO THAT STATE, when its later lines read what
     * the children before them hold (E4): the summary its children tree
     * keeps, and the fold that adds the combined summary of old children the
     * cursor takes whole (5.3), so the state after them is the old one. */
    const markdown_core_stem_summary *children_summary;
    void (*fold_children)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_frame *,
                          uint64_t);
    bool (*continue_container)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_frame *,
                               markdown_core_chunk *, const markdown_core_frame *, bool *);
    /* The bytes `continue_container` can strip from a line besides
     * indentation: the COMPLETE set, as a gate's is, and NULL when it strips
     * indentation only. A block-start question asked of a LATER line from raw
     * source (the definition gate) reaches that line's first stripped byte by
     * walking over indentation and these, so a byte left out here does not
     * make the parser slower, it makes it WRONG: the construct inside this
     * container is silently never recognised. Projected with the hooks into
     * the dialect's `container_prefix`. A declared byte that is also the marker
     * byte of the grammar asking (':' or '~' for a definition) cannot be told
     * from that marker in raw source, so the key hands such a line to the
     * lookahead rather than walking over it. */
    const char *container_prefix_bytes;
    bool (*accepts_blank)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_frame *);
    bool (*blank_line)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_frame *);
    bool (*ends_block)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_frame *,
                       markdown_core_chunk *);
    void (*finalize_block)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_frame *);
    /* A block whose inline content is read in its own way: the hook reads
     * the tokens of `inline_state`, the run of its content, one at a time
     * (markdown_core_inline_complete_token, markdown_core_inline_parse_inline)
     * and decides what its text gives the block -- a heading's attributes
     * and label (docs/plans/2026-09-29-incremental-parsing.md, 5.8). NULL
     * reads every token. A read that waits (parser.h, `unsettled`) stops. */
    void (*read_content)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_inline_state *);
    /* The key groups the read of the block's content declares in, one bit
     * per group (registry.h): the inline stage reads these roots first. */
    uint32_t content_declares;

    bool (*scan_block_start)(const markdown_core_element_instance *, markdown_core_parser *,
                             struct markdown_core_block_start_context *, struct markdown_core_block_start *);
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
     * dialect's byte-indexed projection of it; a byte another element also
     * terminates text at keeps none, and always reaches ordinary element
     * dispatch. */
    bool (*is_inline_start)(const markdown_core_element_instance *, markdown_core_inline_state *, bufsize_t);
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
    const char *flanking_transparent; /* scan_delimiter looks through it; ASCII */
    const char *name;
    /* A retained built-in payload may only parent these kinds after conversion.
     * NULL uses the core kind domain. Dynamic policy remains a separate decision. */
    const markdown_core_node_type *containment_kinds;
    markdown_core_can_contain_func can_contain_func;
    markdown_core_contains_inlines_func contains_inlines_func;
    markdown_core_accepts_lines_func accepts_lines_func;
    /* THE COMPLETION OF A TEXT (markdown-core-element-api.h): the step,
     * asked of each Text of an inline root's content that no node of
     * `complete_text_outside` encloses, as the node holding it completes. */
    markdown_core_complete_text_func complete_text;
    markdown_core_node_type complete_text_outside;
    /* Whether a construct the element keeps open in the run -- a bracket --
     * can still become a node of `kind` around the nodes the run reduces now
     * (markdown_core_inline_reduce). */
    bool (*encloses_inline)(const markdown_core_element_instance *, const markdown_core_inline_state *,
                            markdown_core_node_type);
    markdown_core_opaque_alloc_func opaque_alloc_func;
    markdown_core_opaque_free_func opaque_free_func;
    markdown_core_opaque_copy_func opaque_copy_func;
    markdown_core_visit_owned_subtrees_func visit_owned_subtrees_func;
    /* The bytes of the element's parse record and of its record in each
     * inline run (markdown-core-element-api.h, "AN ELEMENT AS ONE PARSE HOLDS
     * IT"); zero declares none. */
    size_t state_size, run_state_size;
    /* The elements whose state this element's code reads, NULL-terminated
     * (markdown-core-element-api.h, "AN ELEMENT AS ONE PARSE HOLDS IT").
     * Sealing resolves them, in this order, into the instance's `peers`. */
    const markdown_core_element *const *peers;
};

#endif
