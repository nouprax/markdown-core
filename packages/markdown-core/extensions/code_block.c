#include "code_block_scanners.h"
#include "code_block.h"
#include "block_internal.h"
#include "attributes.h"
#define peek_at(input, at) ((input)->data[(at)])

static void remove_trailing_blank_lines(markdown_core_strbuf *ln) {
    bufsize_t i;
    unsigned char c;

    for (i = ln->size - 1; i >= 0; --i) {
        c = ln->ptr[i];

        if (c != ' ' && c != '\t' && !markdown_core_is_line_end(c)) {
            break;
        }
    }

    if (i < 0) {
        markdown_core_strbuf_clear(ln);
        return;
    }

    for (; i < ln->size; ++i) {
        c = ln->ptr[i];

        if (!markdown_core_is_line_end(c)) {
            continue;
        }

        markdown_core_strbuf_truncate(ln, i);
        break;
    }
}

static int continue_code(const markdown_core_extension *self, markdown_core_parser *parser, unsigned char *data,
                         int length, markdown_core_node *container) {
    markdown_core_chunk input_chunk = {(unsigned char *)data, length, 0};
    markdown_core_chunk *input = &input_chunk;
    bool res = false;

    if (!container->as.code->fenced) { // indented
        if (parser->indent >= CODE_INDENT) {
            markdown_core_block_advance_offset(parser, input, CODE_INDENT, true);
            res = true;
        } else if (parser->blank) {
            markdown_core_block_advance_offset(parser, input, parser->first_nonspace - parser->offset, false);
            res = true;
        }
    } else { // fenced
        bufsize_t matched = 0;

        if (parser->indent <= 3 && (peek_at(input, parser->first_nonspace) == container->as.code->fence_char)) {
            matched = scan_close_code_fence(input, parser->first_nonspace);
        }

        if (matched >= container->as.code->fence_length) {
            // closing fence - and since we're at
            // the end of a line, we can stop processing it:
            container->as.code->fence_closed = true;
            markdown_core_block_advance_offset(parser, input, matched, false);
            return MARKDOWN_CORE_BLOCK_CLOSED;
        } else {
            // skip opt. spaces of fence parser->offset
            int i = container->as.code->fence_offset;

            while (i > 0 && markdown_core_block_is_space_or_tab(peek_at(input, parser->offset))) {
                markdown_core_block_advance_offset(parser, input, 1, true);
                i--;
            }
            res = true;
        }
    }

    return res;
}

