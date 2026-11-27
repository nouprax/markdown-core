#include "thematic_break.h"
#include "block_internal.h"

/* WHERE A BREAK FAILED ON THE LINE (the element's parse record): a start
 * at a byte at or before it fails there too, for every container the line
 * opens asks again at a later byte ("- - - a" asks at each marker), so the
 * line is read once. It holds for the visit of the line it was noted on,
 * the line starting at `base` (a line that grows is visited again). */
typedef struct {
    size_t visit;
    uint32_t base, kill;
} thematic_break_work;

enum { THEMATIC_BREAK_OPEN, THEMATIC_BREAK_SEQUENCE, THEMATIC_BREAK_BLOCKS };

/* The break opens as its token is compiled: it's only now that we know the
 * line is not part of a setext heading. */
static void S_open(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    markdown_core_block_turn *turn = markdown_core_block_turned(compiler);
    *turn->container = markdown_core_parser_add_child(turn->parser, *turn->container, MARKDOWN_CORE_NODE_THEMATIC_BREAK,
                                                      (int)(token->start - turn->base) + 1);
    if (!*turn->container) {
        return;
    }
    turn->opened = true;
}

/* THE BREAK (micromark's thematicBreak): three or more of one marker, `*`,
 * `-` or `_`, spaces between them and after, to the line's end. The frame
 * keeps the marker and how many of it the break read. */
enum { BREAK_MARKER, BREAK_COUNT };

static markdown_core_state S_at_break(markdown_core_tokenizer *tokenizer, int code);

static markdown_core_state S_sequence(markdown_core_tokenizer *tokenizer, int code) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    if (code == (int)words[BREAK_MARKER]) {
        markdown_core_consume(tokenizer);
        words[BREAK_COUNT]++;
        return markdown_core_state_of(S_sequence);
    }
    markdown_core_exit(tokenizer, markdown_core_block_token(tokenizer, THEMATIC_BREAK_SEQUENCE));
    if (markdown_core_code_space(code)) {
        return markdown_core_spaces(tokenizer, markdown_core_state_of(S_at_break), MARKDOWN_CORE_TOKEN_SPACE,
                                    UINT32_MAX, code);
    }
    return S_at_break(tokenizer, code);
}

static markdown_core_state S_at_break(markdown_core_tokenizer *tokenizer, int code) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    if (code == (int)words[BREAK_MARKER]) {
        markdown_core_enter(tokenizer, markdown_core_block_token(tokenizer, THEMATIC_BREAK_SEQUENCE));
        return S_sequence(tokenizer, code);
    }
    if (words[BREAK_COUNT] >= 3 && markdown_core_code_ending(code)) {
        markdown_core_exit(tokenizer, markdown_core_block_token(tokenizer, THEMATIC_BREAK_OPEN));
        return markdown_core_ok(tokenizer);
    }
    thematic_break_work *work = ((const markdown_core_element_instance *)markdown_core_self(tokenizer))->state;
    work->kill = tokenizer->position;
    return markdown_core_nok(tokenizer);
}

static markdown_core_state S_before(markdown_core_tokenizer *tokenizer, int code) {
    thematic_break_work *work = ((const markdown_core_element_instance *)markdown_core_self(tokenizer))->state;
    if ((code != '*' && code != '-' && code != '_') || work->kill > tokenizer->position) {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_frame_local(tokenizer)[BREAK_MARKER] = (uint32_t)code;
    markdown_core_enter(tokenizer, markdown_core_block_token(tokenizer, THEMATIC_BREAK_OPEN));
    return S_at_break(tokenizer, code);
}

static markdown_core_state S_start(markdown_core_tokenizer *tokenizer, int code) {
    const block_start_context *context = markdown_core_block_turn_of(tokenizer)->context;
    if (context->paragraph && !context->all_matched) {
        return markdown_core_nok(tokenizer);
    }
    thematic_break_work *work = ((const markdown_core_element_instance *)markdown_core_self(tokenizer))->state;
    const size_t visit = markdown_core_parser_visit(markdown_core_block_turn_of(tokenizer)->parser);
    if (work->visit != visit || work->base != tokenizer->base) {
        *work = (thematic_break_work){visit, tokenizer->base, 0};
    }
    markdown_core_block_turn_of(tokenizer)->start->kind = MARKDOWN_CORE_NODE_THEMATIC_BREAK;
    return markdown_core_spaces(tokenizer, markdown_core_state_of(S_before), MARKDOWN_CORE_TOKEN_SPACE, UINT32_MAX,
                                code);
}

static const markdown_core_block_construct THEMATIC_BREAK_STARTS[] = {
    {.bytes = "*-_", .indent = 3, .construct = {.start = S_start}},
};

static const markdown_core_compile_ops THEMATIC_BREAK_BLOCK_TOKENS[THEMATIC_BREAK_BLOCKS] = {
    [THEMATIC_BREAK_OPEN] = {.enter = S_open},
    [THEMATIC_BREAK_SEQUENCE] = {0},
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
    .state_size = sizeof(thematic_break_work),
    .opens = &THEMATIC_BREAK_OPENS,
    .lines = &THEMATIC_BREAK_LINES,
};
