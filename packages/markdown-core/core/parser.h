#ifndef MARKDOWN_CORE_PARSER_H
#define MARKDOWN_CORE_PARSER_H

#include <stdint.h>
#include <stdio.h>
#include "references.h"
#include "node.h"
#include "buffer.h"
#include "delimiter.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_LINK_LABEL_LENGTH 1000

struct markdown_core_parser {
    struct markdown_core_mem *mem;
    /* A hashtable of urls in the current document for cross-references */
    struct markdown_core_map *refmap;
    /* Which footnote labels the document defines. Block parsing registers a
     * definition the moment its container opens; the inline phase, which runs
     * only once every block is closed, asks whether `[^x]` names one. A
     * reference to a label nobody defines is not a footnote at all — it is the
     * literal text the author typed — so this answer decides a node's type,
     * and it has to be the whole document's answer: see
     * markdown_core_footnote_definition_create for why it is a second map
     * rather than a discriminated column of `refmap`. */
    struct markdown_core_map *footnote_defs;
    /* The root node of the parser, always a MARKDOWN_CORE_NODE_DOCUMENT */
    struct markdown_core_node *root;
    /* The last open block after a line is fully processed */
    struct markdown_core_node *current;
    /* See the documentation for markdown_core_parser_get_line_number() in
     * markdown-core-extension-api.h */
    int line_number;
    /* See the documentation for markdown_core_parser_get_offset() in
     * markdown-core-extension-api.h */
    markdown_core_bufsize offset;
    /* Tab-expanded column of the parse position in the current line; one tab
     * advances it to the next multiple of the tab stop, so it can run ahead
     * of `offset`. */
    markdown_core_bufsize column;
    /* See the documentation for markdown_core_parser_get_first_nonspace() in
     * markdown-core-extension-api.h */
    markdown_core_bufsize first_nonspace;
    /* Tab-expanded column of the byte at `first_nonspace`. */
    markdown_core_bufsize first_nonspace_column;
    markdown_core_bufsize thematic_break_kill_pos;
    /* See the documentation for markdown_core_parser_get_indent() in
     * markdown-core-extension-api.h */
    int indent;
    /* See the documentation for markdown_core_parser_is_blank() in
     * markdown-core-extension-api.h */
    bool blank;
    /* The parse position sits inside a tab: `offset` has not passed the tab
     * byte, but `column` has consumed part of its width. */
    bool partially_consumed_tab;
    /* Contains the currently processed line */
    markdown_core_strbuf curline;
    /* Byte length of the most recently finished line excluding its line
     * ending; closing blocks stamp their end column from it. */
    markdown_core_bufsize last_line_length;
    /* Accumulates partial feed chunks until a complete line is available;
     * curline holds the normalized line currently being parsed. */
    markdown_core_strbuf linebuf;
    /* Options set by the user, see the Options section in markdown_core.h */
    int options;
    /* Sticky allocation-failure flag: once any parse structure is lost,
     * markdown_core_parser_finish reports the whole parse as failed (NULL)
     * instead of returning a silently truncated document. */
    bool oom;
    /* A concrete marker capture (markdown_core_parser_capture_marker) lost
     * its allocation on the line being processed. Deferred rather than
     * folded into `oom` immediately: the oom guard between open_new_blocks and
     * add_text_to_container cuts a line short, and several capture sites
     * sit where that skip would strand parser->current on a block the line
     * already finalized or leave a fenced block without its info line. The
     * line completes with consistent structure and S_process_line folds
     * this into `oom` at the line boundary. */
    bool capture_lost;
    /* Sticky engine-invariant failure. This is separate from allocation loss
     * so facade callers can report MARKDOWN_CORE_ERROR_INTERNAL rather than
     * misclassifying a broken refinement lifecycle as OOM. */
    bool internal_error;
    bool last_buffer_ended_with_cr;
    /* Set by the first feed that delivers any bytes (a zero-length feed does
     * not count); markdown_core_parser_attach_extension refuses new syntax
     * from then on, so the grammar is fixed before parsing begins. */
    bool feed_started;
    markdown_core_llist *extensions;
    /* Immutable for each inline pass and parser-local. It owns compiled
     * trigger buckets and delimiter rule bindings for the attached syntax
     * set, so parsing never scans the extension list to recover an owner. */
    struct markdown_core_inline_config *inline_config;
    /* Parser-lifetime inline scratch. Every inline unit begins at the empty
     * mark and retains lane/record capacity for the next unit. */
    markdown_core_delimiter_engine inline_delimiters;
    /* Where each appended line of the open paragraph came from, so a link
     * reference definition harvested out of that paragraph's accumulated
     * content at finalize can be given the position it was written at.
     *
     * A paragraph is a leaf, so at most one is ever open and one array on the
     * parser suffices — no node grows a field. It is not an asymptotic cost:
     * three ints per line describing a buffer that already holds the line. */
    /* Diagnostics raised while parsing, in source order. Kept as plain
     * ints because core cannot see the facade's types; the document
     * converts them when it takes the tree. One code exists today
     * (a directive's attribute block that did not parse), and the vector
     * stays empty for every document that has none. */
    struct markdown_core_parser_diagnostic {
        int code;
        int start_line;
        int start_column;
        int end_line;
        int end_column;
    } *diagnostics;
    size_t diagnostic_count;
    size_t diagnostic_capacity;
    struct markdown_core_line_mark {
        markdown_core_bufsize content_offset;
        int line;
        int column;
        /* The byte offset in the normalized line where the append began —
         * `column` tab-expands, concrete records do not. The table
         * extension's look-back header capture maps buffer offsets to
         * normalized-line record columns through this. */
        markdown_core_bufsize byte_offset;
        /* Stand-in spaces the append wrote before the line's own bytes: a
         * lazy continuation whose matched prefixes stopped inside a tab
         * buffers the tab's remaining columns as spaces, so the buffer
         * holds `pad` spaces where the normalized line holds the one tab
         * byte at `byte_offset`. Zero everywhere else — the non-lazy
         * paragraph paths advance byte-wise to first_nonspace, which
         * clears the partial-tab state before the append. */
        int pad;
    } *line_marks;
    size_t line_mark_count;
    size_t line_mark_capacity;
};

