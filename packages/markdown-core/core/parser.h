#ifndef MARKDOWN_CORE_PARSER_H
#define MARKDOWN_CORE_PARSER_H

#include <stdint.h>
#include "diagnostics.h"
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
    /* The first and the last definition registered from an inline root, the
     * ends of the chain their owner's value field holds: the engine walks it
     * from `first_inline` without reading that field, which is the owning
     * element's payload. */
    struct markdown_core_node *first_inline, *last_inline;
    /* The last inline-registered definition an inline root's completion walk
     * covered: the walk after the next root starts at its successor. */
    struct markdown_core_node *last_completed;
} markdown_core_definition_collection;

/* One byte may terminate text and/or dispatch to this owner. The roles are
 * independent; the common index preserves descriptor precedence for both. */
typedef struct {
    const markdown_core_element *element;
    bool dispatches, terminates;
} markdown_core_inline_candidate;

/* The rows of the block owner sets that are indexed by a line's first byte.
 * The rows after them are indexed by its indent; see below. */
#define MARKDOWN_CORE_BLOCK_OWNER_BYTES 256

/* The block owners of one first byte, as a bit per owner in registry order
 * (`markdown_core_registry.block_owners`): the owners with the hook that
 * accept lines starting at the byte, so a block-start arbitration visits
 * those and no other. An owner that declared no byte set is in every byte's
 * set. Each set is row-major, `words` 64-bit words per row -- as many as
 * the registry's owners need, one for up to 64 of them -- so owner `n` is
 * bit `n % 64` of word `n / 64` of its row.
 *
 * After the 256 byte rows come the indent rows, one per distinct indent any
 * owner declared (`block_start_indent`), which is how a block that begins at
 * an indent rather than at a byte declares itself. `indent_thresholds` holds
 * those indents in ascending order, `rows - MARKDOWN_CORE_BLOCK_OWNER_BYTES`
 * of them, and row `MARKDOWN_CORE_BLOCK_OWNER_BYTES + g` holds every owner
 * whose declared indent the line has reached by `indent_thresholds[g]` -- so
 * the rows accumulate, and a line reads exactly one of them: the last whose
 * threshold it reaches. An arbitration ors that row with the byte's, and a
 * line that reaches none reads the byte's row twice, so the shape does not
 * change with the line and no owner is visited below the indent it
 * declared. */
typedef struct {
    const uint64_t *scan, *interrupt, *open, *paragraph; /* `rows` * `words` entries each */
    size_t words, rows;
    const int *indent_thresholds;
} markdown_core_block_owner_sets;

/* The elements that implement an inline root's `init_inline`, then
 * `finish_inline`, then `dispose_inline`, each list in registry order and the
 * three stored back to back. */
typedef struct {
    const markdown_core_element *const *elements;
    size_t init_count, finish_count, dispose_count;
} markdown_core_inline_hooks;

/* The projection of one element registry: everything a parse dispatches on
 * that the registry alone determines, so nothing of it is rebuilt per parse.
 * The core registry's projection is a constant prepared with the elements
 * (see core-registry.inc and registry_runner); a registry a private setup
 * extends gets its own, built by markdown_core_registry_prepare in one
 * allocation the parser owns. */
typedef struct markdown_core_registry {
    const markdown_core_element *const *elements;
    size_t element_count;
    /* The element whose `parse_text` makes plain text, and the owner of each
     * delimiter rule. */
    const markdown_core_element *text_structure;
    const markdown_core_element *delimiter_owners[MARKDOWN_CORE_DELIM_RULE_COUNT];
    /* Stable descriptor order projected by byte: the candidates of byte `c`
     * are inline_dispatch[offsets[c] .. offsets[c + 1]), so each token visits
     * only its possible owners. 257 offsets, the last an end sentinel. */
    const size_t *inline_dispatch_offsets;
    const markdown_core_inline_candidate *inline_dispatch;
    /* The elements with block hooks in registry order, and by first byte
     * the ones each arbitration loop visits (markdown_core_block_owner_sets). */
    const markdown_core_element *const *block_owners;
    size_t block_owner_count;
    markdown_core_block_owner_sets block_owner_sets;
    /* Implementers of the inline root lifecycle hooks, so a root visits
     * implementers only. */
    markdown_core_inline_hooks inline_hooks;
    /* Some element delivers complete_inline without asking for it per root,
     * so every inline root of a document is walked for completion. */
    bool inline_completion_walk;
    /* 256 entries each: the bytes that end a text run, and the bytes
     * delimiter flanking looks through. */
    const int8_t *special_chars;
    const int8_t *skip_chars;
    /* The one allocation an owned projection lives in; NULL for a constant. */
    void *storage;
} markdown_core_registry;

