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

/* THE FINISH STEPS, projected by EVENT and KIND.
 *
 * The finish walk delivers two events per node, and a step declares the kinds
 * it is ASKED AT (their EXIT, once the subtree is complete) and the kinds
 * whose EXTENT it tracks (their ENTER and EXIT), so the natural key of the
 * dispatch is (event, kind): a Text's EXIT reaches autolink, a Paragraph's
 * EXIT reaches formula, a Link's ENTER and EXIT reach autolink, and a Text's
 * ENTER or a List's EXIT reach nothing. The projection is one table with a
 * pointer per key to a terminated list of steps in descriptor order, NULL for
 * a key nothing declared, built once per parse beside the block and
 * inline-content families and gated, once the tree is complete, on the kinds
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

/* WHAT THE FINISH WALK ASKS OF A KIND, answered once per parse per kind and
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
    /* Delimiter entries pushed, against which the pool's growth is measured. */
    size_t delimiter_pushes;
    /* Inline placements asked of the content-to-source map, and the runs
     * probed to answer the ones that left the cursor's run. The map's whole
     * claim since #346 is that a placement made while a container is read
     * left to right is measured in the cursor's run and probes nothing, and
     * one that leaves it probes a bounded number of runs rather than
     * searching the container's from scratch; the ratio is that claim. */
    size_t content_mark_queries;
    size_t content_mark_probes;
    /* Ordinary whitespace scalars and contextual-space lookahead bytes. */
    size_t whitespace_work;
    size_t bracket_work;
    /* Elements the inline-content hook dispatch EXAMINED, counted one per
     * element per family per inline-content node. The projection's whole claim
     * is that this grows with the declarers and not with the registry, and
     * nothing else can see the difference: a dispatch that went back to
     * scanning every attached element would build the identical tree. So the
     * invariant is asserted on this counter rather than on output. */
    size_t inline_hook_work;
    /* THE FINISH STAGE'S TRAVERSAL COUNT, in numbers the output cannot show.
     * The stage's whole claim is that it walks each owned root ONCE and runs
     * inline completion, consolidation and every finish step from inside that
     * one walk; a stage that walked a root once per hook would build the
     * identical tree, so the claim is asserted on these rather than on a dump.
     *
     * `finish_walk_events` is every iterator step the finish stage took: the
     * walk's own ENTER, EXIT and DONE events, plus the ENTER and EXIT that
     * consolidation advances over when it absorbs a following Text sibling
     * (those nodes are visited -- by consolidation, which completes and frees
     * them -- and counted as visited). Repositioning the cursor back to the
     * survivor's EXIT is not a step: that event was already delivered.
     * `finish_nodes_entered` is the ENTER events among them, absorbed siblings
     * included; `finish_walk_roots` is the DONE events, one per root walked.
     *
     * The denominator is taken without a traversal, at the two seams where
     * nodes come and go. `nodes_created` counts every node a parse makes, at
     * the same operation that records the node's kind, so the audit that
     * holds one holds the other; `nodes_freed` counts every node a parse
     * releases through `markdown_core_parser_release_node`, which the finish
     * stage's every free takes -- consolidation's, and each step's, which the
     * finish-hook audit holds -- and which counts the descendants and field
     * roots that go with a node, since the release loop visits each of them.
     * The walk notes both in `..._before_finish` as it starts. The finished
     * tree holds every node that existed when the walk started, plus those
     * the walk's inline parsing handed it (`finish_nodes_parsed`, which the
     * walk enters), less those the stage freed, plus those its steps made
     * (which it never enters), so one traversal per root is exactly
     *
     *   finish_nodes_entered == nodes in the finished tree
     *                           + (nodes_freed - nodes_freed_before_finish)
     *                           - (nodes_created - nodes_created_before_finish)
     *                           + finish_nodes_parsed
     *
     * where the finished tree is counted by whoever holds it (the api test
     * walks it with the public iterator and the owned-subtree visitors), and
     * a stage that walked each root k times, counting as the engine's walks
     * count, would enter k times as many. `finish_walk_events == 2 *
     * finish_nodes_entered + finish_walk_roots` then says that every step
     * taken was one of those events. A whole-root consolidation driven through
     * the public entry point with a parser adds exactly one traversal of that
     * root to the events, the entered and the roots.
     *
     * The count sees only the walks that report themselves: the engine's
     * finish walk and that public entry point. A traversal that keeps no count
     * -- an iterator a step opened over its node's subtree -- is invisible
     * here, so the other half of the invariant is held on the source:
     * scripts/audit-finish-hook-shapes.mjs refuses a translation unit that
     * declares a finish step and opens an iterator. */
    size_t nodes_created, nodes_created_before_finish;
    size_t nodes_freed, nodes_freed_before_finish;
    /* The nodes the walk's own inline parsing handed it, at the ENTER of each
     * container it parsed: what the parse made less what it discarded before
     * returning (a bracket's opener text, a token that failed to close), which
     * is why every parse-time release is counted (the kind-record audit holds
     * that). Made after the walk started, so the identity above adds them
     * back. */
    size_t finish_nodes_parsed;
    size_t finish_walk_events;
    size_t finish_nodes_entered;
    size_t finish_walk_roots;
    /* Opener checks of the `%%` comment scanner; and the lines the block-start
     * lookahead visited plus the prefix bytes each visit matched itself, for
     * the linearity gates of both. */
    size_t comment_scan_work;
    size_t block_lookahead_work;
    size_t table_scan_work, table_frontier_peak;
    size_t table_workspace_growth, table_geometry_lines, table_separator_scans;
    /* Properties work: source ranges decoded once at their owning boundary. */
    size_t metadata_decoded_bytes;
    /* Bytes the properties envelope's two search passes examine: the fence
     * pass hands each byte to `memchr` once, and the index pass hands each
     * envelope byte to the LF search and the CR search once each, so the
     * total is bounded by three times the document. Nothing about the output
     * can see how many times a line's geometry was derived, so that bound is
     * asserted on this counter. What the counter proves is exactly that: the
     * searches' bound and, through the bare-CR case, that the LF memo holds.
     * A consumer that re-derived a line with a byte loop of its own would not
     * be counted here; it is kept out by there being no such loop to call. */
    size_t properties_line_work;
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
    /* The one block behind the block families, the inline-content families
     * and the finish steps. */
    void *block_hook_allocation;
    /* Each family's declared gates projected to one list of owners per key
     * (a first non-space byte, no byte, or an indented line), in the family's
     * own order, so a line reads the owners its key names rather than asking
     * every owner (see S_gate_candidates). A NULL table means the family
     * declared nothing and every owner is asked, which is the behaviour a
     * gate replaces. */
    uint8_t *block_gate_lists[MARKDOWN_CORE_BLOCK_HOOK_COUNT];
    /* The inline-content families, projected from the same registry and in the
     * same descriptor order. Zero counts before the projection runs, which is
     * why it runs unconditionally on the one path that creates a parser. */
    const markdown_core_element **inline_hooks[MARKDOWN_CORE_INLINE_HOOK_COUNT];
    size_t inline_hook_counts[MARKDOWN_CORE_INLINE_HOOK_COUNT];
    /* Delimiter entries removed from an inline parse, kept for the next push
     * (see `markdown_core_inline_push_delimiter_entry`); linked through `next`
     * and released with the parser. */
    struct delimiter *free_delimiters;
    /* The finish steps by key (see `markdown_core_finish_key`): each entry
     * points into the same allocation as the block and inline-content
     * families, at a list terminated by a NULL element, or is NULL when
     * nothing declared the key. The lists are projected when the parser is
     * set up; each entry carries its gate (markdown_core_finish_step_entry),
     * which the walk reads at the event. `finish_step_slots` is how many
     * state words the walk keeps per root: one per element that declares a
     * step. `finish_kinds` is the walk's per-kind record, by kind index
     * (markdown_core_finish_kind), projected beside the lists. */
    markdown_core_finish_step_entry *finish_dispatch[MARKDOWN_CORE_FINISH_KEY_COUNT];
    size_t finish_step_slots;
    markdown_core_finish_kind finish_kinds[MARKDOWN_CORE_FINISH_KIND_COUNT + 1];
    /* WHICH KINDS THIS PARSE PRODUCED, recorded where they are produced.
     *
     * Every node creation and every `set_kind` that a parse performs writes
     * here, so the gate on a finish hook is read from a record rather than
     * gathered by a walk: a pass is selected once the finish walk -- which
     * parses the inline content -- has completed, and a step reads it at
     * each event it is asked at (markdown_core_finish_step_entry). Gathering
     * it by a walk instead is what forced the finish stage to traverse the
     * document twice. See the gate in `S_finish_parse` for what the set
     * over-approximates and why that is sound.
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

/* THE RUN AN OFFSET LIES IN, FOUND FROM WHERE THE LAST ONE WAS.
 *
 * A node's runs are contiguous in the parser's vector and ordered by content
 * offset, so the run containing an offset is the last whose start is at or
 * before it. The inline parser asks this once per node it places and reads
 * its container left to right, so the answer is almost always the run the
 * previous answer named or the one after it: a Text never crosses a line
 * ending (a break is its own node), and the next token starts where the last
 * one ended. Those two runs are probed first. Only an offset farther ahead --
 * an opaque span across many lines -- or behind the hint -- a Link placed
 * back at its opener, a rewind -- is searched for, over the part of the run
 * on that side, so no query costs more than the search alone did.
 *
 * Defined here, with the span below, so that placing an inline node is one
 * straight-line body in the caller: the placement's cost is its fixed part,
 * not its probes, and a call for each of three steps was most of it. */