/** One open block as it was before a close touched it. `last_child` is the
 * youngest child it had, so anything the close appended can be found without
 * asking the close to have recorded it; after a retract, `retired` is that
 * run — detached, kept alive, and read by nothing but the identity handover
 * of the next publish, which then frees it. */
typedef struct markdown_core_warm_open_block {
    markdown_core_node *node;
    markdown_core_node *last_child;
    markdown_core_node *retired;
    /* THE FACADE'S, carried here because the spine is its index: the fold
     * of this block's own fields and of every child BEFORE `last_child` —
     * all settled, all the same objects with the same hashes from now on —
     * so the block is restamped from here in the size of what grew. The
     * engine writes nothing to it. */
    uint64_t prefix_hash;
    uint16_t type;
    uint16_t flags;
    int end_line;
    int end_column;
    markdown_core_bufsize content_size;
} markdown_core_warm_open_block;

/** What a projection took, so it can be given back — and, once given back,
 * what it left behind for the next projection to inherit from.
 *
 * The open spine root-down as it was before the close, plus everything on
 * the parser that outlives a line: marks, held bytes, line counters, the
 * current block, the CR seam, and how many diagnostics there were. Owned by
 * the caller from the publish that returns it until
 * markdown_core_parser_warm_undo_free. Its life has two states: PUBLISHED
 * (the parser is closed, the record says how to reopen it) and RETRACTED
 * (the parser is open again, and `spine[i].retired` holds what the close had
 * appended under each block — the frontier a caller pairs identities from). */
struct markdown_core_warm_undo {
    markdown_core_mem *mem;
    markdown_core_warm_open_block *spine;
    size_t spine_count;
    struct markdown_core_line_mark *marks;
    size_t mark_count;
    unsigned char *held;
    markdown_core_bufsize held_size;
    int line_number;
    int last_line_length;
    markdown_core_node *current;
    bool last_buffer_ended_with_cr;
    size_t diagnostic_count;
    /* The close replaced a spine block with another object (a refine that
     * promotes what it refines), which no retract can put back: the record
     * describes a projection that can be read but not reopened. */
    bool final;
    bool retracted;
};
typedef struct markdown_core_warm_undo markdown_core_warm_undo;

/** Whether the parser's open state at end of feed is one a publish can be
 * retracted from — the prose predicate: the open spine is the document, or
 * the document over one paragraph or heading, and the held line and that
 * paragraph's content are lines that can only continue or open a paragraph
 * or be blank (see the predicate's comment in blocks.c for the byte sets
 * and why). A
 * fresh build asks this at EOF to decide whether to publish or to close for
 * good; there is no chunk yet, so only the state is judged. */
