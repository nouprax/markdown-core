#include "line_break.h"
#include "inline_internal.h"
#define advance(inline_state) markdown_core_inline_seek((inline_state), markdown_core_inline_offset(inline_state) + 1)

static markdown_core_node *handle_newline(markdown_core_inline_state *inline_state) {
    markdown_core_inline_push_boundary(inline_state, markdown_core_inline_offset(inline_state) + 1);
    bufsize_t nlpos = markdown_core_inline_offset(inline_state);
    markdown_core_node *brk;
    // skip over cr, crlf, or lf:
    if (markdown_core_inline_peek_at(inline_state, markdown_core_inline_offset(inline_state)) == '\r') {
        advance(inline_state);
    }
    if (markdown_core_inline_peek_at(inline_state, markdown_core_inline_offset(inline_state)) == '\n') {
        advance(inline_state);
    }
    // skip spaces at beginning of line
    markdown_core_inline_skip_spaces(inline_state);
    if (nlpos > 1 && markdown_core_inline_peek_at(inline_state, nlpos - 1) == ' ' &&
        markdown_core_inline_peek_at(inline_state, nlpos - 2) == ' ') {
        brk = markdown_core_inline_make_simple_with_state(inline_state, MARKDOWN_CORE_NODE_LINE_BREAK);
    } else {
        brk = markdown_core_inline_make_simple_with_state(inline_state, MARKDOWN_CORE_NODE_SOFT_BREAK);
    }
    if (brk) {
        // The two spaces of a hard break stay with the text they follow, as
        // upstream also has them, so both forms own exactly the line ending.
        markdown_core_inline_state_place(inline_state, brk, nlpos, nlpos);
        /* The break is the line ending. The spaces skipped after it are the
         * next line's leading whitespace: the parse read them and kept them
         * nowhere, which is what DISCARDED is for, and they belong to the
         * block they were read inside rather than to the break. */
    }
    return brk;
}

static markdown_core_node *match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    return character == '\r' || character == '\n' ? handle_newline(inline_state) : NULL;
}

static const markdown_core_inline_ops LINE_BREAK_INLINES = {
    .precedence = MARKDOWN_CORE_INLINE_TOKEN,
    .match = match,
    .terminates = "\r\n",
    .dispatch = "\r\n",
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
