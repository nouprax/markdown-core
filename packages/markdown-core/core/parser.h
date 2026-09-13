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
    struct markdown_core_mem *mem;
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
markdown_core_lookahead_entry *markdown_core_parser_lookahead_entry(markdown_core_parser *parser, int line);

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
int markdown_core_order_source_entries(markdown_core_mem *mem, void *entries, size_t count, size_t stride,
                                       uint64_t (*key)(const void *));

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
markdown_core_node *markdown_core_parse_document_with_mem(const char *source, size_t length, markdown_core_mem *mem,
                                                          markdown_core_parser_setup_func setup, void *context);

#ifdef __cplusplus
}
#endif

#endif
