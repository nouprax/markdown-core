#ifndef MARKDOWN_CORE_PARSER_H
#define MARKDOWN_CORE_PARSER_H

#include <stdint.h>
#include "references.h"
#include "node.h"
#include "buffer.h"
#include "../elements/heading_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_LINK_LABEL_LENGTH 1000

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

/* Immutable runs map logical content bytes to authored byte intervals.
 * Blocks append runs as lines arrive; transformed cells and decoded inline
 * tokens append runs when assembled. Nodes retain index slices with an origin,
 * so splitting a mapped Text shares its runs without copying a suffix. Text
 * consolidation appends each contributing run once to one contiguous map.
 * The parser owns all runs until the parse transaction ends. */
typedef struct {
    /* Logical offset at which this run begins. */
    bufsize_t content_offset;
    /* The source line the slice was copied from, counted from 1. */
    int line;
    /* The BYTE column on that line the slice begins at, counted from 1. */
    int column;
    /* Authored byte width represented by each logical byte in this run. */
    int source_width;
    /* Source columns advanced per logical byte: one for copied bytes,
     * two for a contracted pipe escape, zero within a decoded token. */
    int source_step;
    /* Virtual indentation after container prefixes, before block content was
     * stripped. Slices retain this line provenance, including table leads;
     * synthetic inline runs use zero because they never finalize as blocks. */
    int indent;
} markdown_core_line_mark;

/* A content span resolved against one node's map: where it begins, where it
 * ends, and the two runs that answer both.
 *
 * Placing an inline node and slicing its owner's map for it are the SAME two
 * queries -- `content_place(from)` and `content_end_place(to)` locate exactly
 * the runs `adopt_content_marks(from, to - from + 1)` then searched for again.
 * Every inline node paid for four binary searches to ask two questions.
 *
 * `has_end` is separate from `has_start` because the two offsets are checked
 * independently: several element entry points compute `to` as `x - 1`, which
 * is -1 at the start of a buffer, and a span may legitimately have a start and
 * no end. `last` is resolved by its own search rather than by walking forward
 * from `first`, so a span whose `to` precedes its `from` still names the run
 * that contains `to`. */
typedef struct {
    int first, last;
    int start_line, start_column;
    int end_line, end_column;
    bool has_start, has_end;
} markdown_core_content_span;

/* Parse-time edges for document-owned footnote and specimen definitions.
 * Every definition is already owned in the block tree or a value field.
 * The index is discarded before any mutating postprocessor runs. */
typedef struct {
    struct markdown_core_node *definition;
    struct markdown_core_node *citation;
} markdown_core_definition_entry;

typedef struct {
    markdown_core_definition_entry *values;
    size_t count;
    size_t capacity;
    struct markdown_core_node *last_inline;
} markdown_core_definition_collection;

