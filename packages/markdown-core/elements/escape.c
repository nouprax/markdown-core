#include "alloc.h"
#include "escape.h"
#include "services.h"
#include "inline_internal.h"
#include "markdown_core_ctype.h"

/* The services this element asks, as `self->needs` holds them: an escaped
 * space is a word space (services.h). */
enum { ESCAPE_WORD_SPACES };
static const char *const ESCAPE_NEEDS[] = {[ESCAPE_WORD_SPACES] = MARKDOWN_CORE_SERVICE_WORD_SPACES, NULL};
#define advance(inline_state) markdown_core_inline_seek((inline_state), markdown_core_inline_offset(inline_state) + 1)

/* The escapes of a slice another element decodes (a link's destination and
 * title, a fence's info string): `buf` loses each backslash before an ASCII
 * punctuation character, in place. */
static void S_strip(const markdown_core_element_instance *self, markdown_core_strbuf *buf) {
    (void)self;
    bufsize_t r, w;

    markdown_core_strbuf_unsettle(buf, 0);
    for (r = 0, w = 0; r < buf->size; ++r) {
        if (buf->ptr[r] == '\\' && markdown_core_ispunct(buf->ptr[r + 1])) {
            r++;
        }

        buf->ptr[w++] = buf->ptr[r];
    }

    markdown_core_strbuf_truncate(buf, w);
}

/* THE ESCAPE (plan symbols-cleanroom.md 23): a backslash and what it
 * escapes. Its tokens: the backslash alone, which is text; an escaped
 * space, a word space; a run of escaped backslashes, read whole where no
 * other construct starts at a backslash; an escaped punctuation
 * character; and a hard break, the backslash and the line's end. */
enum { ESCAPE_LONE, ESCAPE_SPACE, ESCAPE_RUN, ESCAPE_PUNCT, ESCAPE_BREAK, ESCAPE_KINDS };

static markdown_core_state S_token(markdown_core_tokenizer *tokenizer, unsigned kind, uint32_t end) {
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    markdown_core_enter(tokenizer, (uint16_t)(self->token_base + kind));
    markdown_core_consume_to(tokenizer, end);
    markdown_core_exit(tokenizer, (uint16_t)(self->token_base + kind));
    return markdown_core_ok(tokenizer);
}

/* Whether a construct of another element starts at a backslash. */
static bool S_shared(const markdown_core_tokenizer *tokenizer, const markdown_core_element_instance *self) {
    const markdown_core_constructs *text = &tokenizer->text;
    for (uint16_t i = text->offsets['\\']; i < text->offsets['\\' + 1]; i++) {
        if (text->items[i]->context != self) {
            return true;
        }
    }
    return false;
}

static markdown_core_state S_escape(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    markdown_core_escape_work *counts = self->state;
    const markdown_core_inline_state *inline_state = tokenizer->reader;
    const uint32_t start = tokenizer->position, size = tokenizer->size;
    const unsigned char *data = tokenizer->data;
    const int next = markdown_core_code_at(tokenizer, 1);
    if (next == ' ') {
        uint32_t end = start + 1;
        while (end < size && markdown_core_is_space_or_tab(data[end])) {
            end++;
        }
        if (end == size) {
            markdown_core_met_end(tokenizer);
        }
        counts->whitespace += (size_t)(end - start - 1);
        if ((end == size && !MARKDOWN_CORE_NODE_TYPE_INLINE_P(inline_state->owner->kind)) ||
            (end < size && markdown_core_is_line_end(data[end]))) {
            return S_token(tokenizer, ESCAPE_LONE, start + 1);
        }
        return S_token(tokenizer, ESCAPE_SPACE, start + 2);
    }
    if (next != MARKDOWN_CORE_EOF && markdown_core_ispunct((unsigned char)next)) {
        if (next == '\\' && !S_shared(tokenizer, self)) {
            uint32_t end = start;
            while (end + 1 < size && data[end] == '\\' && data[end + 1] == '\\') {
                end += 2;
            }
            if (end + 1 >= size) {
                markdown_core_met_end(tokenizer);
            }
            if (end - start >= 4) {
                return S_token(tokenizer, ESCAPE_RUN, end);
            }
        }
        return S_token(tokenizer, ESCAPE_PUNCT, start + 2);
    }
    if (next == '\r' || next == '\n') {
        uint32_t end = start + 1;
        if (data[end] == '\r') {
            end++;
        }
        if (end < size && data[end] == '\n') {
            end++;
        }
        return S_token(tokenizer, ESCAPE_BREAK, end);
    }
    return S_token(tokenizer, ESCAPE_LONE, start + 1);
}
static const markdown_core_text_construct ESCAPE_CONSTRUCTS[] = {{"\\", {.start = S_escape}}};

static void S_made(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    const markdown_core_element_instance *self = markdown_core_compile_self(compiler);
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    const bufsize_t start = (bufsize_t)token->start, end = (bufsize_t)token->end;
    markdown_core_node *node = NULL;
    switch (token->kind - self->token_base) {
    case ESCAPE_LONE:
        node = make_str(inline_state, start, start, markdown_core_inline_take(inline_state, start, 1));
        break;
    case ESCAPE_SPACE:
        node = make_str(inline_state, start, end - 1, markdown_core_inline_take(inline_state, start, 2));
        if (node) {
            markdown_core_word_space_note(self->needs[ESCAPE_WORD_SPACES], node);
        }
        break;
    case ESCAPE_RUN: {
        const bufsize_t length = (end - start) / 2;
        unsigned char *output = (unsigned char *)markdown_core_alloc((size_t)length + 1, 1);
        if (output) {
            memset(output, '\\', (size_t)length);
            const markdown_core_chunk run = {output, length, 1};
            node = make_str(inline_state, start, end - 1, run);
        }
        break;
    }
    case ESCAPE_PUNCT:
        node = make_str(inline_state, start, end - 1, markdown_core_inline_take(inline_state, start + 1, 1));
        break;
    default:
        node = markdown_core_inline_make_simple_with_state(inline_state, MARKDOWN_CORE_NODE_LINE_BREAK);
        if (node) {
            markdown_core_inline_state_place(inline_state, node, start, end - 1);
        }
        break;
    }
    markdown_core_compile_append(compiler, node);
}
static const markdown_core_compile_ops ESCAPE_TOKENS[ESCAPE_KINDS] = {
    [ESCAPE_LONE] = {.exit = S_made, .plain = true},
    [ESCAPE_SPACE] = {.exit = S_made, .plain = true},
    [ESCAPE_RUN] = {.exit = S_made, .plain = true},
    [ESCAPE_PUNCT] = {.exit = S_made, .plain = true},
    [ESCAPE_BREAK] = {.exit = S_made, .boundary = true}};

static const markdown_core_inline_ops ESCAPE_INLINES = {
    .constructs = ESCAPE_CONSTRUCTS,
    .construct_count = 1,
    .tokens = ESCAPE_TOKENS,
    .token_count = ESCAPE_KINDS,
    .precedence = MARKDOWN_CORE_INLINE_FALLBACK,
};

static const markdown_core_decoder ESCAPE_DECODER = {.strip = S_strip};

static const markdown_core_service ESCAPE_SERVICES[] = {{MARKDOWN_CORE_SERVICE_ESCAPES, &ESCAPE_DECODER}, {0}};

const markdown_core_element MARKDOWN_CORE_ELEMENT_ESCAPE = {
    .provides = ESCAPE_SERVICES,
    .needs = ESCAPE_NEEDS,
    .state_size = sizeof(markdown_core_escape_work),
    .name = "escape",
    .inlines = &ESCAPE_INLINES,
};
