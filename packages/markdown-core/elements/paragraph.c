#include "paragraph.h"
#include "block_internal.h"

#include "block_identifier.h"
#include "heading.h"
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

static const markdown_core_node_type PARAGRAPH_JOINS[] = {MARKDOWN_CORE_NODE_REFERENCE, MARKDOWN_CORE_NODE_NONE};

static const markdown_core_edit_ops PARAGRAPH_EDIT = {
    .joins = PARAGRAPH_JOINS,
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
    .edit = &PARAGRAPH_EDIT,
    .content = &PARAGRAPH_CONTENT,
    .inlines = &PARAGRAPH_INLINES,
    .candidate = &PARAGRAPH_CANDIDATE,
    .opens = &PARAGRAPH_OPENS,
    .lines = &PARAGRAPH_LINES,
};
