#include "code_block_scanners.h"
#include "code_block.h"
#include "block_internal.h"
#include "attributes.h"
#include "formula.h"
#define peek_at(input, at) ((input)->data[(at)])

/* The elements whose state this element reads, as `self->peers` holds them:
 * the formula element, which a fence naming a formula becomes. */
enum { CODE_FORMULA };
static const markdown_core_element *const CODE_PEERS[] = {[CODE_FORMULA] = &MARKDOWN_CORE_ELEMENT_FORMULA, NULL};

/* The end of an indented code block's literal: past the line ending of its
 * last line that is not blank. Every line it read ends with one, and its
 * first line is not blank. */
static bufsize_t S_indented_literal_end(const markdown_core_strbuf *content) {
    bufsize_t i = content->size - 1;
    while (markdown_core_is_whitespace(content->ptr[i])) {
        i--;
    }
    while (content->ptr[i] != '\n') {
        i++;
    }
    return i + 1;
}

static int continue_code(const markdown_core_element_instance *self, markdown_core_parser *parser, unsigned char *data,
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
            matched = scan_close_code_fence(input->data, input->len, parser->first_nonspace);
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

            while (i > 0 && markdown_core_is_space_or_tab(peek_at(input, parser->offset))) {
                markdown_core_block_advance_offset(parser, input, 1, true);
                i--;
            }
            res = true;
        }
    }

    return res;
}

/* A code block's literal is a view of its content: an indented block's
 * through its last line that is not blank, a fenced block's after its first
 * line, which is the rest of the fence line and gives the info string. */
static bool close_code(const markdown_core_element_instance *self, markdown_core_parser *parser, markdown_core_node *b,
                       markdown_core_node *parent) {
    (void)self;
    (void)parent;
    const markdown_core_strbuf *content = &b->content;
    bufsize_t start = 0, end = content->size;

    if (!b->as.code->fenced) {
        end = S_indented_literal_end(content);
    } else {
        const unsigned char *line_end = memchr(content->ptr, '\n', (size_t)content->size);
        assert(line_end);
        bufsize_t pos = (bufsize_t)(line_end - content->ptr);

        /* The info string is the rest of the fence line, less an attribute
         * tail, trimmed of spaces and tabs as written. References and
         * escapes are decoded after the trim, so a space one of them spells
         * is the info string's own. */
        markdown_core_strbuf tmp = MARKDOWN_CORE_BUF_INIT();
        bufsize_t info_end = markdown_core_attributes_attach_tail(parser, b, content->ptr, pos);
        bufsize_t info_start = 0;
        while (info_start < info_end && markdown_core_is_space_or_tab(content->ptr[info_start])) {
            info_start++;
        }
        while (info_end > info_start && markdown_core_is_space_or_tab(content->ptr[info_end - 1])) {
            info_end--;
        }
        houdini_unescape_html_f(&tmp, content->ptr + info_start, info_end - info_start);
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
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            markdown_core_strbuf_free(&tmp);
            b->as.code->info = markdown_core_optional_chunk_absent();
        } else if (tmp.size == 0) {
            markdown_core_strbuf_free(&tmp);
            b->as.code->info = markdown_core_optional_chunk_absent();
        } else {
            markdown_core_chunk info = markdown_core_chunk_buf_detach(&tmp);
            if (!info.data) {
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            }
            b->as.code->info = markdown_core_optional_chunk_present(info);
        }
        start = pos + 1;
    }
    b->as.code->literal = (markdown_core_chunk){content->ptr + start, end - start, 0};
    return true;
}
/* A fence whose info string names a formula becomes a FormulaBlock as
 * completion reaches it, in a dialect that holds formulas, inside the block
 * it settled into: before its explicit anchor is reserved, so the anchor of
 * the fence, which the FormulaBlock does not hold, is never taken. */
static void complete_code(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_node *node, int depth) {
    (void)depth;
    const markdown_core_element_instance *formula = self->peers[CODE_FORMULA];
    if (formula && node->as.code->fenced) {
        markdown_core_formula_promote_fence(formula, parser, node, markdown_core_scaffold_parent(node));
    }
}
static bool open_fenced(const markdown_core_element_instance *self, markdown_core_parser *parser,
                        markdown_core_node **container, markdown_core_chunk *input, block_start *start) {
    (void)self;
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
static bool open_indented(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_node **container, markdown_core_chunk *input, block_start *start) {
    (void)self;

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
static bool scan_code(const markdown_core_element_instance *self, markdown_core_parser *parser,
                      block_start_context *context, block_start *start) {
    (void)self;
    if (context->indent >= CODE_INDENT) {
        /* Indented code interrupts no paragraph, and a lazy line is text. */
        if (context->paragraph || context->lazy || markdown_core_is_line_end(context->input->data[context->first])) {
            return false;
        }
        start->open = open_indented;
    } else {
        start->matched = scan_open_code_fence(context->input->data, context->input->len, context->first);
        if (!start->matched) {
            return false;
        }
        start->open = open_fenced;
    }
    start->kind = MARKDOWN_CORE_NODE_CODE_BLOCK;
    return true;
}
static bool blank_line(const markdown_core_element_instance *self, markdown_core_parser *parser,
                       markdown_core_node *node) {
    (void)self;
    return !node->as.code->fenced;
}
const markdown_core_element MARKDOWN_CORE_ELEMENT_CODE_BLOCK = {
    .name = "code_block",
    .maximum_block_indent = INT_MAX,
    .scan_block_start = scan_code,
    /* A fence; an indented block is reached through the indent bound. */
    .scan_block_gate = {.bytes = "`~"},
    .last_block_matches = continue_code,
    .content_mode = MARKDOWN_CORE_CONTENT_LITERAL,
    .close_block = close_code,
    .complete_node = complete_code,
    .blank_line = blank_line,
    .peers = CODE_PEERS,
};
