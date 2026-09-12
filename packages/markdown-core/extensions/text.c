#include "media.h"
#include "text.h"
#include "inline_internal.h"
#define advance(subj) ((subj)->pos += 1)

static int any_extension_dispatches(markdown_core_parser *parser, unsigned char c) {
    for (size_t i = parser->inline_dispatch_offsets[c]; i < parser->inline_dispatch_offsets[c + 1]; i++) {
        if (parser->inline_dispatch[i] != &MARKDOWN_CORE_EXTENSION_TEXT) {
            return 1;
        }
    }

    return 0;
}

static markdown_core_node *handle_backslash(markdown_core_parser *parser, subject *subj) {
    bufsize_t start = subj->pos;
    /* The line frame BEFORE anything is consumed. The hard-break arm below
     * needs it, and reading it after `markdown_core_inline_skip_line_end` would be right only
     * because `markdown_core_inline_skip_line_end` happens not to advance the frame -- an accident,
     * not a contract. `handle_newline` captures its frame first for the same
     * reason, and the two arms must not merely look symmetric. */
    advance(subj);
    unsigned char nextchar = markdown_core_inline_peek_char(subj);
    if (nextchar == ' ') {
        /* A trailing whitespace run cannot belong to a completed script.
         * Leave it to the inherited text/line-ending scanner, including its
         * trimming and hard-break rules. These lookaheads are disjoint: each
         * begins after its own backslash and ends before the next token. */
        bufsize_t end = subj->pos;
        while (end < subj->input.len && !markdown_core_is_line_end(markdown_core_inline_peek_at(subj, end)) &&
               markdown_core_isspace(markdown_core_inline_peek_at(subj, end))) {
            end++;
            parser->whitespace_work++;
        }
        if ((end == subj->input.len && !MARKDOWN_CORE_NODE_TYPE_INLINE_P(subj->owner->kind)) ||
            (end < subj->input.len && markdown_core_is_line_end(markdown_core_inline_peek_at(subj, end)))) {
            return make_str(subj, start, start, markdown_core_chunk_literal("\\"));
        }
        advance(subj);
        markdown_core_node *escaped = make_str(subj, start, subj->pos - 1, markdown_core_chunk_literal("\\ "));
        if (escaped) {
            /* Contextual escape token: inline completion decodes it once the
             * delimiter/bracket engine has established its semantic owner. */
            escaped->flags |= MARKDOWN_CORE_NODE__ESCAPED_SPACE;
        }
        return escaped;
    }
    if ((parser->backslash_ispunct ? parser->backslash_ispunct : markdown_core_ispunct)(nextchar)) {
        if (nextchar == '\\' && !any_extension_dispatches(parser, '\\')) {
            bufsize_t end = start;
            while (end + 1 < subj->input.len && subj->input.data[end] == '\\' && subj->input.data[end + 1] == '\\') {
                end += 2;
            }
            if (end - start >= 4) {
                bufsize_t output_len = (end - start) / 2;
                unsigned char *output = (unsigned char *)subj->mem->calloc((size_t)output_len + 1, 1);
                if (output) {
                    markdown_core_chunk contents = {output, output_len, 1};
                    markdown_core_node *run;
                    memset(output, '\\', (size_t)output_len);
                    subj->pos = end;
                    run = make_str(subj, start, end - 1, contents);
                    /* One escape per PAIR: the first backslash of each is the
                     * escape and reaches no literal, the second is the byte the
                     * literal is made of. */
                    return run;
                }
            }
        }
        // only ascii symbols and newline can be escaped
        advance(subj);
        {
            markdown_core_node *escaped =
                make_str(subj, subj->pos - 2, subj->pos - 1, markdown_core_chunk_dup(&subj->input, subj->pos - 1, 1));
            return escaped;
        }
    } else if (!markdown_core_inline_is_eof(subj) && markdown_core_inline_skip_line_end(subj)) {
        markdown_core_inline_push_boundary(subj, subj->pos);
        // A backslash hard break CONSUMES a line ending, so the subject has to
        // be told, exactly as handle_newline tells it. It was not, so every node
        // after such a break kept the break's own line and a column measured
        // from the wrong line's start: `foo\` / `bar` reported Text 1:6..1:8 --
        // three columns that do not exist on a four-character line 1.
        // cmark-gfm reports the same numbers, so upstream cannot be the oracle.
        //
        // The node's extent is the bytes that SPELL it: the backslash and the
        // line ending it escapes. The backslash belonged to no node at all
        // before this, so the break is not taking it from anyone.
        markdown_core_node *hard = markdown_core_inline_make_simple_subj(subj, MARKDOWN_CORE_NODE_LINE_BREAK);
        if (hard) {
            /* The break's extent is the backslash and the line ending it
             * escapes; `subj->pos` is one past that ending's last byte, CR, LF
             * or CRLF alike. Projected from the two offsets, so the frame
             * captured before the consume is only the fallback's. */
            markdown_core_inline_parser_place(subj, hard, start, subj->pos - 1);
        }
        return hard;
    } else {
        return make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("\\"));
    }
}