static MARKDOWN_CORE_INLINE int markdown_core_block_content_mark_near(markdown_core_parser *parser,
                                                                      const markdown_core_node *node, bufsize_t offset,
                                                                      int hint) {
    const markdown_core_line_mark *marks = parser->line_marks;
    int lo = node->content_mark, hi = lo + node->content_mark_count - 1;
    int at = hint >= lo && hint <= hi ? hint : lo;
    size_t probes = 1;
    if (marks[at].content_offset <= offset) {
        if (at != hi && marks[at + 1].content_offset <= offset) {
            probes++;
            at++;
            if (at != hi && marks[at + 1].content_offset <= offset) {
                lo = at + 1;
                at = -1;
            }
        }
    } else {
        hi = at - 1;
        at = -1;
    }
    if (at < 0) {
        while (lo < hi) {
            int mid = lo + (hi - lo + 1) / 2;
            probes++;
            if (marks[mid].content_offset <= offset) {
                lo = mid;
            } else {
                hi = mid - 1;
            }
        }
        at = lo;
    }
    parser->content_mark_probes += probes;
    return at;
}

/* The same question with no hint: the plain search, for the block phase and
 * the map copies, which ask it once per node or per run rather than once per
 * token and carry no cursor. Kept apart from `near` so that a block-phase
 * caller inlines a search and not the probe loop and its accounting, which
 * are the placement's. */