struct markdown_core_parser {
    /* A hashtable of urls in the current document for cross-references */
    struct markdown_core_map *refmap;
    /* The labels this document defines footnotes for (see references.h). The
     * block phase fills it as each definition opens; the inline phase reads it
     * to decide whether a `[^label]` is a call at all. */
    struct markdown_core_map *footnote_defs;
    markdown_core_definition_collection footnotes;
    markdown_core_definition_collection specimens;
    markdown_core_key_index specimen_ids;
    markdown_core_heading_collection headings;
    anchor_registry anchors;
    const markdown_core_element *document_structure, *text_structure;
    /* The root node of the parser, always a MARKDOWN_CORE_NODE_DOCUMENT */
    struct markdown_core_node *root;
    /* The active block grammar boundary. The document and mapped cell inputs
     * share this parser and all document registries. Input roots stay owned by
     * the AST; the queue only borrows them until their block content is read. */
    struct markdown_core_node *block_root;
    struct markdown_core_node *matched_container;
    struct markdown_core_node **block_inputs;
    size_t block_input_count, block_input_capacity, block_input_cursor;
    bufsize_t *input_line_offsets;
    size_t input_line_count, input_line_capacity;
    int input_first_line;
    /* A complete candidate may consume through a later source boundary. The
     * source driver advances to it after the current line has finished. */
    const unsigned char *claimed_cursor;
    int claimed_line;
    bufsize_t claimed_last_column;
    /* The last open block after a line is fully processed */
    struct markdown_core_node *current;
    /* See the documentation for markdown_core_parser_get_line_number() in markdown_core.h */
    int line_number;
    /* See the documentation for markdown_core_parser_get_offset() in markdown_core.h */
    bufsize_t offset;
    /* See the documentation for markdown_core_parser_get_column() in markdown_core.h */
    bufsize_t column;
    /* See the documentation for markdown_core_parser_get_first_nonspace() in markdown_core.h */
    bufsize_t first_nonspace;
    /* See the documentation for markdown_core_parser_get_first_nonspace_column() in markdown_core.h
     */
    bufsize_t first_nonspace_column;
    bufsize_t thematic_break_kill_pos;
    /* See the documentation for markdown_core_parser_get_indent() in markdown_core.h */
    int indent;
    /* See the documentation for markdown_core_parser_is_blank() in markdown_core.h */
    bool blank;
    /* See the documentation for markdown_core_parser_has_partially_consumed_tab() in
     * markdown_core.h */
    bool partially_consumed_tab;
    /* Contains the currently processed line */
    markdown_core_strbuf curline;
    /* See the documentation for markdown_core_parser_get_last_line_length() in markdown_core.h */
    bufsize_t last_line_length;
    /* Scratch for a source line containing NUL bytes; curline holds the
     * normalized line currently being parsed. */
    markdown_core_strbuf line_scratch;
    /* Options set by the user, see the Options section in markdown_core.h */
    /* Sticky allocation-failure flag: once any parse structure is lost, the
     * one-shot transaction reports the whole parse as failed (NULL) instead of
     * returning a silently truncated document. */
    bool oom;
    /* Bytes inspected by the cross-link scanner, for deterministic complexity gates. */
    size_t cross_link_scan_work;
    size_t autolink_domain_work;
    size_t opaque_scan_work;
    size_t footnote_body_work;
    size_t definition_registration_work;
    /* Run bytes, opener comparisons, and child moves in the shared delimiter algorithm. */
    size_t delimiter_work;
    /* Ordinary whitespace scalars and contextual-space lookahead bytes. */
    size_t whitespace_work;
    size_t bracket_work;
    /* Opener checks of the `%%` comment scanner; and the lines the block-start
     * lookahead visited plus the prefix bytes each visit matched itself, for
     * the linearity gates of both. */
    size_t comment_scan_work;
    size_t block_lookahead_work;
    size_t table_scan_work, table_frontier_peak;
    size_t table_workspace_growth, table_geometry_lines, table_separator_scans;
    /* Properties work: source ranges decoded once at their owning boundary. */
    size_t metadata_decoded_bytes;
    /* Bytes examined by the shared block-identifier suffix scanner. */
    size_t block_identifier_work;
    size_t callout_scan_work;
    /* Shared attribute grammar and attachment work. */
    size_t attribute_work;
    /* Projection bytes and registry spelling work, including collision probes. */
    size_t anchor_work;
    /* Ordinary image-label bytes and bounded dimension work for Embedded and embeds. */
    size_t dimension_work;
    size_t list_marker_work;
    size_t specimen_work;
    size_t citation_work;
    /* Cumulative capacity bytes reserved for per-inline state brace event records. */
    size_t citation_brace_bytes;
    size_t definition_list_work;
    /* THE SOURCE AFTER THE LINE BEING PROCESSED. `S_parse_source` sets the
     * cursor to the first byte of the next raw line before it hands each line
     * to `S_process_line`, so a block start whose grammar needs a later line --
     * the `%%` block comment's closer -- can look ahead without consuming
     * anything (see markdown_core_parser_lookahead_begin). NULL until the
     * first line is processed; `cursor == end` once the input has run out. */
    const unsigned char *lookahead_cursor;
    const unsigned char *lookahead_end;
    /* The input's last line as the block parser will see it, normalized once
     * and reused by every lookahead that reaches it: it has no terminator of
     * its own in the source, and a line handed to the prefix matchers must
     * end in one. */
    markdown_core_strbuf lookahead_last_line;
    bool lookahead_last_line_ready;
    /* The open containers a lookahead matches, root first, with the flags they
     * had when it began; and the per-line resume cache. Both are owned by the
     * parser so a document with many candidates allocates them once. */
    struct markdown_core_node **lookahead_chain;
    markdown_core_node_internal_flags *lookahead_chain_flags;
    int lookahead_chain_alloc;
    struct markdown_core_lookahead_entry *lookahead_entries;
    int lookahead_entries_alloc;
    int lookahead_entries_used;
    int lookahead_base_line;
    /* One active table query borrows this reusable line workspace. Per-line
     * geometry is released by the query; the allocation dies with the parser. */
    struct markdown_core_table_source_line *table_lines;
    size_t table_lines_capacity;
    /* Borrow the fixed immutable dialect registry. Private setup callers may
     * extend it before parsing; only that replacement buffer is owned here. */
    const markdown_core_element *const *elements;
    const markdown_core_element **element_allocation;
    size_t element_count;
    /* Stable descriptor order projected by byte once before inline parsing.
     * Each token visits only its possible owners; offsets include an end sentinel. */
    size_t inline_dispatch_offsets[257];
    const markdown_core_element **inline_dispatch;
    /* The same idea one phase earlier: each block-start hook family projected
     * to the elements that implement it, in descriptor order, once per parse.
     * A line asks four of these families in turn, so without the projection a
     * line pays the whole registry four times to reach the one to four owners
     * that can answer -- `try_interrupting_block` has a single implementer and
     * was reached by walking every element attached to the parser.
     *
     * Order inside a family IS the grammar: the first owner that claims a line
     * wins it, which is why heading precedes thematic break (setext `---`) and
     * thematic break precedes list (`***`). The projection therefore preserves
     * descriptor order rather than grouping by anything else. */
    const markdown_core_element **block_hooks[MARKDOWN_CORE_BLOCK_HOOK_COUNT];
    size_t block_hook_counts[MARKDOWN_CORE_BLOCK_HOOK_COUNT];
    const markdown_core_element **block_hook_allocation;
    /* Each family's declared gates flattened to one 256-bit admitted-byte map
     * per owner, in the family's own order, so a line tests a bit rather than
     * walking a declared set. A NULL map means the family declared nothing and
     * every owner is asked, which is the behaviour a gate replaces. */
    uint8_t *block_gate_bytes[MARKDOWN_CORE_BLOCK_HOOK_COUNT];
    uint8_t *block_gate_allocation;
    /* The inline-content families, projected from the same registry and in the
     * same descriptor order. Zero counts before the projection runs, which is
     * why it runs unconditionally on the one path that creates a parser. */
    const markdown_core_element **inline_hooks[MARKDOWN_CORE_INLINE_HOOK_COUNT];
    size_t inline_hook_counts[MARKDOWN_CORE_INLINE_HOOK_COUNT];
    /* Every node kind this parse produced, accumulated by the consolidation
     * walk that already visits every node just before the postprocess passes
     * run, so the record costs no traversal of its own. */
    /* WHICH KINDS THIS PARSE PRODUCED, recorded where they are produced.
     *
     * Every node creation and every `set_kind` that a parse performs writes
     * here, so the postprocess gate can be evaluated before the finish stage
     * walks anything. Gathering it by a walk instead is what forced the finish
     * stage to traverse the document twice: the gate could not be read until
     * the walk that produced it had finished. See the gate in `S_finish_parse`
     * for what the set over-approximates and why that is sound.
     *
     * Every production creation site goes through `markdown_core_parser_note_kind`;
     * `scripts/audit-parser-kind-record.mjs` holds that. */
    markdown_core_node_kind_set kinds_created;
    markdown_core_ispunct_func backslash_ispunct;
    /* Inline special-character tables for this parser: the core defaults plus
     * the special/emphasis-skip characters of the attached inline elements.
     * Parser-local so concurrent parsers with different element sets never
     * observe each other's characters. */
    const markdown_core_element *delimiter_owners[MARKDOWN_CORE_DELIM_RULE_COUNT];
    markdown_core_delimiter_rule delimiter_chars[256];
    bool (*inline_start_predicates[256])(markdown_core_inline_state *, bufsize_t);
    int8_t special_chars[256];
    int8_t skip_chars[256];
    /* The content-to-source map (see markdown_core_line_mark). It is read while the
     * parse is still running -- the block phase reads it as blocks close and
     * the inline phase reads it before the transaction returns -- and it is
     * released with the rest of the per-parse state. */
    markdown_core_line_mark *line_marks;
    bufsize_t line_marks_size;
    bufsize_t line_marks_alloc;
};

