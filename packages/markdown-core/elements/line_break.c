#include "line_break.h"
#include "inline_internal.h"
#define advance(inline_state) markdown_core_inline_seek((inline_state), markdown_core_inline_offset(inline_state) + 1)

/* THE LINE'S END (plan symbols-cleanroom.md 23): a CR, an LF or a CRLF,
 * and the spaces that begin the next line, which it reads and keeps
 * nowhere. The break is hard when two spaces end the line before it. */
enum { LINE_BREAK_SOFT, LINE_BREAK_HARD };
static markdown_core_state S_spaces(markdown_core_tokenizer *tokenizer, int code) {
    if (code == ' ' || code == '\t') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_spaces);
    }
    return markdown_core_ok(tokenizer);
}
static markdown_core_state S_feed(markdown_core_tokenizer *tokenizer, int code) {
    if (code == '\n') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_spaces);
    }
    return S_spaces(tokenizer, code);
}
static markdown_core_state S_end(markdown_core_tokenizer *tokenizer, int code) {
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    const bool hard = tokenizer->position > 1 && markdown_core_code_at(tokenizer, -1) == ' ' &&
                      markdown_core_code_at(tokenizer, -2) == ' ';
    markdown_core_enter(tokenizer, (uint16_t)(self->token_base + (hard ? LINE_BREAK_HARD : LINE_BREAK_SOFT)));
    markdown_core_consume(tokenizer);
    markdown_core_exit(tokenizer, (uint16_t)(self->token_base + (hard ? LINE_BREAK_HARD : LINE_BREAK_SOFT)));
    return markdown_core_state_of(code == '\r' ? S_feed : S_spaces);
}
static const markdown_core_text_construct LINE_BREAK_CONSTRUCTS[] = {{"\r\n", {.start = S_end}}};

static void S_break(markdown_core_compiler *compiler, const markdown_core_token *token, bool hard) {
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    markdown_core_node *brk = markdown_core_inline_make_simple_with_state(
        inline_state, hard ? MARKDOWN_CORE_NODE_LINE_BREAK : MARKDOWN_CORE_NODE_SOFT_BREAK);
    if (brk) {
        markdown_core_inline_state_place(inline_state, brk, (bufsize_t)token->start, (bufsize_t)token->start);
    }
    markdown_core_compile_append(compiler, brk);
}
static void S_soft(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    S_break(compiler, token, false);
}
static void S_hard(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    S_break(compiler, token, true);
}
static const markdown_core_compile_ops LINE_BREAK_TOKENS[] = {[LINE_BREAK_SOFT] = {.exit = S_soft, .boundary = true},
                                                              [LINE_BREAK_HARD] = {.exit = S_hard, .boundary = true}};

static const markdown_core_inline_ops LINE_BREAK_INLINES = {
    .constructs = LINE_BREAK_CONSTRUCTS,
    .construct_count = 1,
    .tokens = LINE_BREAK_TOKENS,
    .token_count = 2,
    .precedence = MARKDOWN_CORE_INLINE_TOKEN,
};

/* A break reads as a `-` in a name (element.h, markdown_core_facet). */
static bool S_words(const markdown_core_node *node, const markdown_core_words *words) {
    (void)node;
    return words->text(words->context, (const unsigned char *)"-", 1);
}

const markdown_core_facet MARKDOWN_CORE_LINE_BREAK_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_SOFT_BREAK, .words = S_words},
    {.kind = MARKDOWN_CORE_KIND_LINE_BREAK, .words = S_words},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_LINE_BREAK = {
    .facets = MARKDOWN_CORE_LINE_BREAK_FACETS,
    .name = "line_break",
    .inlines = &LINE_BREAK_INLINES,
};
