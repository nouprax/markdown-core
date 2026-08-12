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
     * and it has to be the whole document's answer even when an edit
     * reparses one paragraph of it. That is why the map is the document's, not
     * the parse's: see markdown_core_footnote_definition_create for why it is
     * a second map rather than a discriminated column of `refmap`. */
    struct markdown_core_map *footnote_defs;
    /* The root node of the parser, always a MARKDOWN_CORE_NODE_DOCUMENT */
    struct markdown_core_node *root;
    /* The last open block after a line is fully processed */
    struct markdown_core_node *current;
    /* See the documentation for markdown_core_parser_get_line_number() in markdown_core.h */
    int line_number;
    /* See the documentation for markdown_core_parser_get_offset() in markdown_core.h */
    markdown_core_bufsize offset;
    /* See the documentation for markdown_core_parser_get_column() in markdown_core.h */
    markdown_core_bufsize column;
    /* See the documentation for markdown_core_parser_get_first_nonspace() in markdown_core.h */
    markdown_core_bufsize first_nonspace;
    /* See the documentation for markdown_core_parser_get_first_nonspace_column() in markdown_core.h
     */
    markdown_core_bufsize first_nonspace_column;
    markdown_core_bufsize thematic_break_kill_pos;
    /* See the documentation for markdown_core_parser_get_indent() in markdown_core.h */
    int indent;
    /* See the documentation for markdown_core_parser_is_blank() in markdown_core.h */
    bool blank;
    /* True while processing a line that began with the document as the only
     * open block; direct document children opened on such a line get
     * MARKDOWN_CORE_NODE__CLEAN_START. */
    bool line_began_clean;
    /* True while processing a line whose open chain at line start consisted
     * solely of footnote definitions and whose own shape failed every one of
     * their prefixes (check_open_blocks stopped at the document): the line
     * closes the whole chain, so its direct document children get
     * CLEAN_START qualified by CLEAN_START_SEALING. */
    bool line_defs_only;
    /* See the documentation for markdown_core_parser_has_partially_consumed_tab() in
     * markdown_core.h */
    bool partially_consumed_tab;
    /* Contains the currently processed line */
    markdown_core_strbuf curline;
    /* See the documentation for markdown_core_parser_get_last_line_length() in markdown_core.h */
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
    size_t total_size;
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