/* THE PARSE'S NODE OPERATIONS, WHICH RECORD THE KIND THEY PRODUCE.
 *
 * `kinds_created` decides which postprocess passes run, so a production site
 * that writes a kind without recording it does not fail a build or a test --
 * it makes the gate skip a pass some document needed, and the defect surfaces
 * as a missing rewrite far from the line that caused it. That is a bad thing
 * to police with an audit over twenty-one call sites, so it is not policed:
 * recording is part of producing a node, in the two operations below.
 *
 * `markdown_core_node_new` and `markdown_core_node_set_kind` stay for callers
 * that have no parse -- the tests build trees by hand -- and production code
 * uses these instead, which `scripts/audit-parser-kind-record.mjs` holds.
 *
 * The record OVER-APPROXIMATES, deliberately. A node the parse creates and
 * then discards leaves its bit set although the finished tree holds no such
 * node. Making it exact would mean observing REMOVAL, and the only way to do
 * that is another walk of the whole tree -- which is the cost this record
 * exists to avoid. Over-approximating can only make the gate skip fewer
 * passes, never miss one, because a kind in the finished tree was necessarily
 * created; and a pass that runs over a tree holding none of its declared kinds
 * finds nothing to do. */
static inline void markdown_core_parser_note_kind(markdown_core_parser *parser, markdown_core_node_type kind) {
    if (parser) {
        markdown_core_node_kind_set_add(&parser->kinds_created, kind);
    }
}

