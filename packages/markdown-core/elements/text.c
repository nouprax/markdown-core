#include "alloc.h"
#include "embedded.h"
#include "text.h"
#include "inline_internal.h"
#define advance(inline_state) ((inline_state)->pos += 1)

static int any_element_dispatches(markdown_core_parser *parser, unsigned char c) {
    const markdown_core_dialect *dialect = parser->dialect;
    for (size_t i = dialect->inline_dispatch_offsets[c]; i < dialect->inline_dispatch_offsets[c + 1]; i++) {
        if (dialect->inline_dispatch[i] != &MARKDOWN_CORE_ELEMENT_TEXT) {
            return 1;
        }
    }

    return 0;
}

static markdown_core_node *handle_backslash(markdown_core_parser *parser, markdown_core_inline_state *inline_state) {
    bufsize_t start = inline_state->pos;
    /* The line frame BEFORE anything is consumed. The hard-break arm below
     * needs it, and reading it after `markdown_core_inline_skip_line_end` would be right only
     * because `markdown_core_inline_skip_line_end` happens not to advance the frame -- an accident,
     * not a contract. `handle_newline` captures its frame first for the same
     * reason, and the two arms must not merely look symmetric. */
    advance(inline_state);
    unsigned char nextchar = markdown_core_inline_peek_char(inline_state);
    if (nextchar == ' ') {
        /* A trailing whitespace run cannot belong to a completed script.
         * Leave it to the inherited text/line-ending scanner, including its
         * trimming and hard-break rules. These lookaheads are disjoint: each
         * begins after its own backslash and ends before the next token. */
        bufsize_t end = inline_state->pos;
        while (end < inline_state->input.len &&
               !markdown_core_is_line_end(markdown_core_inline_peek_at(inline_state, end)) &&
               markdown_core_isspace(markdown_core_inline_peek_at(inline_state, end))) {
            end++;
            parser->whitespace_work++;
        }
        if ((end == inline_state->input.len && !MARKDOWN_CORE_NODE_TYPE_INLINE_P(inline_state->owner->kind)) ||
            (end < inline_state->input.len &&
             markdown_core_is_line_end(markdown_core_inline_peek_at(inline_state, end)))) {
            return make_str(inline_state, start, start, markdown_core_chunk_dup(&inline_state->input, start, 1));
        }
        advance(inline_state);
        markdown_core_node *escaped = make_str(inline_state, start, inline_state->pos - 1,
                                               markdown_core_chunk_dup(&inline_state->input, start, 2));
        if (escaped) {
            /* Contextual escape token: inline completion decodes it once the
             * delimiter/bracket engine has established its semantic owner. */
            escaped->flags |= MARKDOWN_CORE_NODE__ESCAPED_SPACE;
        }
        return escaped;
    }
    if (markdown_core_ispunct(nextchar)) {
        if (nextchar == '\\' && !any_element_dispatches(parser, '\\')) {
            bufsize_t end = start;
            while (end + 1 < inline_state->input.len && inline_state->input.data[end] == '\\' &&
                   inline_state->input.data[end + 1] == '\\') {
                end += 2;
            }
            if (end - start >= 4) {
                bufsize_t output_len = (end - start) / 2;
                unsigned char *output = (unsigned char *)markdown_core_alloc((size_t)output_len + 1, 1);
                if (output) {
                    markdown_core_chunk contents = {output, output_len, 1};
                    markdown_core_node *run;
                    memset(output, '\\', (size_t)output_len);
                    inline_state->pos = end;
                    run = make_str(inline_state, start, end - 1, contents);
                    /* One escape per PAIR: the first backslash of each is the
                     * escape and reaches no literal, the second is the byte the
                     * literal is made of. */
                    return run;
                }
            }
        }
        // only ascii symbols and newline can be escaped
        advance(inline_state);
        {
            markdown_core_node *escaped =
                make_str(inline_state, inline_state->pos - 2, inline_state->pos - 1,
                         markdown_core_chunk_dup(&inline_state->input, inline_state->pos - 1, 1));
            return escaped;
        }
    } else if (!markdown_core_inline_is_eof(inline_state) && markdown_core_inline_skip_line_end(inline_state)) {
        markdown_core_inline_push_boundary(inline_state, inline_state->pos);
        // A backslash hard break CONSUMES a line ending, so the inline state has to
        // be told, exactly as handle_newline tells it. It was not, so every node
        // after such a break kept the break's own line and a column measured
        // from the wrong line's start: `foo\` / `bar` reported Text 1:6..1:8 --
        // three columns that do not exist on a four-character line 1.
        // cmark-gfm reports the same numbers, so upstream cannot be the oracle.
        //
        // The node's extent is the bytes that SPELL it: the backslash and the
        // line ending it escapes. The backslash belonged to no node at all
        // before this, so the break is not taking it from anyone.
        markdown_core_node *hard =
            markdown_core_inline_make_simple_with_state(inline_state, MARKDOWN_CORE_NODE_LINE_BREAK);
        if (hard) {
            /* The break's extent is the backslash and the line ending it
             * escapes; `inline_state->pos` is one past that ending's last byte, CR, LF
             * or CRLF alike. Projected from the two offsets, so the frame
             * captured before the consume is only the fallback's. */
            markdown_core_inline_state_place(inline_state, hard, start, inline_state->pos - 1);
        }
        return hard;
    } else {
        return make_str(inline_state, inline_state->pos - 1, inline_state->pos - 1,
                        markdown_core_chunk_dup(&inline_state->input, inline_state->pos - 1, 1));
    }
}