bool markdown_core_parser_warm_eligible_at_eof(const markdown_core_parser *parser);

/** The same predicate for a tick: the open state is read off the record of
 * the previous publish — which must be PUBLISHED, not final and not yet
 * retracted — and every line the arriving chunk begins or continues is
 * judged too. A pure probe: nothing is retracted, fed or written, so a
 * caller that gets `false` still holds exactly the projection it had. */
bool markdown_core_parser_warm_eligible(
    const markdown_core_parser *parser,
    const markdown_core_warm_undo *published,
    const unsigned char *chunk,
    size_t length
);

/** SETTLES what a step closed: refines, once and for good, every unit that
 * is closed and lies in the region a record describes — for each saved open
 * block, deepest first, the children appended past its saved youngest child
 * (whole subtrees, children before their container), then the block itself
 * if it is closed now. Open nodes are descended into and left alone; the
 * document root is never refined. With `before == NULL` the region is the
 * whole closed part of the tree: the form for a fresh parser, whose every
 * closed unit is unrefined.
 *
 * A warm tick calls this after the feed with the record it retracted, so
 * exactly the units the feed closed are refined and keep their inline
 * children — and therefore their identities — from then on. Answers false
 * when a spine block was replaced by its own refine; the entry is repointed
 * at the survivor so nothing dangles, and the caller decides what that
 * means (under the eligibility predicate it cannot happen). */
bool markdown_core_parser_warm_settle(markdown_core_parser *parser, markdown_core_warm_undo *before);

/** PUBLISHES a projection from a parser that is still mid-stream: the held
 * partial line is processed for real, every open block is finalized up to
 * the root, and every unit THAT CLOSE closed — the spine, and whatever the
 * held line put under it — is refined. Units the feed closed are the
 * caller's to settle (markdown_core_parser_warm_settle) and are not looked
 * at, which is what keeps a publish O(open spine + held line) rather than
 * O(tree). What comes back is the record of what the close took — pass it
 * to markdown_core_parser_warm_retract to put the parser back exactly as it
 * was, so the next chunk continues as if the projection had never been
 * asked for.
 *
 * Returns NULL if the record cannot be allocated, in which case nothing was
 * closed and the parser is untouched. A record that comes back `final`
 * describes a projection whose close replaced a spine block; it can be
 * read and freed, not retracted.
 *
 * WHAT MAKES A RECORD RETRACTABLE is the eligibility predicate: only a close
 * whose effects stay inside the record (see the predicate's comment in
 * blocks.c) can be undone, and callers publish only from states that pass
 * it, or accept a projection they will not reopen. */
markdown_core_warm_undo *markdown_core_parser_warm_publish(markdown_core_parser *parser);

/** Gives back everything the publish took: the blocks it closed are reopened
 * with their end coordinates and content restored, the line counters, the
 * marks, the diagnostics count and the held partial line return to what
 * they were, and what the close had appended under each open block — the
 * spine leaf's tentative inline children included — is DETACHED and kept on
 * the record as `spine[i].retired`, not freed: the next publish hands its
 * identities to whatever takes its place, and frees it then — so first the
 * frontier is made to own its bytes, which its literals borrow from the leaf's
 * content buffer that the next feed will grow. The parser is then fed exactly
 * as if it had never been published from. Returns false, touching nothing,
 * for a record that is final or already retracted, and when the frontier
 * could not be given its bytes; the record is still the published one. */
bool markdown_core_parser_warm_retract(markdown_core_parser *parser, markdown_core_warm_undo *undo);

/** Frees a record, and with it any retired frontier it still holds. */
void markdown_core_parser_warm_undo_free(markdown_core_warm_undo *undo);

/** Refines ONE closed unit: its inlines if it owns any, then its own
 * block-local postprocess. Refining each unit once, as it closes, is what
 * lets settled nodes keep being the same nodes — re-parsing a settled unit
 * would retire and re-mint every inline node it owns, and identity is what a
 * consumer keys on.
 *
 * RETURNS THE NODE NOW AT THE UNIT'S POSITION. A postprocessor may replace a
 * unit and free what it replaced (a fenced code block whose info is
 * `formula`, and a paragraph that is nothing but a display formula, both in
 * the formula extension), so the returned pointer is the only one a caller
 * may keep. Two rules come with it: anything caching the unit's CHILD
 * pointers must run AFTER this call, because the autolink pass splices that
 * list; and no stamp is performed, which is the caller's to do if the tree
 * may still meet the append diff.
 *
 * Call order is close order — children before the containers that closed
 * them — which markdown_core_parser_warm_settle keeps for its callers. */
