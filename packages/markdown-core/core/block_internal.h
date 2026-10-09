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
/* Whether `node` ends with a blank line: its last line was blank, or, for a
 * kind a child's blank line propagates out of, its last child the facts see
 * ends with one. */
bool markdown_core_block_ends_with_blank_line(const markdown_core_node *node);
/* Whether a blank line separates two children of `node`, a list, or two
 * children of one of them: the list is loose. */
bool markdown_core_block_loose(const markdown_core_node *node);
/* The children summary (E4) of a kind a child's blank line propagates out
 * of. */
extern const markdown_core_stem_summary MARKDOWN_CORE_BLANK_SUMMARY;
markdown_core_member *markdown_core_block_finalize(markdown_core_parser *parser, markdown_core_member *b);
/* A BLOCK AN ELEMENT CLOSES ITSELF, once its range is settled: a table's
 * lead paragraph, which the delimiter line closed and which with `holds`
 * holds the next block, or a grid or multiline table, whole once it is
 * built. It records what the line has read (5.1, 5.3) and settles as
 * markdown_core_block_finalize settles a block. */
void markdown_core_block_close(markdown_core_parser *parser, markdown_core_member *b, bool holds);
void markdown_core_block_advance_offset(markdown_core_parser *parser, markdown_core_chunk *input, bufsize_t count,
                                        bool columns);
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
     * lazy line, as in cmark. A start the context refuses says so through
     * markdown_core_block_start_refuses, which sets `refused`. */
    bool paragraph, lazy, all_matched, refused;
    size_t depth;
    bufsize_t thematic_kill;
} block_start_context;
/* Whether `context` refuses a start whose own syntax matched the line: one
 * that does not interrupt a paragraph (`paragraph`) or open on a lazy line
 * (`lazy`). The line would open it after a closed block, so a refusal is
 * the line's reading depending on the block it would continue: the block
 * the line closes holds the next one (5.3). */
bool markdown_core_block_start_refuses(block_start_context *context, bool paragraph, bool lazy);

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