static inline markdown_core_node *markdown_core_parser_make_node(markdown_core_parser *parser,
                                                                 markdown_core_node_type type) {
    markdown_core_parser_note_kind(parser, type);
    return markdown_core_node_new(type);
}

static inline markdown_core_node *markdown_core_parser_make_node_with_ext(markdown_core_parser *parser,
                                                                          markdown_core_node_type type,
                                                                          const markdown_core_element *element) {
    markdown_core_parser_note_kind(parser, type);
    return markdown_core_node_new_with_ext(type, element);
}

static inline markdown_core_node_set_kind_result markdown_core_parser_set_node_kind(markdown_core_parser *parser,
                                                                                    markdown_core_node *node,
                                                                                    markdown_core_node_type kind) {
    markdown_core_parser_note_kind(parser, kind);
    return markdown_core_node_set_kind(node, kind);
}

/* ONE LINE OF THE BLOCK-START LOOKAHEAD'S RESUME CACHE.
 *
 * A candidate that scans forward matches the open containers' prefixes on
 * every line it visits. Two failed candidates that both reach a line have
 * nested container chains -- the later one opened inside the earlier one's
 * scan -- so the later scan resumes each line from the deepest container the
 * earlier one matched, and every (container, line) prefix is matched at most
 * once per parse. `container` is NULL for a line no scan has recorded. */