/* Project `elements` (with `extra` appended when not NULL) into one owned
 * allocation. Returns false on allocation failure, leaving `registry` as it
 * was; success replaces every field. */
bool markdown_core_registry_prepare(markdown_core_mem *mem, const markdown_core_element *const *elements, size_t count,
                                    const markdown_core_element *extra, markdown_core_registry *registry);
void markdown_core_registry_release(markdown_core_mem *mem, markdown_core_registry *registry);

/* First nonblank line under one prospective block parent. Shared by block
 * owners during a single block-start arbitration; no speculative state or
 * owned storage survives here. Reset before the parser mutates that context. */
typedef struct {
    struct markdown_core_node *parent;
    markdown_core_chunk input;
    int first, indent, blanks;
    bool available;
} markdown_core_block_peek;

/* The line that opened the paragraph opened last. A grammar that can only
 * decide at its second line what its first line was -- a simple table's
 * header, a definition's term -- reads that first line back from here when
 * the second one arrives, instead of every paragraph looking ahead at its
 * own start. The bytes are the source's own, or the parser's copy when the
 * source line was rewritten (line_scratch); `after` is where the next raw
 * line begins, as a lookahead would have reported it. */
typedef struct {
    struct markdown_core_node *node;
    const unsigned char *data;
    bufsize_t length;
    int offset, first, first_column, indent, line;
    const unsigned char *after;
} markdown_core_paragraph_line;