static MARKDOWN_CORE_INLINE int markdown_core_block_content_mark_at(markdown_core_parser *parser,
                                                                    const markdown_core_node *node, bufsize_t offset) {
    const markdown_core_line_mark *marks = parser->line_marks;
    int lo = node->content_mark, hi = lo + node->content_mark_count - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (marks[mid].content_offset <= offset) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

/* Resolve both ends of [from, to] against `node`'s map, each found from the
 * run before it: `cursor`, when given, is the run the caller last resolved
 * and is left on the run that answers `to`; a cursor outside the node's run,
 * a zeroed one included, is a hint that is simply not taken.
 * Returns 0 when the node has no map at all, in which case neither end is
 * resolved. The caller owns what it does with the answer: the run indices are
 * handed back rather than written onto a node, because whether a slice is
 * taken at all is a decision only the caller can make -- writing
 * `content_mark_count` on a node that is not a verbatim copy of its source
 * would give every SPAN, LINK and EMPHASIS node a map it does not have, and
 * three places read that count as the question "is there a mapping". */
static MARKDOWN_CORE_INLINE int markdown_core_parser_content_span(markdown_core_parser *parser,
                                                                  markdown_core_node *node, bufsize_t from,
                                                                  bufsize_t to, markdown_core_content_span *span,
                                                                  int *cursor) {
    span->has_start = false;
    span->has_end = false;
    span->first = span->last = 0;
    if (!parser || !node || node->content_mark_count <= 0) {
        return 0;
    }
    int hint = cursor ? *cursor : node->content_mark;
    if (from >= 0) {
        bufsize_t offset = from + node->content_mark_offset;
        span->first = markdown_core_block_content_mark_near(parser, node, offset, hint);
        const markdown_core_line_mark *mark = &parser->line_marks[span->first];
        span->start_line = mark->line;
        span->start_column = mark->column + (int)(offset - mark->content_offset) * mark->source_step;
        span->has_start = true;
        hint = span->first;
    }
    if (to >= 0) {
        bufsize_t offset = to + node->content_mark_offset;
        span->last = markdown_core_block_content_mark_near(parser, node, offset, hint);
        hint = span->last;
        const markdown_core_line_mark *mark = &parser->line_marks[span->last];
        span->end_line = mark->line;
        span->end_column =
            mark->column + (int)(offset - mark->content_offset) * mark->source_step + mark->source_width - 1;
        span->has_end = true;
    }
    if (cursor) {
        *cursor = hint;
    }
    return 1;
}

/* Take the slice a resolved span already names. `from` is the span's own
 * start offset, which the runs were resolved against. */
static MARKDOWN_CORE_INLINE void markdown_core_parser_adopt_content_span(markdown_core_node *owner,
                                                                         markdown_core_node *node,
                                                                         const markdown_core_content_span *span,
                                                                         bufsize_t from) {
    node->content_mark = span->first;
    node->content_mark_count = span->last - span->first + 1;
    node->content_mark_offset = from + owner->content_mark_offset;
}

/* THE PARSE'S NODE OPERATIONS, WHICH RECORD THE KIND THEY PRODUCE.
 *
 * `kinds_created` decides which finish hooks run -- a global pass, a step at
 * every event it was projected to -- so a production site that writes a kind
 * without recording it does not fail a build or a test: it makes the gate skip
 * a hook some document needed, and the defect surfaces
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

/* A creation records the kind and counts the node: the count is the finish
 * stage's denominator (the traversal counters above). */
static inline void markdown_core_parser_note_node(markdown_core_parser *parser, markdown_core_node_type kind) {
    if (parser) {
        markdown_core_node_kind_set_add(&parser->kinds_created, kind);
        parser->nodes_created++;
    }
}

/* A release counts what it freed, for the same denominator; a caller with no
 * parse frees as the public function does. */
static inline void markdown_core_parser_release_node(markdown_core_parser *parser, markdown_core_node *node) {
    size_t released = markdown_core_node_release(node);
    if (parser) {
        parser->nodes_freed += released;
    }
}

/* Whether the walk asks `entry`'s step at the event it is projected to: its
 * gate, read against the kinds the parse has produced so far. */
static inline bool markdown_core_finish_step_admitted(const markdown_core_finish_step_entry *entry,
                                                      const markdown_core_parser *parser) {
    return !entry->gated || (entry->acts_on.blocks & parser->kinds_created.blocks) ||
           (entry->acts_on.inlines & parser->kinds_created.inlines);
}

static inline markdown_core_node *markdown_core_parser_make_node(markdown_core_parser *parser,
                                                                 markdown_core_node_type type) {
    markdown_core_parser_note_node(parser, type);
    return markdown_core_node_new(type);
}

static inline markdown_core_node *markdown_core_parser_make_node_with_ext(markdown_core_parser *parser,
                                                                          markdown_core_node_type type,
                                                                          const markdown_core_element *element) {
    markdown_core_parser_note_node(parser, type);
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

#endif
