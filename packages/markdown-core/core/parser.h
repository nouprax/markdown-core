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
     * and it has to be the whole document's answer even when a session
     * reparses one paragraph of it. That is why the map is the session's, not
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
    struct markdown_core_line_mark {
        markdown_core_bufsize content_offset;
        int line;
        int column;
    } *line_marks;
    size_t line_mark_count;
    size_t line_mark_capacity;
};

/** Returns a parser whose parse ended (root handed off or freed) to its
 * post-construction state for another parse, keeping the line buffers'
 * capacity, an attached (empty) reference map, and the extension
 * attachments. Allocation failure poisons the parser like a failed reset. */
void markdown_core_parser_renew(markdown_core_parser *parser);

/** Applies the core list-item continuation rule to the current line.
 * Extension-owned list items use this to stay in lockstep with plain list
 * items. */
bool markdown_core_parser_match_list_item_prefix(
    markdown_core_parser *parser,
    markdown_core_chunk *input,
    markdown_core_node *container
);

#ifdef __cplusplus
}
#endif

#endif
