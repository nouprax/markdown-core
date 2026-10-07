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

/* The source start of an entry that begins with a markdown_core_source_entry. */
uint64_t markdown_core_source_key(const void *entry);
bool markdown_core_block_last_line_blank(const markdown_core_node *node);
markdown_core_node_type markdown_core_block_type(const markdown_core_node *node);
bool markdown_core_block_is_blank(markdown_core_strbuf *s, bufsize_t offset);
void markdown_core_block_rebase_content_marks(markdown_core_parser *parser, markdown_core_node *node, bufsize_t dropped,
                                              bufsize_t remaining);
bool markdown_core_block_ends_with_blank_line(const markdown_core_parser *parser, markdown_core_node *node);
/* The sibling after `member` the blank-line facts see: the next one that is
 * not MARKDOWN_CORE_NODE__BLANK_TRANSPARENT, or NULL. */
markdown_core_member *markdown_core_block_next_seen(const markdown_core_member *member);
/* Whether a node the blank-line facts see follows the one at `index` of
 * `stem`. */
bool markdown_core_block_seen_after(const markdown_core_stem *stem, size_t index);
markdown_core_member *markdown_core_block_finalize(markdown_core_parser *parser, markdown_core_member *b);
/* A BLOCK AN ELEMENT CLOSES ITSELF, once its range is settled: a table's
 * lead paragraph, which the delimiter line closed and which with `holds`
 * holds the next block, or a grid or multiline table, whole once it is
 * built. It records what the line has read (5.1, 5.3) and settles as
 * markdown_core_block_finalize settles a block. */
void markdown_core_block_close(markdown_core_parser *parser, markdown_core_member *b, bool holds);
void markdown_core_block_advance_offset(markdown_core_parser *parser, markdown_core_chunk *input, bufsize_t count,
                                        bool columns);
int markdown_core_block_order_definitions(markdown_core_parser *parser,
                                          markdown_core_definition_collection *collection);
typedef struct markdown_core_block_start_context {
    markdown_core_member *container;
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
    bool (*open)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_member **,
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
markdown_core_member *markdown_core_block_parent_for(markdown_core_parser *parser, markdown_core_member *parent,
                                                     markdown_core_node_type kind);
/* Commit a parent selected by block_parent_for without repeating its policy. */
markdown_core_member *markdown_core_parser_add_child_validated(markdown_core_parser *parser,
                                                               markdown_core_member *parent,
                                                               markdown_core_node_type kind, int start_column);
#endif