struct markdown_core_parser {
    struct markdown_core_mem *mem;
    /* The transaction's storage. Created with the parser, handed to the root
     * document with the tree, and released by whichever of the two ends up
     * owning the root. */
    markdown_core_arena *arena;
    /* Source-ordered reference declarations, indexed by normalized label. */
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
    const markdown_core_element *document_structure;
    /* The root node of the parser, always a MARKDOWN_CORE_NODE_DOCUMENT */
    struct markdown_core_node *root;
    /* The active block grammar boundary. The document and mapped cell inputs
     * share this parser and all document registries. Input roots stay owned by
     * the AST; the queue only borrows them until their block content is read. */
    struct markdown_core_node *block_root;
    struct markdown_core_node *matched_container;
    struct markdown_core_node **block_inputs;
    size_t block_input_count, block_input_capacity, block_input_cursor;
    /* Finalized blocks their element completes once block parsing ends, in
     * the order they were finalized: a block after every block it contains. */
    struct markdown_core_node **completions;
    size_t completion_count, completion_capacity;
    /* Completion requests of inline roots parsed within another root's
     * parse (a field root entered from the inline parser), which the
     * enclosing root collects; and how deep that nesting currently is. */
    unsigned nested_completion_requests, nested_inline_depth;
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
    /* A dash separator rejected at this byte also rejects every earlier
     * suffix on the current physical line. Reset with the line cursor. */
    bufsize_t table_separator_kill_pos;
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
    /* Scratch a link destination or title is cleaned into before its bytes
     * are copied into the arena: one buffer per parser, not one per link. */
    markdown_core_strbuf url_scratch;
    /* Options set by the user, see the Options section in markdown_core.h */
    /* Sticky allocation-failure flag: once any parse structure is lost, the
     * one-shot transaction reports the whole parse as failed (NULL) instead of
     * returning a silently truncated document. */
    bool oom;
#if MARKDOWN_CORE_DIAGNOSTICS
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
    /* Run comparisons in content-to-source projection, including cursor advances. */
    size_t content_map_work;
    /* Branch tests and searches of key indexes the parser already released;
     * live indexes report their own counters. */
    size_t key_index_work, key_index_operations;
    size_t bracket_work;
    /* Opener checks of the `%%` comment scanner; and the lines the block-start
     * lookahead visited plus the prefix bytes each visit matched itself, for
     * the linearity gates of both. */
    size_t comment_scan_work;
    /* HTML scanner runs at a `<` -- tag, comment, CDATA, declaration or
     * instruction -- in the inline scan and the citation brace prescan
     * alike. An unclosed form runs once per inline root. */
    size_t html_scan_work;
    size_t block_lookahead_work;
    /* Block hook invocations: one per owner consulted for a line. */
    /* Block owners visited by a line's block-start arbitration: the owners
     * of the line's first byte with the hook, and no other. */
    size_t block_dispatch_work;
    /* Indent thresholds probed to find a line's indent row: the projection
     * keeps them sorted, so this is a comparison per doubling of the
     * declared indents, not one per threshold. */
    size_t block_indent_probe_work;
    /* Reference definition parses attempted on a block front or a term. */
    size_t reference_probe_work;
    /* Hook deliveries of the inline completion walk and of the node finishing
     * walk: one per node each, however many elements are attached. */
    size_t completion_work, finishing_work;
    /* Finish hook calls: one per node offered to an element's hook, which
     * is one per node of a kind the element declared. */
    size_t finisher_work;
    /* Lifecycle hook calls made for inline roots: implementers only. */
    size_t inline_lifecycle_work;
    /* Literal runs grown in place instead of split, and body bytes a
     * fenced code block relocated at close (none: its info string is read
     * at the fence). */
    size_t text_run_extensions, code_block_move_work;
    size_t table_scan_work, table_frontier_peak;
    size_t table_workspace_growth, table_geometry_lines, table_separator_scans;
    /* Pipe row recognitions and the bytes they read; column-map allocations;
     * growth steps of the table scratch region. */
    size_t table_row_scans, table_row_work, table_geometry_allocations, table_scratch_growth;
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
#endif
    /* THE SOURCE AFTER THE LINE BEING PROCESSED. `S_parse_source` sets the
     * cursor to the first byte of the next raw line before it hands each line
     * to `S_process_line`, so a block start whose grammar needs a later line --
     * the `%%` block comment's closer -- can look ahead without consuming
     * anything (see markdown_core_parser_lookahead_begin). NULL until the
     * first line is processed; `cursor == end` once the input has run out. */
    const unsigned char *lookahead_cursor;
    const unsigned char *lookahead_end;
    /* The finishing-phase clock a setup hook installed, or NULL. */
    struct markdown_core_phase_clock *phase_clock;
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
    markdown_core_block_peek block_peek;
    markdown_core_paragraph_line paragraph_line;
    markdown_core_strbuf paragraph_line_copy;
    /* The raw source bytes of the line being processed, through its
     * terminator, or NULL for a line the parser rewrote. */
    const unsigned char *line_source;
    bufsize_t line_source_length;
    /* One active table query borrows this reusable line workspace, and
     * carves its lines' column maps and separator intervals from the two
     * regions below, which every query starts empty (table.c). The
     * allocations die with the parser. */
    struct markdown_core_table_source_line *table_lines;
    size_t table_lines_capacity;
    int *table_columns;
    size_t table_columns_capacity, table_columns_used;
    struct markdown_core_table_interval *table_dashes;
    size_t table_dashes_capacity, table_dashes_used;
    /* The pipe row geometry recognized on the current line, kept from the
     * open table's matcher for the row opener that follows on the same line;
     * and one scratch region every table search carves its arrays from,
     * sized once to the peak a candidate asked for (see table.c). */
    struct markdown_core_table_row_geometry *table_row;
    void *table_scratch;
    size_t table_scratch_capacity;
    /* The registry this parse dispatches on and its projection: the fixed
     * immutable dialect's constant, borrowed, until a private setup caller
     * extends it, from when on it is `owned_registry`. `elements` and
     * `element_count` mirror the registry's for the phases that walk it.
     * Every parser reads its own registry, so concurrent parsers with
     * different element sets never observe each other's projections. */
    const markdown_core_registry *registry;
    markdown_core_registry owned_registry;
    const markdown_core_element *const *elements;
    size_t element_count;
    markdown_core_ispunct_func backslash_ispunct;
    /* The content-to-source map (see markdown_core_line_mark). It is read while the
     * parse is still running -- the block phase reads it as blocks close and
     * the inline phase reads it before the transaction returns -- and it is
     * released with the rest of the per-parse state. */
    markdown_core_line_mark *line_marks;
    bufsize_t line_marks_size;
    bufsize_t line_marks_alloc;
};

