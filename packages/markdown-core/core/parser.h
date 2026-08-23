#ifndef MARKDOWN_CORE_PARSER_H
#define MARKDOWN_CORE_PARSER_H

#include <stdint.h>
#include <stdio.h>
#include "references.h"
#include "node.h"
#include "buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_LINK_LABEL_LENGTH 1000

/* THE CONCRETE RECORD SET (requirement 11a).
 *
 * A region is a byte range of the normalized source with exactly one owner and
 * exactly one role. Every block-level byte is in exactly one region, so the
 * regions on a line tile it and the regions in order reproduce the source.
 *
 * The three roles are what the byte DID, not what it looks like:
 *
 *   MARKER     the bytes that made the owner the kind of thing it is, and are
 *              not its content: `> `, `- `, `#`s, a fence, `[^label]:`.
 *   CONTENT    the bytes that went into the owner's content buffer, and so are
 *              the bytes its children are cut from.
 *   DISCARDED  the bytes the parse read and kept nowhere -- indentation
 *              stripped from a continuation line, the trailing hashes of a
 *              closed ATX heading, a line ending nothing owns. They still have
 *              an owner: the block they were read inside.
 *
 * A region may be REFINED -- split into adjacent regions covering the same
 * bytes -- which is how an extension takes credit for part of a span without
 * breaking the tiling. It may never be moved and never be deleted. */
typedef enum {
    MARKDOWN_CORE_REGION_MARKER = 0,
    MARKDOWN_CORE_REGION_CONTENT = 1,
    MARKDOWN_CORE_REGION_DISCARDED = 2
} markdown_core_region_role;

typedef struct {
    /* Offset in the parser's normalized source, and the length in bytes. */
    bufsize_t start;
    bufsize_t length;
    /* The node these bytes belong to. Never NULL: bytes that belong to no
     * block belong to the document. */
    struct markdown_core_node *owner;
    uint8_t role;
} markdown_core_region;

/* Where one source line's bytes landed in a block's content buffer.
 *
 * A block's content is the concatenation of the line slices `add_line` copies
 * into it, and the source column a slice starts at is NOT derivable from the
 * block's own `start_column`: the container prefix stripped from a
 * continuation line need not match the one stripped from the first, so
 * `"> foo\nbar"` strips two bytes then none. One mark per `add_line` call
 * records where the slice came from, and `markdown_core_parser_content_place`
 * reads them back.
 *
 * Marks are appended in parse order and only the deepest open block takes
 * lines, so one block's marks are the contiguous run
 * [node->content_mark, node->content_mark + node->content_mark_count). */
typedef struct {
    /* Offset in the owning block's content where this slice begins. */
    bufsize_t content_offset;
    /* The source line the slice was copied from, counted from 1. */
    int line;
    /* The BYTE column on that line the slice begins at, counted from 1. */
    int column;
} markdown_core_line_mark;

struct markdown_core_parser {
    struct markdown_core_mem *mem;
    /* A hashtable of urls in the current document for cross-references */
    struct markdown_core_map *refmap;
    /* The labels this document defines footnotes for (see references.h). The
     * block phase fills it as each definition opens; the inline phase reads it
     * to decide whether a `[^label]` is a call at all. */
    struct markdown_core_map *footnote_defs;
    /* The root node of the parser, always a MARKDOWN_CORE_NODE_DOCUMENT */
    struct markdown_core_node *root;
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
    /* THE NORMALIZED SOURCE: every line exactly as S_process_line normalized
     * it -- UTF-8 validated if the option is on, NUL replaced, the line ending
     * a single '\n' whether the author wrote one, wrote CRLF, or wrote nothing
     * at all -- concatenated in order. The document retains it because the
     * concrete record set indexes it: a region is a byte range in HERE, not in
     * whatever buffer the caller fed, and requirement 11a's L3 says the regions
     * in order reproduce it byte for byte.
     *
     * It is NOT the caller's bytes. `markdown_core_parser_feed` may be called
     * with any split, the caller's buffer may be freed the moment feed returns,
     * and two different inputs normalize to the same source -- which is the
     * point: what the tree describes is this. */
    markdown_core_strbuf source;
    /* Where each line begins in `source`: line N starts at line_starts[N - 1].
     * The line index the same requirement names, and the only thing that can
     * turn a source offset back into a (line, column) after the parse. */
    bufsize_t *line_starts;
    bufsize_t line_starts_size;
    bufsize_t line_starts_alloc;
    /* The concrete record set (see markdown_core_region), in source order, and
     * how far the line in hand has been attributed. Regions are emitted while
     * the line that contains them is being processed and never afterwards,
     * which is L4: the record set is complete for lines 1..N once line N has
     * been fed. */
    markdown_core_region *regions;
    bufsize_t regions_size;
    bufsize_t regions_alloc;
    bufsize_t region_cursor;
    /* When set, markdown_core_parser_finish writes the record set here before
     * releasing it. There is no public reader: requirement 12 is where a
     * document keeps the concrete view, and until then the CLI's `--concrete`
     * and the gate that drives it are the only consumers. */
    FILE *concrete_out;
    /* See the documentation for markdown_core_parser_get_last_line_length() in markdown_core.h */
    bufsize_t last_line_length;
    /* Accumulates partial feed chunks until a complete line is available;
     * curline holds the normalized line currently being parsed. */
    markdown_core_strbuf linebuf;
    /* Options set by the user, see the Options section in markdown_core.h */
    int options;
    /* Sticky allocation-failure flag: once any parse structure is lost,
     * markdown_core_parser_finish reports the whole parse as failed (NULL)
     * instead of returning a silently truncated document. */
    bool oom;
    bool last_buffer_ended_with_cr;
    size_t total_size;
    markdown_core_llist *syntax_extensions;
    markdown_core_llist *inline_syntax_extensions;
    markdown_core_ispunct_func backslash_ispunct;
    /* Inline special-character tables for this parser: the core defaults plus
     * the special/emphasis-skip characters of the attached inline extensions.
     * Parser-local so concurrent parsers with different extension sets never
     * observe each other's characters. */
    int8_t special_chars[256];
    int8_t skip_chars[256];
    /* The content-to-source map (see markdown_core_line_mark): one run per
     * block that took lines, appended in parse order. It is read while the
     * parse is still running -- the block phase reads it as blocks close and
     * the inline phase reads it before markdown_core_parser_finish resets --
     * and it is released with the rest of the per-parse state. */
    markdown_core_line_mark *line_marks;
    bufsize_t line_marks_size;
    bufsize_t line_marks_alloc;
};

#ifdef __cplusplus
}
#endif

#endif