static markdown_core_node *handle_entity(subject *subj) {
    markdown_core_strbuf ent = MARKDOWN_CORE_BUF_INIT(subj->mem);
    bufsize_t len;

    advance(subj);

    len = houdini_unescape_ent(&ent, subj->input.data + subj->pos, subj->input.len - subj->pos);

    if (len == 0) {
        markdown_core_node *literal = make_str(subj, subj->pos - 1, subj->pos - 1, markdown_core_chunk_literal("&"));
        /* Not an entity: the `&` IS the literal, so it is content. */
        return literal;
    }

    subj->pos += len;
    if (ent.oom) {
        subj->oom = 1;
    }
    return make_str(subj, subj->pos - 1 - len, subj->pos - 1, markdown_core_chunk_buf_detach(&ent));
}

static markdown_core_node *match(const markdown_core_extension *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_parser *subj) {
    if (character == '\\') {
        return handle_backslash(parser, subj);
    }
    if (character == '&') {
        return handle_entity(subj);
    }
    return NULL;
}
markdown_core_node *markdown_core_text_parse(markdown_core_parser *parser, subject *subj, bufsize_t endpos) {
    markdown_core_chunk contents;
    bufsize_t startpos;
    /* Disjoint ordinary text slices alone contribute whitespace barriers.
     * Escapes, entities, and opaque tokens have their own token owners. */
    for (bufsize_t at = subj->pos; at < endpos;) {
        int32_t scalar = 0;
        int width = markdown_core_utf8proc_iterate(subj->input.data + at, endpos - at, &scalar);
        parser->whitespace_work++;
        if (markdown_core_utf8proc_is_space(scalar)) {
            markdown_core_inline_push_boundary(subj, at + width);
        }
        at += width > 0 ? width : 1;
    }
    /* Text runs are disjoint, so recording separators costs at most one
     * extra visit per byte, regardless of bracket nesting or digit-run
     * length. No image closer scans its label again. */
    markdown_core_media_record_text(parser, subj, endpos);
    contents = markdown_core_chunk_dup(&subj->input, subj->pos, endpos - subj->pos);
    startpos = subj->pos;
    subj->pos = endpos;

    // if we're at a newline, strip trailing spaces.
    if (markdown_core_is_line_end(markdown_core_inline_peek_char(subj))) {
        markdown_core_chunk_rtrim(&contents);
    }

    markdown_core_node *new_inl = make_str(subj, startpos, endpos - 1, contents);
    /* `rtrim` above takes the trailing spaces before a line ending OUT of
     * the literal, and the run's SCOPE still covers them -- so they are the
     * run's, in the role a byte kept nowhere has. Giving them to the block
     * instead left the node covering eight columns and owning three, which
     * is what L5 measures. */
    return new_inl;
}
static void complete_inline(markdown_core_parser *parser, markdown_core_node *node, int word_depth) {
    if (node->flags & MARKDOWN_CORE_NODE__ESCAPED_SPACE) {
        if (word_depth > 0) {
            markdown_core_chunk_free(parser->mem, node->as.literal);
            *node->as.literal = markdown_core_chunk_literal("\xC2\xA0");
        }
        node->flags &= ~MARKDOWN_CORE_NODE__ESCAPED_SPACE;
    }
}

const markdown_core_extension MARKDOWN_CORE_EXTENSION_TEXT = {
    .inline_precedence = MARKDOWN_CORE_INLINE_FALLBACK,
    .parse_text = markdown_core_text_parse,
    .complete_inline = complete_inline,

    .name = "text",
    .match_inline = match,
    .terminates_text = "\\&",
    .dispatch = "\\&",
};
