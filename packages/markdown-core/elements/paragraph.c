#include "paragraph.h"
#include "block_internal.h"

#include "block_identifier.h"

/* A paragraph closing inside `parent`, past the items its text opened with
 * (`cuts`, blocks.c, THE FRONT CUT), yields itself less a block identifier
 * at the back of its text. */
static bool close_paragraph(const markdown_core_element_instance *self, markdown_core_parser *parser,
                            markdown_core_node *paragraph, markdown_core_node *parent, markdown_core_reader *reader) {
    markdown_core_block_attach_paragraph_identifier(self->state, parser, paragraph, parent, reader);
    return true;
}

static int continue_paragraph(const markdown_core_element_instance *self, markdown_core_parser *parser,
                              markdown_core_reader *reader, markdown_core_node *container) {
    return !reader->blank;
}
static bool accepts_lazy(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         markdown_core_node *node, markdown_core_reader *reader) {
    (void)reader;
    (void)self;
    (void)parser;
    (void)node;
    return true;
}
static markdown_core_node *open_lazy(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                     markdown_core_node *node, markdown_core_reader *reader) {
    (void)self;
    return node;
}

/* A text line no block claims opens a paragraph: the dialect's text block. */
static markdown_core_node *open_text(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                     markdown_core_node *container, markdown_core_reader *reader) {
    container = markdown_core_block_parent_for(parser, container, MARKDOWN_CORE_NODE_PARAGRAPH);
    if (!container) {
        return NULL;
    }
    if (markdown_core_block_attach_identifier_line(self, parser, container, reader) || parser->error) {
        return NULL;
    }
    container =
        markdown_core_parser_add_child_validated(parser, container, MARKDOWN_CORE_NODE_PARAGRAPH, reader->nonspace + 1);
    if (container) {
        markdown_core_reader_skip(reader);
    }
    return container;
}

static const markdown_core_content_ops PARAGRAPH_CONTENT = {
    .mode = MARKDOWN_CORE_CONTENT_PROSE,
    .inlines = true,
    .paragraph = true,
};

/* A BLOCK IDENTIFIER LINE FOLLOWS A CLOSED BLOCK (element.h,
 * markdown_core_follow): the line names the block before it
 * (block_identifier.c) of the kinds here. */
static const markdown_core_node_type PARAGRAPH_LEADERS[] = {MARKDOWN_CORE_NODE_LIST, MARKDOWN_CORE_NODE_CALLOUT,
                                                            MARKDOWN_CORE_NODE_TABLE, MARKDOWN_CORE_NODE_NONE};
static const markdown_core_follow PARAGRAPH_FOLLOWS[] = {
    {.at = MARKDOWN_CORE_FOLLOW_LINE, .kinds = PARAGRAPH_LEADERS, .closed = true},
    {0},
};

static const markdown_core_candidate_ops PARAGRAPH_CANDIDATE = {
    .feed = markdown_core_block_feed_identifier_line,
    .reads = markdown_core_block_identifier_reads,
};

static const markdown_core_open_ops PARAGRAPH_OPENS = {
    .text = open_text,
};

static const markdown_core_line_ops PARAGRAPH_LINES = {
    .lazy = accepts_lazy,
    .lazy_open = open_lazy,
    .matches = continue_paragraph,
    .close = close_paragraph,
    .cuts = true,
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_PARAGRAPH = {
    .state_size = sizeof(markdown_core_block_identifier_work),
    .name = "paragraph",
    .follows = PARAGRAPH_FOLLOWS,
    .content = &PARAGRAPH_CONTENT,
    .candidate = &PARAGRAPH_CANDIDATE,
    .opens = &PARAGRAPH_OPENS,
    .lines = &PARAGRAPH_LINES,
};