typedef struct markdown_core_lookahead_entry {
    /* Table grammar search facts under one matched container prefix. */
    const struct markdown_core_node *table_container;
    int table_offset;
    unsigned table_absent;
    const struct markdown_core_node *container;
    /* Its distance from the document root: chain[depth] == container. */
    int depth;
    /* The line state after that container's prefix: what S_advance_offset left. */
    bufsize_t offset;
    bufsize_t column;
    bool partially_consumed_tab;
    /* A list's second consecutive blank line at indentation zero: the innermost
     * open block owns the whole line and nothing below the list is asked. */
    bool taken;
    /* Blank after the recorded container's prefix. */
    bool blank;
    /* On the first line of a run of blank lines: the number of the first line
     * after the run and where it begins, so a later scan whose extra containers
     * accept every blank line steps over the run at once. 0 when not a run. */
    int run_end;
    const unsigned char *run_end_cursor;
} markdown_core_lookahead_entry;
/* ONE LINE'S ENTRY: an index into an array, where the compiler can see it.
 *
 * Lines are numbered from the first line any lookahead visited: candidates
 * come in source order and each begins at the line after its own, so no
 * lookahead asks about an earlier line.
 *
 * The body is a subtraction, a bounds test and an address. It was an ordinary
 * out-of-line function, asked once per line visit from two translation units
 * -- about 10,600 times on `block-hr` alone, at 147 Ir a call inclusive, of
 * which 27 Ir was the prologue and epilogue of a call that computes an array
 * subscript.
 *
 * The growth path is what kept it out of line, and it is the rare one: a
 * realloc that doubles, so it runs a handful of times per document. It stays
 * out of line, and the index path does not pay for it. */
markdown_core_lookahead_entry *markdown_core_parser_lookahead_entry_grow(markdown_core_parser *parser, int index);

static inline markdown_core_lookahead_entry *markdown_core_parser_lookahead_entry(markdown_core_parser *parser,
                                                                                  int line) {
    int index;

    if (parser->lookahead_base_line == 0) {
        parser->lookahead_base_line = line;
    }
    index = line - parser->lookahead_base_line;
    if (index < 0) {
        /* Unreachable by the ordering argument above; a line before the base
         * is matched without the cache rather than through it. */
        return NULL;
    }
    if (index >= parser->lookahead_entries_alloc) {
        return markdown_core_parser_lookahead_entry_grow(parser, index);
    }
    if (parser->lookahead_entries_used <= index) {
        parser->lookahead_entries_used = index + 1;
    }
    return &parser->lookahead_entries[index];
}

/* A NON-CONSUMING LOOKAHEAD over the lines after the one being processed.
 *
 * A block start whose grammar reaches past its own line -- the `%%` block
 * comment, which is a paragraph line unless a closer line follows under the
 * same container prefixes -- decides here before it opens anything, so a
 * candidate that fails consumes nothing and the block parser never rewinds.
 * Each line is offered exactly as the block parser will see it once the block
 * exists: the open containers' prefixes matched by the same matchers
 * `check_open_blocks` runs, through the same parser cursor, with a list item
 * whose first child is the block about to be added accepting a blank line the
 * way it will then. A line that does not carry every prefix, a container's
 * own closing line, or the end of the input ends the lookahead. Blank lines
 * are stepped over and counted, because no block start begins on one.
 *
 * `begin` saves the parser's line state and the chain's flags, `end` restores
 * them; nothing else in the parser or the tree is touched. `begin` returns
 * false only when an allocation failed, and has then marked the parse lost. */
