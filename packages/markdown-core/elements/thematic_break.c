#include "thematic_break.h"
#include "block_internal.h"

static int S_scan_thematic_break(markdown_core_reader *reader, bufsize_t offset, bufsize_t *kill_pos) {
    bufsize_t i;
    char c;
    char nextc = '\0';
    int count;
    i = offset;
    c = markdown_core_reader_peek(reader, i);
    if (!(c == '*' || c == '_' || c == '-')) {
        *kill_pos = i;
        return 0;
    }
    count = 1;
    while ((nextc = markdown_core_reader_peek(reader, ++i))) {
        if (nextc == c) {
            count++;
        } else if (!markdown_core_is_space_or_tab(nextc)) {
            break;
        }
    }
    if (count >= 3 && (nextc == '\r' || nextc == '\n')) {
        return (i - offset) + 1;
    } else {
        *kill_pos = i;
        return 0;
    }
}

enum { THEMATIC_BREAK_OPEN, THEMATIC_BREAK_BLOCKS };

/* The break opens as its token is compiled: it's only now that we know the
 * line is not part of a setext heading. */
static void S_open(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)token;
    (void)index;
    markdown_core_block_turn *turn = markdown_core_block_turned(compiler);
    markdown_core_reader *reader = turn->context->reader;
    *turn->container = markdown_core_parser_add_child(turn->parser, *turn->container, MARKDOWN_CORE_NODE_THEMATIC_BREAK,
                                                      reader->nonspace + 1);
    if (!*turn->container) {
        return;
    }
    markdown_core_reader_end(reader);
    turn->opened = true;
}

static markdown_core_state S_start(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    block_start_context *context = markdown_core_block_turn_of(tokenizer)->context;
    block_start *start = markdown_core_block_turn_of(tokenizer)->start;
    if (context->paragraph && !context->all_matched) {
        return markdown_core_nok(tokenizer);
    }
    if (context->thematic_kill > context->reader->nonspace ||
        !(start->matched =
              S_scan_thematic_break(context->reader, context->reader->nonspace, &context->thematic_kill))) {
        return markdown_core_nok(tokenizer);
    }
    start->kind = MARKDOWN_CORE_NODE_THEMATIC_BREAK;
    return markdown_core_block_found(tokenizer, THEMATIC_BREAK_OPEN);
}

static const markdown_core_block_construct THEMATIC_BREAK_STARTS[] = {
    {.bytes = "*-_", .indent = 3, .construct = {.start = S_start}},
};

static const markdown_core_compile_ops THEMATIC_BREAK_BLOCK_TOKENS[THEMATIC_BREAK_BLOCKS] = {
    [THEMATIC_BREAK_OPEN] = {.enter = S_open},
};

static const markdown_core_open_ops THEMATIC_BREAK_OPENS = {
    .starts = THEMATIC_BREAK_STARTS,
    .start_count = 1,
    .tokens = THEMATIC_BREAK_BLOCK_TOKENS,
    .token_count = THEMATIC_BREAK_BLOCKS,
};

static const markdown_core_line_ops THEMATIC_BREAK_LINES = {
    .blank_opaque = true,
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_THEMATIC_BREAK = {
    .name = "thematic_break",
    .opens = &THEMATIC_BREAK_OPENS,
    .lines = &THEMATIC_BREAK_LINES,
};