static markdown_core_node *handle_entity(markdown_core_inline_state *inline_state) {
    markdown_core_strbuf ent = MARKDOWN_CORE_BUF_INIT();
    bufsize_t len;

    advance(inline_state);

    len = houdini_unescape_ent(&ent, inline_state->input.data + inline_state->pos,
                               inline_state->input.len - inline_state->pos);

    if (len == 0) {
        markdown_core_node *literal = make_str(inline_state, inline_state->pos - 1, inline_state->pos - 1,
                                               markdown_core_chunk_dup(&inline_state->input, inline_state->pos - 1, 1));
        /* Not an entity: the `&` IS the literal, so it is content. */
        return literal;
    }

    inline_state->pos += len;
    if (ent.oom) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
    }
    return make_str(inline_state, inline_state->pos - 1 - len, inline_state->pos - 1,
                    markdown_core_chunk_buf_detach(&ent));
}

static markdown_core_node *match(const markdown_core_element *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    if (character == '\\') {
        return handle_backslash(parser, inline_state);
    }
    if (character == '&') {
        return handle_entity(inline_state);
    }
    return NULL;
}
markdown_core_node *markdown_core_text_parse(markdown_core_parser *parser, markdown_core_inline_state *inline_state,
                                             bufsize_t endpos) {
    markdown_core_chunk contents;
    bufsize_t startpos;
    /* Disjoint ordinary text slices alone contribute whitespace barriers.
     * Escapes, entities, and opaque tokens have their own token owners. */
    /* THE SLICE'S WHITESPACE BOUNDARY, which is ONE position.
     *
     * `markdown_core_inline_push_boundary` OVERWRITES when the last delimiter
     * is already a boundary (core/inlines.c), and nothing else is pushed while
     * this runs -- so a pass that pushes on every space leaves exactly one
     * entry behind, at the position just past the LAST whitespace character in
     * [pos, endpos). Decoding every byte to find it walks the whole slice for
     * an answer that depends only on the slice's tail.
     *
     * Valid UTF-8 is a caller precondition (markdown_core.h), so the encoding
     * is self-synchronising here: stepping back over continuation bytes
     * (0x80-0xBF) lands on the lead byte of the preceding character, and that
     * is the same segmentation the forward pass builds from `pos`. Walking
     * back therefore visits the slice's characters in reverse and stops at the
     * last whitespace, which is the answer.
     *
     * The DECODER is entered only where the character could be whitespace at
     * all. Every non-ASCII member of the set `markdown_core_utf8proc_is_space`
     * matches -- U+00A0, U+1680, U+2000-200A, U+202F, U+205F, U+3000 -- leads
     * with 0xC2, 0xE1, 0xE2 or 0xE3, which the generator of its Unicode table
     * (scripts/tooling/generate-unicode-categories.mjs) asserts. CJK starts at
     * 0xE4 and the astral planes at 0xF0, so those are walked back on byte
     * tests alone.
     *
     * The `lead > pos` bound keeps a malformed run of continuation bytes in
     * range. What such input PARSES to is not defined -- the header says Markdown
     * Core neither validates nor repairs -- but it must still not read out of
     * the slice. */
    bufsize_t boundary = -1;
    for (bufsize_t i = endpos; i > inline_state->pos;) {
        unsigned char byte = inline_state->input.data[i - 1];
        parser->whitespace_work++;
        if (byte < 0x80) {
            /* The ASCII members of that same set; the rest are above 128. */
            if (byte == 9 || byte == 10 || byte == 12 || byte == 13 || byte == 32) {
                boundary = i;
                break;
            }
            i--;
            continue;
        }
        bufsize_t lead = i - 1;
        while (lead > inline_state->pos && (inline_state->input.data[lead] & 0xC0) == 0x80) {
            lead--;
        }
        unsigned char first = inline_state->input.data[lead];
        if (first == 0xC2 || (first >= 0xE1 && first <= 0xE3)) {
            int32_t scalar = 0;
            int width = markdown_core_utf8proc_step(inline_state->input.data + lead, endpos - lead, &scalar);
            if (markdown_core_utf8proc_is_space(scalar)) {
                boundary = lead + width;
                break;
            }
        }
        i = lead;
    }
    if (boundary >= 0) {
        markdown_core_inline_push_boundary(inline_state, boundary);
    }
    /* Text runs are disjoint, so recording separators costs at most one
     * extra visit per byte, regardless of bracket nesting or digit-run
     * length. No image closer scans its label again. */
    markdown_core_embedded_record_text(parser, inline_state, endpos);
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
static void complete_inline(markdown_core_parser *parser, markdown_core_node *node, int word_depth) {
    if (node->flags & MARKDOWN_CORE_NODE__ESCAPED_SPACE) {
        if (word_depth > 0) {
            markdown_core_chunk_free(node->as.literal);
            *node->as.literal = markdown_core_chunk_literal("\xC2\xA0");
        }
        node->flags &= ~MARKDOWN_CORE_NODE__ESCAPED_SPACE;
    }
}

const markdown_core_element MARKDOWN_CORE_ELEMENT_TEXT = {
    .inline_precedence = MARKDOWN_CORE_INLINE_FALLBACK,
    .parse_text = markdown_core_text_parse,
    .complete_inline = complete_inline,

    .name = "text",
    .match_inline = match,
    .terminates_text = "\\&",
    .dispatch = "\\&",
};
