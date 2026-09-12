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
#include "../elements/markdown-core-elements.h"
#define CODE_INDENT 4
#define TAB_STOP 4
#ifndef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif

uint64_t markdown_core_source_key(const void *entry);
bool markdown_core_block_last_line_blank(const markdown_core_node *node);
markdown_core_node_type markdown_core_block_type(const markdown_core_node *node);
bool markdown_core_block_is_space_or_tab(char c);
void markdown_core_block_set_end_to_current_line(markdown_core_parser *parser, markdown_core_node *b);
bool markdown_core_block_is_blank(markdown_core_strbuf *s, bufsize_t offset);
bool markdown_core_block_accepts_lines(markdown_core_node *node);
int markdown_core_block_content_mark_at(markdown_core_parser *parser, const markdown_core_node *node, bufsize_t offset);
void markdown_core_block_rebase_content_marks(markdown_core_parser *parser, markdown_core_node *node, bufsize_t dropped,
                                              bufsize_t remaining);
bool markdown_core_block_ends_with_blank_line(markdown_core_node *node);
markdown_core_node *markdown_core_block_finalize(markdown_core_parser *parser, markdown_core_node *b);
void markdown_core_block_advance_offset(markdown_core_parser *parser, markdown_core_chunk *input, bufsize_t count,
                                        bool columns);
int markdown_core_block_order_definitions(markdown_core_mem *mem, markdown_core_definition_collection *collection);
void markdown_core_block_own_definitions(markdown_core_definition_collection *collection, markdown_core_node **slot);
typedef struct markdown_core_block_start_context {
    markdown_core_node *container;
    markdown_core_chunk *input;
    int first, column, indent;
    bool paragraph, lazy, all_matched;
    size_t depth;
    bufsize_t thematic_kill;
} block_start_context;

typedef struct markdown_core_block_start {
    bool (*open)(markdown_core_parser *, markdown_core_node **, markdown_core_chunk *,
                 struct markdown_core_block_start *);
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
#endif
