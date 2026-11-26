#include "text.h"
#include "services.h"
#include "inline_internal.h"

static void S_note(const markdown_core_element_instance *self, markdown_core_node *node) {
    (void)self;
    node->flags |= MARKDOWN_CORE_NODE__ESCAPED_SPACE;
}

static void complete_node(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_node *node, int word_depth) {
    (void)self;
    if (node->flags & MARKDOWN_CORE_NODE__ESCAPED_SPACE) {
        if (word_depth > 0) {
            markdown_core_chunk_free(node->as.literal);
            *node->as.literal = markdown_core_chunk_literal("\xC2\xA0");
        }
        node->flags &= ~MARKDOWN_CORE_NODE__ESCAPED_SPACE;
    }
}

static const markdown_core_finish_ops TEXT_FINISH = {
    .complete = complete_node,
};

/* THE DATA (plan symbols-cleanroom.md 23.5): the text no construct read,
 * one Text of its bytes, the spaces before a line's end left out. */
static void S_data(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    markdown_core_chunk contents =
        markdown_core_inline_take(inline_state, (bufsize_t)token->start, (bufsize_t)(token->end - token->start));
    if (token->end < (uint32_t)markdown_core_inline_size(inline_state) &&
        markdown_core_is_line_end(markdown_core_inline_window(inline_state, 0).data[token->end])) {
        markdown_core_chunk_rtrim(&contents);
    }
    markdown_core_compile_append(compiler,
                                 make_str(inline_state, (bufsize_t)token->start, (bufsize_t)token->end - 1, contents));
}

static const markdown_core_compile_ops TEXT_DATA = {.exit = S_data, .plain = true, .verbatim = true};

static const markdown_core_inline_ops TEXT_INLINES = {
    .data = &TEXT_DATA,
    .precedence = MARKDOWN_CORE_INLINE_FALLBACK,
    .plain = MARKDOWN_CORE_NODE_TEXT,
};

const markdown_core_facet MARKDOWN_CORE_TEXT_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_TEXT, .whole = true, .reads = MARKDOWN_CORE_READS_LITERAL},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

static const markdown_core_word_space TEXT_WORD_SPACES = {.note = S_note};
static const markdown_core_service TEXT_SERVICES[] = {{MARKDOWN_CORE_SERVICE_WORD_SPACES, &TEXT_WORD_SPACES}, {0}};

const markdown_core_element MARKDOWN_CORE_ELEMENT_TEXT = {
    .provides = TEXT_SERVICES,
    .facets = MARKDOWN_CORE_TEXT_FACETS,
    .state_size = sizeof(markdown_core_text_work),
    .name = "text",
    .finish = &TEXT_FINISH,
    .inlines = &TEXT_INLINES,
};

/* THE LITERAL RECORD every literal kind holds -- a Text's, an inline code
 * span's, raw HTML's, a comment's, a topic's, a block comment's: one chunk
 * (markdown_core_record_ops). An inline literal views the content it was read
 * from until it settles; a block comment's literal may be pending
 * (markdown_core_literal_set). */
static void free_literal(markdown_core_node_pool *pool, markdown_core_node *node) {
    (void)pool;
    markdown_core_chunk_free(node->as.literal);
}

static bool viewed_literal(markdown_core_node *node) { return markdown_core_chunk_own_viewed(node->as.literal); }

const markdown_core_record_ops MARKDOWN_CORE_RECORD_LITERAL = {
    .size = sizeof(markdown_core_chunk),
    .free = free_literal,
    .viewed = viewed_literal,
};