typedef struct {
    markdown_core_parser *parser;
    struct markdown_core_node *parent;
    int depth;
    const unsigned char *cursor;
    int line;
    int run_start;
    bufsize_t saved_offset;
    bufsize_t saved_column;
    bufsize_t saved_first_nonspace;
    bufsize_t saved_first_nonspace_column;
    int saved_indent;
    bool saved_blank;
    bool saved_partially_consumed_tab;
    bool active;
} markdown_core_block_lookahead;

/* Stable source-coordinate ordering, shared by deferred nodes and cell geometry. */
int markdown_core_order_source_entries(void *entries, size_t count, size_t stride, uint64_t (*key)(const void *));

struct markdown_core_block_reader;
/* Query the ordinary block-start rules before the table slot. Paragraph
 * continuation uses its real interruption rules (notably list starts, type-7
 * HTML and indentation); following lines come from the caller's source view. */
bool markdown_core_parser_has_block_start(markdown_core_parser *parser, markdown_core_node *parent,
                                          markdown_core_chunk *input, int first, int column, int indent, bool paragraph,
                                          struct markdown_core_block_reader *reader);

/* Schedule an already owned node's mapped content for the ordinary block
 * parser. No nested parse transaction, document, registry or C recursion. */
void markdown_core_parser_finalize_unmatched_blocks(markdown_core_parser *parser);
bool markdown_core_parser_queue_block_input(markdown_core_parser *parser, markdown_core_node *owner);
/* Project a byte column in the active input to its original source column.
 * Line numbers already name physical source lines. Column zero stays a
 * line-ending sentinel. Producers call this when assigning node scopes. */
int markdown_core_parser_source_column(markdown_core_parser *parser, int line, int column);
int markdown_core_parser_append_source_marks(markdown_core_parser *parser, markdown_core_node *node, int line,
                                             int column, bufsize_t length, bufsize_t offset);

bool markdown_core_parser_lookahead_begin(markdown_core_parser *parser, struct markdown_core_node *parent_container,
                                          markdown_core_node_type child, markdown_core_block_lookahead *lookahead);
/* The next non-blank line that carries the prefixes: its bytes through its
 * line ending, the index of its first non-space byte, its indentation after
 * the prefixes, and the blank lines stepped over before it. Returns 0 when the
 * lookahead has ended. */
int markdown_core_parser_lookahead_next(markdown_core_block_lookahead *lookahead, markdown_core_chunk *line,
                                        int *first_nonspace, int *indent, int *blank_lines);
void markdown_core_parser_lookahead_end(markdown_core_block_lookahead *lookahead);

/* Register an element's committed definition in its borrowed parse index.
 * inline_owner, when present, adopts the detached body into its AST value
 * chain. Otherwise the block tree retains ownership until resolution ends.
 * Allocation failure leaves ownership unchanged and aborts the transaction. */
bool markdown_core_parser_register_definition(markdown_core_parser *parser,
                                              markdown_core_definition_collection *collection,
                                              markdown_core_node *definition, markdown_core_node *citation,
                                              markdown_core_node **inline_owner);

/* The engine has one parse operation. `setup`, when present, configures the
 * fresh parser after the complete dialect is attached, before any source is
 * read. Tests may add instrumentation; no caller selects the language.
 * Returning false aborts the transaction. The
 * parser never escapes this call and is destroyed before it returns. */
typedef bool (*markdown_core_parser_setup_func)(markdown_core_parser *parser, void *context);
markdown_core_node *markdown_core_parse_document_with_setup(const char *source, size_t length,
                                                            markdown_core_parser_setup_func setup, void *context);

#ifdef __cplusplus
}
#endif

int markdown_core_parser_content_span(markdown_core_parser *parser, markdown_core_node *node, bufsize_t from,
                                      bufsize_t to, markdown_core_content_span *span);
void markdown_core_parser_adopt_content_span(markdown_core_node *owner, markdown_core_node *node,
                                             const markdown_core_content_span *span, bufsize_t from);

#endif