markdown_core_node *markdown_core_parser_warm_refine_settled(markdown_core_parser *parser, markdown_core_node *unit);

/** Parses inlines and postprocesses the tree WITHOUT ending the parse.
 *
 * markdown_core_parser_refine_blocks does the same two passes and then takes
 * the tree away — `res = parser->root; parser->root = NULL` — after which the
 * parser ignores every further feed. This is the second form: no stamp, no
 * detach, and a false return where refine_blocks would have freed the tree.
 * It is the TERMINAL close of a build whose end state cannot be published
 * from — the parser keeps owning the tree, and is simply never fed again.
 *
 * NO STAMP is a caller obligation, not a saving: subtree hashes are what the
 * append diff pairs on, so a tree that may still be handed to it has to be
 * stamped by whoever closed it.
 *
 * PRECONDITION: no unit in the tree already holds inline children. A unit is
 * parsed for inlines exactly once — the records vector is assigned, not
 * merged — so this is for a parser nothing has settled. */
bool markdown_core_parser_warm_refine(markdown_core_parser *parser);

/** Everything a projection may read but must not change, in one value: the
 * line counters and sticky failure bits, the held partial line and its
 * pending CR, every node's type, flags, coordinates and content bytes, both
 * definition tables in order, and the paragraph's line marks.
 *
 * The question it answers is whether a parser is EXACTLY where it was — the
 * decidable form of "the projection left no trace" — so a speculative close
 * and its undo can be gated on restoring it bit for bit. Anything added to
 * the parser or to a node must be added here, or the gate goes blind to it.
 *
 * O(tree + text) per call: a gate's budget, not a tick's. */
uint64_t markdown_core_parser_warm_fingerprint(const markdown_core_parser *parser);

/** Maps the content-buffer extent [x0, x1) of the line `mark` records onto
 * that normalized source line: `*column` receives the record column and the
 * return value the record length. The map is affine at slope one except for
 * the mark's stand-in pad: an extent that begins inside the pad begins on
 * the tab byte itself, and every offset past the pad sits one tab byte —
 * not `pad` spaces — after `byte_offset`. `x1` never lands inside the pad,
 * because every extent a caller maps ends on a marker or spelling byte and
 * the pad holds only the append's stand-in spaces. */
markdown_core_bufsize markdown_core_line_mark_extent(
    const struct markdown_core_line_mark *mark,
    markdown_core_bufsize x0,
    markdown_core_bufsize x1,
    markdown_core_bufsize *column
);

/** Records one diagnostic at the given extent. A lost diagnostic is not a
 * lost parse: the tree is unaffected and the document is still correct, it
 * is only missing an underline, so this reports nothing on allocation
 * failure rather than poisoning a parse that otherwise succeeded. */
void markdown_core_parser_record_diagnostic(
    markdown_core_parser *parser,
    int code,
    int start_line,
    int start_column,
    int end_line,
    int end_column
);

/** Applies the core list-item continuation rule to the current line.
 * Extension-owned list items use this to stay in lockstep with plain list
 * items. */
bool markdown_core_parser_match_list_item_prefix(
    markdown_core_parser *parser,
    markdown_core_chunk *input,
    markdown_core_node *container
);

/** Captures one concrete marker record on the node whose ownership region
 * the marker belongs to (11.1). `column` and `length` are byte extents in
 * the current normalized line; the record's line is derived here as the
 * offset from the node's still-absolute start_line. The one capture
 * funnel for block-phase marker material, core and extensions alike: a
 * lost record sets parser->capture_lost, which S_process_line folds into
 * `oom` at the line boundary rather than mid-line (see the field's
 * comment above). */
void markdown_core_parser_capture_marker(
    markdown_core_parser *parser,
    markdown_core_node *node,
    uint8_t kind,
    markdown_core_bufsize column,
    markdown_core_bufsize length
);

/** The look-back variant: identical, but the marker was spelled on
 * `line` (absolute, same numbering as the live parse) rather than the
 * line being processed — the table extension recovers a header row from
 * the paragraph's accumulated content one line after buffering it. */
void markdown_core_parser_capture_marker_at(
    markdown_core_parser *parser,
    markdown_core_node *node,
    uint8_t kind,
    int line,
    markdown_core_bufsize column,
    markdown_core_bufsize length
);

#ifdef __cplusplus
}
#endif

#endif
