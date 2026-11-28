#include "embed.h"
#include "text.h"
#include "inline_internal.h"

/* The elements whose state this element reads, as `self->peers` holds them. */
enum { TEXT_EMBED };
static const markdown_core_element *const TEXT_PEERS[] = {[TEXT_EMBED] = &MARKDOWN_CORE_ELEMENT_EMBED, NULL};

markdown_core_node *markdown_core_text_parse(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                             markdown_core_inline_state *inline_state, bufsize_t endpos) {
    markdown_core_text_work *counts = self->state;
    markdown_core_chunk contents;
    bufsize_t startpos;
    /* Disjoint ordinary text slices alone contribute whitespace barriers.
     * Escapes, entities, and opaque tokens have their own token owners. */
    /* THE SLICE'S WHITESPACE BOUNDARY, which is ONE position.
     *
     * `markdown_core_inline_push_boundary` OVERWRITES when the last delimiter
     * is already a boundary (core/inlines.c), and nothing else is pushed while
     * this runs -- so a pass that pushes on every space leaves exactly one
     * entry behind, at the position just past the LAST whitespace byte in
     * [pos, endpos). Whitespace is a space, a tab or a line ending, all of
     * them ASCII, so the walk back from `endpos` tests bytes and decodes
     * nothing: no byte of a multi-byte character is whitespace. */
    bufsize_t boundary = -1, walked = endpos;
    while (walked > inline_state->pos) {
        if (markdown_core_is_whitespace(inline_state->input.data[--walked])) {
            boundary = walked + 1;
            break;
        }
    }
    counts->whitespace += (size_t)(endpos - walked);
    if (boundary >= 0) {
        markdown_core_inline_push_boundary(inline_state, boundary);
    }
    /* Text runs are disjoint, so recording separators costs at most one
     * extra visit per byte, regardless of bracket nesting or digit-run
     * length. No image closer scans its label again. */
    if (self->peers[TEXT_EMBED]) {
        markdown_core_embed_record_text(self->peers[TEXT_EMBED], inline_state, endpos);
    }
    contents = markdown_core_chunk_dup(&inline_state->input, inline_state->pos, endpos - inline_state->pos);
    startpos = inline_state->pos;
    inline_state->pos = endpos;

    // if we're at a newline, strip trailing spaces.
    if (markdown_core_is_line_end(markdown_core_inline_peek_char(inline_state))) {
        markdown_core_chunk_rtrim(&contents);
    }

    markdown_core_node *new_inl = make_str(inline_state, startpos, endpos - 1, contents);
    /* `rtrim` above takes the trailing spaces before a line ending OUT of
     * the literal, and the run's SCOPE still covers them -- so they are the
     * run's, in the role a byte kept nowhere has. Giving them to the block
     * instead left the node covering eight columns and owning three, which
     * is what L5 measures. */
    return new_inl;
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

static const markdown_core_inline_ops TEXT_INLINES = {
    .precedence = MARKDOWN_CORE_INLINE_FALLBACK,
    .text = markdown_core_text_parse,
    .plain = MARKDOWN_CORE_NODE_TEXT,
};

const markdown_core_facet MARKDOWN_CORE_TEXT_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_TEXT = {
    .facets = MARKDOWN_CORE_TEXT_FACETS,
    .peers = TEXT_PEERS,
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

static bool view_literal(markdown_core_node_pool *pool, markdown_core_node *copy, const markdown_core_node *node,
                         bool failed) {
    (void)pool;
    (void)node;
    (void)failed;
    markdown_core_literal_view(copy->as.literal);
    return true;
}

static bool own_literal(markdown_core_node *copy, const markdown_core_node *node) {
    return markdown_core_chunk_own(copy->as.literal, node->as.literal);
}

static void adopt_literal(markdown_core_node *view, markdown_core_node *node) {
    markdown_core_chunk_adopt(view->as.literal, node->as.literal);
}

static bool viewed_literal(markdown_core_node *node) { return markdown_core_chunk_own_viewed(node->as.literal); }

const markdown_core_record_ops MARKDOWN_CORE_RECORD_LITERAL = {
    .size = sizeof(markdown_core_chunk),
    .free = free_literal,
    .view = view_literal,
    .own = own_literal,
    .adopt = adopt_literal,
    .viewed = viewed_literal,
};
