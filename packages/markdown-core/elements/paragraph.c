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

static const markdown_core_node_type PARAGRAPH_JOINS[] = {MARKDOWN_CORE_NODE_REFERENCE, MARKDOWN_CORE_NODE_NONE};

static const markdown_core_edit_ops PARAGRAPH_EDIT = {
    .joins = PARAGRAPH_JOINS,
};

/* A BLOCK IDENTIFIER LINE FOLLOWS A CLOSED BLOCK (element.h,
 * markdown_core_follow): the line names the block before it
 * (block_identifier.c) of the kinds here. */
static const markdown_core_node_type PARAGRAPH_LEADERS[] = {MARKDOWN_CORE_NODE_LIST, MARKDOWN_CORE_NODE_CALLOUT,
                                                            MARKDOWN_CORE_NODE_TABLE, MARKDOWN_CORE_NODE_NONE};
static const markdown_core_follow PARAGRAPH_FOLLOWS[] = {
    {.at = MARKDOWN_CORE_FOLLOW_LINE,
     .kinds = PARAGRAPH_LEADERS,
     .closed = true,
     .writes = markdown_core_block_identifier_writes},
    {0},
};

enum { PARAGRAPH_OPEN, PARAGRAPH_BLOCKS };

/* THE CONTENT (element.h, markdown_core_open_ops `content`): every line
 * nothing else opens on opens a paragraph. */
static markdown_core_state S_content(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return markdown_core_block_found(tokenizer, PARAGRAPH_OPEN);
}

static void S_paragraph_open(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)token;
    (void)index;
    markdown_core_block_turn *turn = markdown_core_block_turned(compiler);
    markdown_core_node *opened =
        open_text(markdown_core_compile_self(compiler), turn->parser, *turn->container, turn->context->reader);
    if (opened) {
        *turn->container = opened;
    }
    turn->opened = opened != NULL;
}

static const markdown_core_block_construct PARAGRAPH_CONTENT_START = {.construct = {.start = S_content}};

static const markdown_core_compile_ops PARAGRAPH_BLOCK_TOKENS[PARAGRAPH_BLOCKS] = {
    [PARAGRAPH_OPEN] = {.enter = S_paragraph_open},
};

static const markdown_core_open_ops PARAGRAPH_OPENS = {
    .tokens = PARAGRAPH_BLOCK_TOKENS,
    .token_count = PARAGRAPH_BLOCKS,
    .content = &PARAGRAPH_CONTENT_START,
};

/* THE CONTINUATION (element.h, markdown_core_line_ops `continuation`). */
static markdown_core_state S_continue(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    /* A block of its kind holds no block a lookahead adds. */
    return markdown_core_block_verdict(
        tokenizer, turn->look
                       ? MARKDOWN_CORE_LINE_OFF
                       : continue_paragraph(markdown_core_self(tokenizer), turn->parser, turn->reader, turn->node));
}

static const markdown_core_line_ops PARAGRAPH_LINES = {
    .continuation = {.start = S_continue},
    .beyond = true,
    .lazy = accepts_lazy,
    .close = close_paragraph,
    .cuts = true,
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_PARAGRAPH = {
    .state_size = sizeof(markdown_core_block_identifier_work),
    .name = "paragraph",
    .edit = &PARAGRAPH_EDIT,
    .follows = PARAGRAPH_FOLLOWS,
    .content = &PARAGRAPH_CONTENT,
    .opens = &PARAGRAPH_OPENS,
    .lines = &PARAGRAPH_LINES,
};
