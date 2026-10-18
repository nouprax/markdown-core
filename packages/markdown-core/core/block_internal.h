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
#include "inlines.h"
#include "element.h"
#define CODE_INDENT 4
#define TAB_STOP 4
#ifndef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif

uint64_t markdown_core_source_key(const void *entry);
bool markdown_core_block_last_line_blank(const markdown_core_node *node);
markdown_core_node_type markdown_core_block_type(const markdown_core_node *node);
void markdown_core_block_set_end_to_current_line(markdown_core_parser *parser, markdown_core_node *b);
void markdown_core_block_rebase_content_marks(markdown_core_parser *parser, markdown_core_node *node, bufsize_t cut,
                                              bufsize_t remaining);
/* Whether `node`, a closed block, ends with a blank line (blocks.c). */
bool markdown_core_block_ends_with_blank_line(const markdown_core_node *node);
/* Closes the open blocks from the current one down to `b`, which is open
 * above the active input's root, each ending where the line in hand says;
 * returns the block that is current after them. */
markdown_core_node *markdown_core_block_close(markdown_core_parser *parser, markdown_core_node *b);
/* `node`, a block made whole, its own children settled, settles into
 * `parent`, which has no pending child: it joins the end of the parent's
 * children and the completion queue (blocks.c, A BLOCK SETTLES). */
void markdown_core_block_settle(markdown_core_parser *parser, markdown_core_node *node, markdown_core_node *parent);
/* `field`, a field root `holder` holds -- a callout's title, a definition's
 * term, a block directive's label, a table's caption -- settles as `holder`
 * installs it, once the line that writes it is read: it holds as a settled
 * block does from then on, and joins the completion queue with `holder`, to
 * complete on its own (blocks.c, A BLOCK SETTLES). */
void markdown_core_block_settle_field(markdown_core_parser *parser, markdown_core_node *field,
                                      markdown_core_node *holder);
/* `b`, a block made whole that is on no stack, closes inside `parent` as a
 * block the line machine closes does -- its element reads it as a finished
 * thing, and it takes the runs of its own lines -- and settles into it. */
void markdown_core_block_close_into(markdown_core_parser *parser, markdown_core_node *b, markdown_core_node *parent);
/* The current block, its range settled, closes and becomes the pending child
 * of the block below it. */
void markdown_core_block_close_current(markdown_core_parser *parser);
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
markdown_core_node *markdown_core_block_parent_for(markdown_core_parser *parser, markdown_core_node *parent,
                                                   markdown_core_node_type kind);
/* Commit a parent selected by block_parent_for without repeating its policy. */
markdown_core_node *markdown_core_parser_add_child_validated(markdown_core_parser *parser, markdown_core_node *parent,
                                                             markdown_core_node_type kind, int start_column);
#endif
