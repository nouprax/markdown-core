#ifndef MARKDOWN_CORE_BLOCK_INTERNAL_H
#define MARKDOWN_CORE_BLOCK_INTERNAL_H
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "markdown_core_ctype.h"
#include "utf8.h"
#include "houdini.h"
#include "iterator.h"
#include "inlines.h"
#include "element.h"
#define CODE_INDENT 4
#define TAB_STOP 4
#ifndef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif

bool markdown_core_block_last_line_blank(const markdown_core_node *node);
markdown_core_node_type markdown_core_block_type(const markdown_core_node *node);
void markdown_core_block_set_end_to_current_line(markdown_core_parser *parser, markdown_core_node *b);
bool markdown_core_block_is_blank(markdown_core_strbuf *s, bufsize_t offset);
void markdown_core_block_rebase_content_marks(markdown_core_parser *parser, markdown_core_node *node, bufsize_t dropped,
                                              bufsize_t remaining);
/* Whether the last child of `parent`, an open block, ends with a blank line,
 * descending into lists and sublists (blocks.c). */
bool markdown_core_block_last_child_ends_blank(const markdown_core_parser *parser, const markdown_core_node *parent);
/* Whether the deepest open block, a list, is loose (blocks.c). */
bool markdown_core_block_list_loose(const markdown_core_parser *parser);
markdown_core_node *markdown_core_block_finalize(markdown_core_parser *parser, markdown_core_node *b);
/* A BLOCK SPLIT OFF BEFORE THE DEEPEST OPEN BLOCK: `lead`, made of the open
 * block's first lines and finalized by its element, which the line being
 * processed decided end before the open block, and which its parent holds
 * just before it. The lead closes as any block does: it starts after what
 * the open block's entry recorded, which is the lead's now, and it read
 * what the line has; the open block now starts right after it. */
bool markdown_core_parser_close_lead(markdown_core_parser *parser, markdown_core_node *lead);
/* A LATER LINE'S WRITE TO A CLOSED BLOCK (docs/plans/2026-09-29-incremental-
 * parsing.md, E2): the last child of `parent`, an open block, which a
 * decision of the line being processed changes and extends to `end`. Its
 * reach is raised to what the line has read, and its parent's next child
 * starts after it. The block is this parse's own, held once, so it changes
 * in place: a block a later line may write into is EXIT_FRAGILE (node.h),
 * and no run of taken blocks ends at it. */
markdown_core_node *markdown_core_parser_write_closed(markdown_core_parser *parser, markdown_core_node *parent,
                                                      size_t end);
/* THE OLD BLOCK A MAKER READS ITS PARTS FROM (docs/plans/2026-09-29-
 * incremental-parsing.md, 5.3 and E5): the old block of `kind` that starts
 * in [from, to] among the children of the old block the open block
 * `parent` continues, when the open blocks down to `parent` carry what their
 * old blocks carried, with where it starts in `*start`; NULL otherwise. A
 * block its element makes whole from the lines a lookahead reads -- a grid,
 * multiline or simple table -- takes the parts of the old one its own
 * reading reproduces. A block the edits left with no byte is no such
 * block. */
const markdown_core_node *markdown_core_parser_old_block(const markdown_core_parser *parser,
                                                         const markdown_core_node *parent, markdown_core_node_type kind,
                                                         size_t from, size_t to, int64_t *start);
/* A RUN OF PARTS A MAKER TAKES (5.3): the `count` children of `old` from
 * child `first`, the first of which starts at `start` and the last ends at
 * `end`, join the children of `node`, which the maker is making, as the
 * parse that made them left them, and the parse records the run. False when
 * storage runs out, with the parse lost. */
bool markdown_core_parser_take_parts(markdown_core_parser *parser, markdown_core_node *node,
                                     const markdown_core_node *old, size_t first, size_t count, size_t start,
                                     size_t end);
void markdown_core_block_advance_offset(markdown_core_parser *parser, markdown_core_chunk *input, bufsize_t count,
                                        bool columns);
typedef struct markdown_core_block_start_context {
    markdown_core_node *container;
    markdown_core_chunk *input;
    int first, column, indent;
    /* `paragraph`: the container is a paragraph the line would continue.
     * `lazy`: the line missed the current block's prefix, and that block
     * would take it as text. Indented code, an HTML block of the seventh
     * kind, a dash-led table and the paragraph hooks open on neither, so such
     * a line stays text. Every other start reads `paragraph` alone, if
     * anything: a list that cannot interrupt a paragraph still opens on a
     * lazy line, as in cmark. */
    bool paragraph, lazy, all_matched;
    size_t depth;
    bufsize_t thematic_kill;
} block_start_context;

typedef struct markdown_core_block_start {
    /* The claiming owner's open, and the owner itself, which the dispatcher
     * records and hands `open` as its `self`. */
    bool (*open)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_node **,
                 markdown_core_chunk *, struct markdown_core_block_start *);
    const markdown_core_element_instance *owner;
    markdown_core_node_type kind;
    bufsize_t matched;
    markdown_core_list list;
    markdown_core_specimen_value specimen;
} block_start;
int markdown_core_block_consume_item_marker(markdown_core_parser *parser, markdown_core_chunk *input, int marker_width);
void markdown_core_block_find_first_nonspace(markdown_core_parser *parser, markdown_core_chunk *input);
bool markdown_core_block_continue_indented(markdown_core_parser *parser, markdown_core_chunk *input, int continuation,
                                           bool has_content);
void markdown_core_block_add_line(markdown_core_node *node, markdown_core_chunk *input, markdown_core_parser *parser);
/* `node`, a leaf block, starts at `start` now, its content having lost what
 * came before: its lines stay where they were read, the first one's lead
 * running from the new start (E5). */
void markdown_core_parser_move_start(markdown_core_parser *parser, markdown_core_node *node, uint32_t start);
markdown_core_node *markdown_core_block_parent_for(markdown_core_parser *parser, markdown_core_node *parent,
                                                   markdown_core_node_type kind);
/* Commit a parent selected by block_parent_for without repeating its policy. */
markdown_core_node *markdown_core_parser_add_child_validated(markdown_core_parser *parser, markdown_core_node *parent,
                                                             markdown_core_node_type kind, int start_column);
#endif