static void finalize_code(markdown_core_parser *parser, markdown_core_node *b) {
    bufsize_t pos;
    markdown_core_strbuf *node_content = &b->content;

    if (!b->as.code->fenced) { // indented code
        remove_trailing_blank_lines(node_content);
        markdown_core_strbuf_putc(node_content, '\n');
    } else {
        // first line of contents becomes info
        for (pos = 0; pos < node_content->size; ++pos) {
            if (markdown_core_is_line_end(node_content->ptr[pos])) {
                break;
            }
        }
        assert(pos < node_content->size);

        markdown_core_strbuf tmp = MARKDOWN_CORE_BUF_INIT(parser->mem);
        bufsize_t info_end = markdown_core_attributes_attach_tail(parser, b, node_content->ptr, pos);
        houdini_unescape_html_f(&tmp, node_content->ptr, info_end);
        markdown_core_strbuf_trim(&tmp);
        markdown_core_strbuf_unescape(&tmp);
        /* WHETHER THE SOURCE WROTE AN INFO STRING IS DECIDED HERE, ONCE.
         * A fence with nothing but whitespace after it wrote none, and
         * this is the only place that still knows the difference between
         * that and the `js` in ```` ```js ````. The facade used to decide
         * it again by testing the length, which is the fold requirement 14
         * forbids. */
        if (tmp.oom) {
            /* A buffer that could not be grown has `size == 0` and it is
             * NOT an absent info string -- it is an info string the parse
             * lost. The strict OOM sweep requires that loss to terminate
             * the parse. */
            parser->oom = true;
            markdown_core_strbuf_free(&tmp);
            b->as.code->info = markdown_core_optional_chunk_absent();
        } else if (tmp.size == 0) {
            markdown_core_strbuf_free(&tmp);
            b->as.code->info = markdown_core_optional_chunk_absent();
        } else {
            markdown_core_chunk info = markdown_core_chunk_buf_detach(&tmp);
            if (!info.data) {
                parser->oom = true;
            }
            b->as.code->info = markdown_core_optional_chunk_present(info);
        }

        if (node_content->ptr[pos] == '\r') {
            pos += 1;
        }
        if (node_content->ptr[pos] == '\n') {
            pos += 1;
        }
        markdown_core_strbuf_drop(node_content, pos);
    }
    b->as.code->literal = markdown_core_chunk_buf_detach(node_content);
    if (!b->as.code->literal.data) {
        parser->oom = true;
    }
}
static bool open_fenced(markdown_core_parser *parser, markdown_core_node **container, markdown_core_chunk *input,
                        block_start *start) {
    bufsize_t matched = start->matched;

    *container =
        markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_CODE_BLOCK, parser->first_nonspace + 1);
    if (!*container) {
        return false;
    }
    (*container)->as.code->fenced = true;
    (*container)->as.code->fence_char = peek_at(input, parser->first_nonspace);
    (*container)->as.code->fence_length = (matched > 255) ? 255 : (uint8_t)matched;
    (*container)->as.code->fence_offset = (int8_t)(parser->first_nonspace - parser->offset);
    (*container)->as.code->fence_closed = false;
    /* Nothing is known about an info string until the fence line is
     * read; ABSENT is the honest state, and the close either replaces
     * it or leaves it. It used to open as an empty STRING, which said
     * the source had written one. */
    (*container)->as.code->info = markdown_core_optional_chunk_absent();
    markdown_core_block_advance_offset(parser, input, parser->first_nonspace + matched - parser->offset, false);

    return true;
}
static bool open_indented(markdown_core_parser *parser, markdown_core_node **container, markdown_core_chunk *input,
                          block_start *start) {

    markdown_core_block_advance_offset(parser, input, CODE_INDENT, true);
    *container = markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_CODE_BLOCK, parser->offset + 1);
    if (!*container) {
        return false;
    }
    (*container)->as.code->fenced = false;
    (*container)->as.code->fence_char = 0;
    (*container)->as.code->fence_length = 0;
    (*container)->as.code->fence_offset = 0;
    (*container)->as.code->fence_closed = false;
    /* An indented code block has no fence and therefore no info
     * string, ever. */
    (*container)->as.code->info = markdown_core_optional_chunk_absent();

    return true;
}
static bool scan_code(markdown_core_parser *parser, block_start_context *context, block_start *start) {
    if (context->indent >= CODE_INDENT) {
        if (context->lazy || markdown_core_is_line_end(context->input->data[context->first])) {
            return false;
        }
        start->open = open_indented;
    } else {
        start->matched = scan_open_code_fence(context->input, context->first);
        if (!start->matched) {
            return false;
        }
        start->open = open_fenced;
    }
    start->kind = MARKDOWN_CORE_NODE_CODE_BLOCK;
    return true;
}
static bool blank_line(markdown_core_parser *parser, markdown_core_node *node) { return !node->as.code->fenced; }
const markdown_core_extension MARKDOWN_CORE_EXTENSION_CODE_BLOCK = {
    .name = "code_block",
    .maximum_block_indent = INT_MAX,
    .scan_block_start = scan_code,
    .last_block_matches = continue_code,
    .content_mode = MARKDOWN_CORE_CONTENT_LITERAL,
    .finalize_block = finalize_code,
    .blank_line = blank_line,
};
