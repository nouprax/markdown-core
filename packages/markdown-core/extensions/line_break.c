#include "line_break.h"
#include "inline_internal.h"
#define advance(subj) ((subj)->pos += 1)

static markdown_core_node *handle_newline(subject *subj) {
    markdown_core_inline_push_boundary(subj, subj->pos + 1);
    bufsize_t nlpos = subj->pos;
    markdown_core_node *brk;
    // skip over cr, crlf, or lf:
    if (markdown_core_inline_peek_at(subj, subj->pos) == '\r') {
        advance(subj);
    }
    if (markdown_core_inline_peek_at(subj, subj->pos) == '\n') {
        advance(subj);
    }
    // skip spaces at beginning of line
    markdown_core_inline_skip_spaces(subj);
    if (nlpos > 1 && markdown_core_inline_peek_at(subj, nlpos - 1) == ' ' &&
        markdown_core_inline_peek_at(subj, nlpos - 2) == ' ') {
        brk = markdown_core_inline_make_simple_subj(subj, MARKDOWN_CORE_NODE_LINE_BREAK);
    } else {
        brk = markdown_core_inline_make_simple_subj(subj, MARKDOWN_CORE_NODE_SOFT_BREAK);
    }
    if (brk) {
        // The two spaces of a hard break stay with the text they follow, as
        // upstream also has them, so both forms own exactly the line ending.
        markdown_core_inline_parser_place(subj, brk, nlpos, nlpos);
        /* The break is the line ending. The spaces skipped after it are the
         * next line's leading whitespace: the parse read them and kept them
         * nowhere, which is what DISCARDED is for, and they belong to the
         * block they were read inside rather than to the break. */
    }
    return brk;
}

static markdown_core_node *match(const markdown_core_extension *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_parser *subj) {
    return character == '\r' || character == '\n' ? handle_newline(subj) : NULL;
}

const markdown_core_extension MARKDOWN_CORE_EXTENSION_LINE_BREAK = {
    .inline_precedence = MARKDOWN_CORE_INLINE_TOKEN,

    .name = "line_break",
    .match_inline = match,
    .terminates_text = "\r\n",
    .dispatch = "\r\n",
};