/* Release a parser-owned key index and keep its deterministic search
 * accounting with the parser; product builds only free it. */
static MARKDOWN_CORE_INLINE void markdown_core_parser_release_key_index(struct markdown_core_parser *parser,
                                                                        markdown_core_key_index *index) {
    MARKDOWN_CORE_DIAGNOSTIC(parser->key_index_work += index->branch_visits;
                             parser->key_index_operations += index->operations;)
    (void)parser;
    markdown_core_key_index_free(index);
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
    /* Prefix cursor plus the already scanned indentation suffix. Retain both
     * so deeper candidates do not rescan the same leading whitespace. */
    bufsize_t offset;
    bufsize_t column;
    bufsize_t first_nonspace;
    bufsize_t first_nonspace_column;
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

/* Project an endpoint and return its immutable run index, or -1 without a map.
 * An optional per-owner cursor advances only; earlier endpoints use binary search. */
int markdown_core_parser_project_content(markdown_core_parser *parser, const markdown_core_node *node, bufsize_t offset,
                                         bool end, int *cursor, int *line, int *column);

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
/* Release storage that may never have been allocated: the allocator is
 * handed only what it handed out, never a null pointer for a field that
 * stayed empty. */
static MARKDOWN_CORE_INLINE void markdown_core_mem_release(markdown_core_mem *mem, void *pointer) {
    if (pointer) {
        mem->free(pointer);
    }
}
/* The inline completion walk over one root: complete_inline and
 * observe_inline for every node of the root's tree and owned fields. */
void markdown_core_block_complete_inline_root(markdown_core_parser *parser, markdown_core_node *root);
/* Queue a finalized block for completion once block parsing ends; a block
 * finalized outside markdown_core_block_finalize (a table's lead paragraph)
 * queues itself where it is finalized. */
void markdown_core_block_queue_completion(markdown_core_parser *parser, markdown_core_node *b);
/* The completion walk a root asked for, run as its parse ends
 * (markdown_core_inline_finish_inlines and the heading's own loop). */
void markdown_core_inline_complete_root(markdown_core_parser *parser, markdown_core_inline_state *inline_state);
/* Project a byte column in the active input to its original source column.
 * Line numbers already name physical source lines. Column zero stays a
 * line-ending sentinel. Producers call this when assigning node scopes. */
int markdown_core_parser_source_column(markdown_core_parser *parser, int line, int column);
int markdown_core_parser_append_source_marks(markdown_core_parser *parser, markdown_core_node *node, int line,
                                             int column, bufsize_t length, bufsize_t offset);

/* The borrowed result lasts until the next peek or block-start context.
 * Uses exactly the same container matching and blank rules as lookahead. */
const markdown_core_block_peek *markdown_core_parser_peek_block_line(markdown_core_parser *parser,
                                                                     struct markdown_core_node *parent,
                                                                     markdown_core_node_type child);
/* Records the line that opened `paragraph` (markdown_core_paragraph_line). */
void markdown_core_parser_note_paragraph_line(markdown_core_parser *parser, markdown_core_node *paragraph,
                                              const markdown_core_chunk *input);
/* The line that opened `paragraph`, while it is the paragraph opened last
 * and still that one line: open on the line before the current one, or
 * closed on the line it opened. NULL otherwise. */
const markdown_core_paragraph_line *markdown_core_parser_paragraph_line(const markdown_core_parser *parser,
                                                                        const markdown_core_node *paragraph);

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
/* Readings of a caller's clock at the sequence points of the finishing
 * phases, for a phase breakdown: the block pass is complete at `blocks`,
 * the document is prepared at `prepared`, inline trees are built at
 * `inlines`, and the tree is finished and consolidated at `finished`.
 * A setup hook installs it; a product parse leaves it NULL and pays one
 * test per sequence point. */
typedef struct markdown_core_phase_clock {
    uint64_t (*now)(void *context);
    void *context;
    uint64_t blocks, prepared, inlines, finished;
} markdown_core_phase_clock;

typedef bool (*markdown_core_parser_setup_func)(markdown_core_parser *parser, void *context);
markdown_core_node *markdown_core_parse_document_with_mem(const char *source, size_t length, markdown_core_mem *mem,
                                                          markdown_core_parser_setup_func setup, void *context);

#ifdef __cplusplus
}
#endif

#endif
