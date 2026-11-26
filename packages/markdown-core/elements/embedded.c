#include "embedded.h"
#include "label.h"
#include "services.h"
#include "inline_internal.h"
#include "block_internal.h"

/* An image's label, opened by a `![`, whose `!` stays as text when the
 * image replaces it (label.h). */
static const markdown_core_label_shape MARKDOWN_CORE_EMBEDDED_BRACKET = {
    .width = 2, .kept = 1, .mark = 1, .keeps = true, .closer = ']', .kind = MARKDOWN_CORE_KIND_EMBEDDED};
static bool dimension_component(const unsigned char *s, bufsize_t *pos, bufsize_t end, int32_t *value, size_t *work) {
    if (*pos == end || s[*pos] < '1' || s[*pos] > '9') {
        return false;
    }
    int32_t number = 0;
    while (*pos < end && s[*pos] >= '0' && s[*pos] <= '9') {
        (*work)++;
        int digit = s[(*pos)++] - '0';
        if (number > (INT32_MAX - digit) / 10) {
            return false;
        }
        number = number * 10 + digit;
    }
    *value = number;
    return true;
}

static bool S_parse(const markdown_core_element_instance *self, markdown_core_chunk label, bufsize_t suffix,
                    bufsize_t separator_length, markdown_core_dimensions *value, size_t *work) {
    (void)self;
    bufsize_t pos = suffix + separator_length;
    markdown_core_dimensions parsed = {0};
    (*work)++;
    if ((suffix > 0 && markdown_core_is_whitespace(label.data[suffix - 1])) ||
        !dimension_component(label.data, &pos, label.len, &parsed.width, work)) {
        return false;
    }
    if (pos < label.len && label.data[pos] == 'x') {
        int32_t height;
        pos++;
        if (!dimension_component(label.data, &pos, label.len, &height, work)) {
            return false;
        }
        parsed.height = (markdown_core_optional_i64){true, height};
    }
    if (pos != label.len) {
        return false;
    }
    *value = parsed;
    return true;
}

/* THE IMAGE'S TEXT (plan symbols-cleanroom.md 23.12): a `![` opens a
 * label (label.h) where the dialect closes one, and any other `!` is its
 * own text. */
enum { EMBEDDED_BANG, EMBEDDED_KINDS };

static markdown_core_state S_start(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    const markdown_core_inline_state *inline_state = tokenizer->reader;
    const uint32_t at = tokenizer->position;
    if (markdown_core_label_closes(self, inline_state->dialect) && markdown_core_code_at(tokenizer, 1) == '[' &&
        markdown_core_code_at(tokenizer, 2) != '^') {
        return markdown_core_label_open(tokenizer, &MARKDOWN_CORE_EMBEDDED_BRACKET);
    }
    markdown_core_enter(tokenizer, (uint16_t)(self->token_base + EMBEDDED_BANG));
    markdown_core_consume_to(tokenizer, at + 1);
    markdown_core_exit(tokenizer, (uint16_t)(self->token_base + EMBEDDED_BANG));
    return markdown_core_ok(tokenizer);
}

/* The suffix of an image's label: its last pipe at the label's own depth
 * in text, and the dimensions after it. */
static bool S_measure(const markdown_core_element_instance *self, markdown_core_tokenizer *tokenizer,
                      const markdown_core_opener *label, markdown_core_dimensions *value, uint32_t *field) {
    markdown_core_embedded_work *counts = self->state;
    const markdown_core_inline_state *inline_state = tokenizer->reader;
    if (inline_state->error || inline_state->owner_parser->error) {
        return false;
    }
    markdown_core_events *events = &tokenizer->events;
    const size_t to = label->close;
    const unsigned char *data = tokenizer->data;
    bufsize_t pipe = -1;
    size_t last = SIZE_MAX;
    for (size_t at = (size_t)label->enter + 2; at < to; at = markdown_core_label_step(tokenizer, at, to)) {
        if (events->items[at].phase != MARKDOWN_CORE_ENTER) {
            continue;
        }
        last = at;
        const markdown_core_token *token = markdown_core_event_token(events, at);
        if (token->kind != MARKDOWN_CORE_TOKEN_DATA) {
            continue;
        }
        for (uint32_t i = token->start; i < token->end; i++) {
            counts->dimensions++;
            if (data[i] == '|') {
                pipe = (bufsize_t)i;
            }
        }
    }
    const bufsize_t position = (bufsize_t)label->position;
    const bufsize_t end = (bufsize_t)markdown_core_event_token(events, to)->start;
    const bufsize_t suffix = pipe >= 0 ? pipe : position;
    const markdown_core_chunk text = {(unsigned char *)data + position, end - position, 0};
    if (!S_parse(self, text, suffix - position, pipe >= 0 ? 1 : 0, value, &counts->dimensions)) {
        return false;
    }
    /* A successful suffix contains only ordinary ASCII text and belongs to
     * the body's last text at its own depth, which it leaves. */
    markdown_core_token *tail = last == SIZE_MAX ? NULL : markdown_core_event_token(events, last);
    assert(tail && tail->kind == MARKDOWN_CORE_TOKEN_DATA && tail->end == (uint32_t)end &&
           tail->end - tail->start >= (uint32_t)(end - suffix));
    tail->end = (uint32_t)suffix;
    if (tail->end == tail->start) {
        tail->kind = MARKDOWN_CORE_TOKEN_NONE;
    }
    *field = (uint32_t)(suffix + (pipe >= 0));
    return true;
}

static void S_bang(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    markdown_core_compile_text(compiler, (bufsize_t)token->start, (bufsize_t)token->end);
}

static const markdown_core_text_construct EMBEDDED_CONSTRUCTS[] = {{"!", MARKDOWN_CORE_LABEL_CONSTRUCT(S_start)}};
static const markdown_core_compile_ops EMBEDDED_TOKENS[EMBEDDED_KINDS] = {
    [EMBEDDED_BANG] = {.exit = S_bang, .plain = true}};

static const markdown_core_inline_ops EMBEDDED_INLINES = {
    .constructs = EMBEDDED_CONSTRUCTS,
    .construct_count = sizeof(EMBEDDED_CONSTRUCTS) / sizeof(EMBEDDED_CONSTRUCTS[0]),
    .tokens = EMBEDDED_TOKENS,
    .token_count = EMBEDDED_KINDS,
    .precedence = MARKDOWN_CORE_INLINE_FALLBACK,
};

static const markdown_core_dimension_ops EMBEDDED_DIMENSIONS = {.parse = S_parse, .measure = S_measure};
static const markdown_core_service EMBEDDED_SERVICES[] = {{MARKDOWN_CORE_SERVICE_DIMENSIONS, &EMBEDDED_DIMENSIONS},
                                                          {0}};

const markdown_core_element MARKDOWN_CORE_ELEMENT_EMBEDDED = {
    .provides = EMBEDDED_SERVICES,
    .state_size = sizeof(markdown_core_embedded_work),
    .name = "embedded",
    .inlines = &EMBEDDED_INLINES,
};
