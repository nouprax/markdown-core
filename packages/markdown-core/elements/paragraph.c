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

static const markdown_core_content_ops PARAGRAPH_CONTENT = {
    .mode = MARKDOWN_CORE_CONTENT_PROSE,
    .inlines = true,
    .paragraph = true,
};

static const markdown_core_inline_ops PARAGRAPH_INLINES = {
    .enter = begin_inline,
};

/* A paragraph of link reference definitions closes as the line that starts
 * the next is read, as the old reading read on from them: the next reads
 * as from a paragraph's start (link.c, THE REACH OF A DEFINITION). */
static bool yields(const markdown_core_element_instance *self, markdown_core_parser *parser,
                   const markdown_core_node *open, markdown_core_node_type kind) {
    (void)self;
    return kind == MARKDOWN_CORE_NODE_REFERENCE && markdown_core_block_reference_definitions_whole(parser, open);
}

static const markdown_core_node_type PARAGRAPH_JOINS[] = {MARKDOWN_CORE_NODE_REFERENCE, MARKDOWN_CORE_NODE_NONE};

static const markdown_core_edit_ops PARAGRAPH_EDIT = {
    .yields = yields,
    .joins = PARAGRAPH_JOINS,
};

static const markdown_core_candidate_ops PARAGRAPH_CANDIDATE = {
    .feed = markdown_core_block_feed_identifier_line,
    .decide = markdown_core_block_decide_identifier_line,
};

static const markdown_core_open_ops PARAGRAPH_OPENS = {
    .text = open_text,
};

static const markdown_core_line_ops PARAGRAPH_LINES = {
    .lazy = accepts_lazy,
    .lazy_open = open_lazy,
    .matches = continue_paragraph,
    .reach = continue_reach,
    .close = close_paragraph,
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_PARAGRAPH = {
    .state_size = sizeof(markdown_core_block_identifier_work),
    .name = "paragraph",
    .edit = &PARAGRAPH_EDIT,
    .content = &PARAGRAPH_CONTENT,
    .inlines = &PARAGRAPH_INLINES,
    .candidate = &PARAGRAPH_CANDIDATE,
    .opens = &PARAGRAPH_OPENS,
    .lines = &PARAGRAPH_LINES,
};
