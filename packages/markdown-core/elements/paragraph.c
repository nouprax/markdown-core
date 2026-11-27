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

/* THE CONTENT (element.h, markdown_core_open_ops `content`): every line
 * nothing else opens on opens a paragraph, the dialect's text block, in
 * the nearest container that holds one, but an identifier line, which
 * names the block before it when the boundary follows (block_identifier.c,
 * THE IDENTIFIER LINE), read by a check of the document's reading. */
enum { PARAGRAPH_OPEN, PARAGRAPH_NAMES, PARAGRAPH_BLOCKS };

typedef struct {
    markdown_core_node *parent;
    markdown_core_block_identifier_line_found line;
} paragraph_found;
MARKDOWN_CORE_BLOCK_FOUND_FITS(paragraph_found, paragraph_found_fits);

/* The next line, as the look reads it once: blank is the boundary. */
static markdown_core_state S_names_line(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return markdown_core_nok(tokenizer);
}

static markdown_core_state S_names_end(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return markdown_core_block_turn_of(tokenizer)->parser->look->blanks > 0 ? markdown_core_ok(tokenizer)
                                                                            : markdown_core_nok(tokenizer);
}

static markdown_core_state S_names_next(markdown_core_tokenizer *tokenizer, int code) {
    return markdown_core_look_next(tokenizer, markdown_core_state_of(S_names_line), markdown_core_state_of(S_names_end),
                                   code);
}

static const markdown_core_construct PARAGRAPH_BOUNDARY = {.start = S_names_next};

static markdown_core_state S_names_read(markdown_core_tokenizer *tokenizer, bool bounded) {
    markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    if (turn->parser->error) {
        return markdown_core_nok(tokenizer);
    }
    return markdown_core_block_found(tokenizer, bounded ? PARAGRAPH_NAMES : PARAGRAPH_OPEN);
}

static markdown_core_state S_bounded(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return S_names_read(tokenizer, true);
}

static markdown_core_state S_unbounded(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return S_names_read(tokenizer, false);
}

static markdown_core_state S_content(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    markdown_core_parser *parser = turn->parser;
    paragraph_found *found = MARKDOWN_CORE_BLOCK_FOUND(turn->start, paragraph_found);
    /* The content opens: the blocks the paragraph's container closes on
     * close as it is found. */
    found->parent = markdown_core_block_parent_for(parser, turn->context->container, MARKDOWN_CORE_NODE_PARAGRAPH);
    if (!found->parent) {
        return markdown_core_nok(tokenizer);
    }
    bool bounded = false;
    if (!markdown_core_block_identifier_line(markdown_core_self(tokenizer), parser, found->parent,
                                             turn->context->reader, &found->line, &bounded)) {
        return parser->error ? markdown_core_nok(tokenizer) : markdown_core_block_found(tokenizer, PARAGRAPH_OPEN);
    }
    if (bounded) {
        return markdown_core_block_found(tokenizer, PARAGRAPH_NAMES);
    }
    const markdown_core_look_ask ask = {.container = found->parent,
                                        .kind = MARKDOWN_CORE_NODE_PARAGRAPH,
                                        .construct = &PARAGRAPH_BOUNDARY,
                                        .once = true};
    return markdown_core_look(tokenizer, &ask, markdown_core_state_of(S_bounded), markdown_core_state_of(S_unbounded),
                              code);
}

static void S_paragraph_open(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)token;
    (void)index;
    markdown_core_block_turn *turn = markdown_core_block_turned(compiler);
    markdown_core_reader *reader = turn->context->reader;
    markdown_core_node *opened = markdown_core_parser_add_child_validated(
        turn->parser, MARKDOWN_CORE_BLOCK_FOUND(turn->start, paragraph_found)->parent, MARKDOWN_CORE_NODE_PARAGRAPH,
        reader->nonspace + 1);
    if (opened) {
        markdown_core_reader_skip(reader);
        *turn->container = opened;
    }
    turn->opened = opened != NULL;
}

/* The identifier line names its owner, and opens nothing; a paragraph when
 * it could not. */
static void S_paragraph_names(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    markdown_core_block_turn *turn = markdown_core_block_turned(compiler);
    if (markdown_core_block_identifier_line_attach(turn->parser, turn->context->reader,
                                                   &MARKDOWN_CORE_BLOCK_FOUND(turn->start, paragraph_found)->line) ||
        turn->parser->error) {
        turn->opened = false;
        return;
    }
    S_paragraph_open(compiler, token, index);
}

static const markdown_core_block_construct PARAGRAPH_CONTENT_START = {.construct = {.start = S_content}};

static const markdown_core_compile_ops PARAGRAPH_BLOCK_TOKENS[PARAGRAPH_BLOCKS] = {
    [PARAGRAPH_OPEN] = {.enter = S_paragraph_open},
    [PARAGRAPH_NAMES] = {.enter = S_paragraph_names},
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
