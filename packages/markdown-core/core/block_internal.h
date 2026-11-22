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
 * `parent`, which has no open or pending child: it joins the end of the
 * parent's children and the completion queue (blocks.c, A BLOCK SETTLES). */
void markdown_core_block_settle(markdown_core_parser *parser, markdown_core_node *node, markdown_core_node *parent);
/* The same before the last of `parent`'s children: the block it was read
 * from as that closes, or the block it was split from. */
void markdown_core_block_settle_before(markdown_core_parser *parser, markdown_core_node *node,
                                       markdown_core_node *parent);
/* `field`, a field root `holder` holds -- a callout's title, a definition's
 * term, a block directive's label, a table's caption -- settles as `holder`
 * installs it, once the line that writes it is read: it holds as a settled
 * block does from then on, and joins the completion queue with `holder`, to
 * complete on its own (blocks.c, A BLOCK SETTLES). */
void markdown_core_block_settle_field(markdown_core_parser *parser, markdown_core_node *field,
                                      markdown_core_node *holder);
/* `field`, the field root `holder` held, goes, as a reading that stands
 * inside the line that wrote it makes the field again (element.h, THE TEXT
 * OF ITS LINE): what it holds goes to the records the publish names its
 * content by, as a paragraph's children go when it reads its content
 * again, and lives on until the publish lets it go (blocks.c, THE SEAMS). */
void markdown_core_block_unsettle_field(markdown_core_parser *parser, markdown_core_node *field,
                                        markdown_core_node *holder);
/* The current block, its range settled, closes and becomes the pending child
 * of the block below it. */
void markdown_core_block_close_current(markdown_core_parser *parser);
typedef struct markdown_core_block_start_context {
    markdown_core_node *container;
    /* The line, at its first non-space byte. */
    markdown_core_reader *reader;
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

/* What a callout's scan found after its marker (callout.c): a header
 * `[!variant]`, its fold and title, and where a variant the line's end cuts
 * short stands, -1 for none, which its open reports. */
typedef struct {
    bufsize_t variant, variant_end, title, report;
    bool metadata, fold, collapsed;
    /* Where the marker's motion leaves the reader. */
    markdown_core_reader_mark after;
} markdown_core_callout_header;

typedef struct markdown_core_block_start {
    /* The claiming owner's open, and the owner itself, which the dispatcher
     * records and hands `open` as its `self`. */
    bool (*open)(const markdown_core_element_instance *, markdown_core_parser *, markdown_core_node **,
                 markdown_core_reader *, struct markdown_core_block_start *);
    /* What the open reads past what the scan read, read on a copy of the
     * reader the engine hands it (the marker's padding, the item's first
     * byte). The engine asks it only on a line more text can change, where
     * how far the step reads decides whether the line is held; NULL when the
     * open reads nothing more. */
    void (*reads)(const markdown_core_element_instance *, markdown_core_reader *,
                  const struct markdown_core_block_start *);
    const markdown_core_element_instance *owner;
    markdown_core_node_type kind;
    bufsize_t matched;
    markdown_core_list list;
    markdown_core_specimen_value specimen;
    markdown_core_callout_header callout;
} block_start;
int markdown_core_block_consume_item_marker(markdown_core_reader *reader, int marker_width);
/* Adds the rest of the line in hand, from the reader's offset, to `node`'s content. */
void markdown_core_block_add_line(markdown_core_parser *parser, markdown_core_node *node);
markdown_core_node *markdown_core_block_parent_for(markdown_core_parser *parser, markdown_core_node *parent,
                                                   markdown_core_node_type kind);
/* Commit a parent selected by block_parent_for without repeating its policy. */
markdown_core_node *markdown_core_parser_add_child_validated(markdown_core_parser *parser, markdown_core_node *parent,
                                                             markdown_core_node_type kind, int start_column);
/* `child`, a block made for `parent`, which holds its kind, opens in it as
 * add_child_validated opens the block it makes: what is open inside the
 * container closes first, its pending child settles, and `child` is its last
 * child and the builder above it. NULL, with the parse failed, when the
 * children could not grow. */
markdown_core_node *markdown_core_parser_open_child(markdown_core_parser *parser, markdown_core_node *parent,
                                                    markdown_core_node *child);
/* THE CALL (parser.h), inlined at each opener with its element's `reads`,
 * the hook its candidates are decided by: most calls are refused by their
 * reading's first test, which then costs no frame of the engine's. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) markdown_core_candidate_decision
    markdown_core_parser_call(markdown_core_parser *parser, const markdown_core_element_instance *owner,
                              bool (*reads)(const markdown_core_element_instance *, markdown_core_parser *,
                                            struct markdown_core_node *, markdown_core_reader *, void *),
                              struct markdown_core_node *container, markdown_core_reader *reader,
                              markdown_core_node_type child, void *record, bool ends) {
    assert(reads == owner->element->candidate->reads);
    if (markdown_core_parser_candidate_meets(parser, owner, container)) {
        return MARKDOWN_CORE_CANDIDATE_REJECT;
    }
    markdown_core_parser_clear_end_read(parser);
    const bool claims = reads(owner, parser, container, reader, record);
    if (parser->error) {
        return MARKDOWN_CORE_CANDIDATE_REJECT;
    }
    if (markdown_core_parser_end_read(parser)) {
        markdown_core_parser_request_candidate(parser, owner, container, child, record, ends);
        return MARKDOWN_CORE_CANDIDATE_WAIT;
    }
    return claims ? MARKDOWN_CORE_CANDIDATE_ACCEPT : MARKDOWN_CORE_CANDIDATE_REJECT;
}

#endif
