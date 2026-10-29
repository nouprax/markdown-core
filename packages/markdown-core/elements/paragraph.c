#include "paragraph.h"
#include "block_internal.h"

#include "link.h"
#include "block_identifier.h"
#include "heading.h"
/* A paragraph closing inside `parent` yields the reference definitions at
 * the front of its text, settled into `parent` before it, and then itself,
 * less a block identifier at the back of its text, when anything of it is
 * left. */
static bool close_paragraph(const markdown_core_element_instance *self, markdown_core_parser *parser,
                            markdown_core_node *paragraph, markdown_core_node *parent) {
    if (!markdown_core_block_resolve_reference_link_definitions(parser, paragraph, parent)) {
        return false;
    }
    markdown_core_block_attach_paragraph_identifier(self->state, parser, paragraph, parent);
    return true;
}

static int continue_paragraph(const markdown_core_element_instance *self, markdown_core_parser *parser,
                              unsigned char *data, int length, markdown_core_node *container) {
    return !parser->at.blank;
}
/* The paragraph reads whether the line is blank: its first non-space byte. */
static bufsize_t continue_reach(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                const markdown_core_node *node, const markdown_core_chunk *input) {
    (void)self;
    (void)node;
    (void)input;
    return parser->at.first_nonspace;
}
static bool accepts_lazy(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         markdown_core_node *node) {
    (void)self;
    (void)parser;
    (void)node;
    return true;
}
static markdown_core_node *open_lazy(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                     markdown_core_node *node, markdown_core_chunk *input) {
    (void)self;
    return node;
}

/* A text line no block claims opens a paragraph: the dialect's text block. */
static markdown_core_node *open_text(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                     markdown_core_node *container, markdown_core_chunk *input) {
    container = markdown_core_block_parent_for(parser, container, MARKDOWN_CORE_NODE_PARAGRAPH);
    if (!container) {
        return NULL;
    }
    if (markdown_core_block_attach_identifier_line(self, parser, container, input) || parser->error) {
        return NULL;
    }
    container = markdown_core_parser_add_child_validated(parser, container, MARKDOWN_CORE_NODE_PARAGRAPH,
                                                         parser->at.first_nonspace + 1);
    if (container) {
        markdown_core_block_advance_offset(parser, input, parser->at.first_nonspace - parser->at.offset, false);
    }
    return container;
}

/* A paragraph a setext underline may yet follow is a heading, whose tail
 * its last line may hold: a context run reports the tails it may, which
 * stand only where the document finishes the heading. */
static void begin_inline(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         markdown_core_inline_state *inline_state, markdown_core_node *parent) {
    (void)self;
    (void)parser;
    (void)parent;
    if (inline_state->context) {
        markdown_core_heading_report_tail(inline_state, true);
    }
}

/* The blocks an identifier line after them names (block_identifier.c). */
static const markdown_core_node_type IDENTIFIED_KINDS[] = {MARKDOWN_CORE_NODE_LIST, MARKDOWN_CORE_NODE_CALLOUT,
                                                           MARKDOWN_CORE_NODE_TABLE, MARKDOWN_CORE_NODE_NONE};

const markdown_core_element MARKDOWN_CORE_ELEMENT_PARAGRAPH = {
    .begin_inline = begin_inline,
    .trail_kinds = IDENTIFIED_KINDS,
    .state_size = sizeof(markdown_core_block_identifier_work),
    .accepts_lazy = accepts_lazy,
    .open_lazy = open_lazy,

    .name = "paragraph",
    .last_block_matches = continue_paragraph,
    .continue_reach = continue_reach,
    .content_mode = MARKDOWN_CORE_CONTENT_PROSE,
    .inline_content = true,
    .paragraph = true,
    .close_block = close_paragraph,
    .open_text_block = open_text,
    .feed_candidate = markdown_core_block_feed_identifier_line,
    .decide_candidate = markdown_core_block_decide_identifier_line,
};
