#include "code_block_scanners.h"
#include "code_block.h"
#include "block_internal.h"
#include "sink.h"
#include "attributes.h"
#include "formula.h"
#include <string.h>
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
        if (parser->at.indent >= CODE_INDENT) {
            markdown_core_block_advance_offset(parser, input, CODE_INDENT, true);
            res = true;
        } else if (parser->at.blank) {
            markdown_core_block_advance_offset(parser, input, parser->at.first_nonspace - parser->at.offset, false);
            res = true;
        }
    } else { // fenced
        bufsize_t matched = 0;

        if (parser->at.indent <= 3 && (peek_at(input, parser->at.first_nonspace) == container->as.code->fence_char)) {
            matched = scan_close_code_fence(input->data, input->len, parser->at.first_nonspace);
        }

        if (matched >= container->as.code->fence_length) {
            // closing fence - and since we're at
            // the end of a line, we can stop processing it:
            container->as.code->fence_closed = true;
            markdown_core_block_advance_offset(parser, input, matched, false);
            return MARKDOWN_CORE_BLOCK_CLOSED;
        } else {
            // skip opt. spaces of fence parser->at.offset
            int i = container->as.code->fence_offset;

            while (i > 0 && markdown_core_is_space_or_tab(peek_at(input, parser->at.offset))) {
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
 * line, which is the rest of the fence line and gave the info string as the
 * block opened. */
static bool close_code(const markdown_core_element_instance *self, markdown_core_parser *parser, markdown_core_node *b,
                       markdown_core_node *parent) {
    (void)self;
    (void)parser;
    (void)parent;
    const markdown_core_strbuf *content = &b->content;
    const bufsize_t start = b->as.code->literal_start;
    const bufsize_t end = b->as.code->fenced ? content->size : S_indented_literal_end(content);
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
/* The character reference at `at`, an `&`, when it spells whitespace, as
 * the info string's decoding reads it: its length; 0 when it spells
 * anything else or nothing; -1 when the data ends while it may still be
 * one. */
static bufsize_t space_reference(const unsigned char *data, bufsize_t at, bufsize_t length) {
    bufsize_t i = at + 1;
    if (i < length && data[i] == '#') {
        i++;
        const bool hex = i < length && (data[i] | 0x20) == 'x';
        i += hex;
        const bufsize_t digits = i, most = hex ? 6 : 7;
        long point = 0;
        for (; i < length && i - digits < most; i++) {
            const unsigned char c = data[i], low = c | 0x20;
            if (markdown_core_isdigit((char)c)) {
                point = point * (hex ? 16 : 10) + (c - '0');
            } else if (hex && low >= 'a' && low <= 'f') {
                point = point * 16 + (low - 'a' + 10);
            } else {
                break;
            }
        }
        if (i >= length) {
            return -1;
        }
        return i > digits && data[i] == ';' && (point == 9 || point == 10 || point == 13 || point == 32) ? i + 1 - at
                                                                                                         : 0;
    }
    static const char *const names[] = {"Tab;", "NewLine;"};
    for (size_t n = 0; n < sizeof(names) / sizeof(*names); n++) {
        const bufsize_t size = (bufsize_t)strlen(names[n]);
        const bufsize_t have = length - i < size ? length - i : size;
        if (!memcmp(data + i, names[n], (size_t)have)) {
            return have < size ? -1 : 1 + size;
        }
    }
    return 0;
}

/* Where an info string's first word goes on past a cursor: at a space or a
 * reference that spells one, or at the attribute tail that ends the line,
 * as the fence's info string leaves it; a brace that opens no tail is a
 * byte of the word. `cut` says whether a reference the data's end cuts
 * short may yet spell a space. */
static bufsize_t info_word(const unsigned char *data, bufsize_t length, bool *cut) {
    bufsize_t stop = length;
    while (stop > 0 && markdown_core_is_whitespace(data[stop - 1])) {
        stop--;
    }
    markdown_core_attribute_parser attributes = {.data = data, .length = stop};
    const bufsize_t tail = markdown_core_attributes_tail(&attributes, 0, stop);
    const int oom = attributes.oom;
    markdown_core_attribute_parser_free(&attributes);
    if (oom) {
        return -1;
    }
    bufsize_t end = 0;
    *cut = false;
    while (end < length && !markdown_core_is_whitespace(data[end]) && end != tail) {
        if (data[end] == '\\' && end + 1 < length && markdown_core_ispunct(data[end + 1])) {
            end += 2;
            continue;
        }
        if (data[end] == '&') {
            const bufsize_t space = space_reference(data, end, length);
            if (space > 0) {
                break;
            }
            *cut = *cut || space < 0;
        }
        end++;
    }
    return end;
}
static bufsize_t info_end(const unsigned char *data, bufsize_t length) {
    bool cut;
    return info_word(data, length, &cut);
}

/* The fence line's fields the line's end cuts short, `rest` its `length`
 * bytes past the fence, to the parse's context sink: a member of an open
 * attribute tail, and the info string's first word, after the spaces and
 * the references that spell them before it. Both are candidates: what
 * follows the cursor decides whether the brace opens a tail or is a byte of
 * the word, so the query confirms one against the whole line. */
static void S_report_info(markdown_core_parser *parser, const unsigned char *rest, bufsize_t length) {
    const bufsize_t offset = parser->at.offset;
    const unsigned char *line = rest - offset;
    markdown_core_attributes_report_tail(parser, MARKDOWN_CORE_KIND_CODE_BLOCK, line, offset, offset + length);
    bufsize_t start = 0, space;
    while (start < length) {
        if (markdown_core_is_space_or_tab(rest[start])) {
            start++;
        } else if (rest[start] == '&' && (space = space_reference(rest, start, length)) > 0) {
            start += space;
        } else {
            break;
        }
    }
    bool cut;
    const bufsize_t end = info_word(rest + start, length - start, &cut);
    if (end < 0) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    } else if (!cut && start + end == length) {
        const markdown_core_field info = {
            MARKDOWN_CORE_SLOT_INFO, MARKDOWN_CORE_KIND_CODE_BLOCK, offset + start, info_end, {{0}}, 0, false, 0};
        markdown_core_parser_open(parser, MARKDOWN_CORE_KIND_CODE_BLOCK, parser->at.first_nonspace, offset, &info, 1);
    }
}

/* THE FENCE LINE GIVES THE INFO STRING AS THE BLOCK OPENS, once: `rest`,
 * `length` bytes, is the line past the fence, which the block's content
 * begins with, and the literal starts past it. The info string is the rest
 * of the fence line, less an attribute tail, trimmed of spaces and tabs as
 * written. References and escapes are decoded after the trim, so a space one
 * of them spells is the info string's own. */
static void S_fence_info(markdown_core_parser *parser, markdown_core_node *b, const unsigned char *rest,
                         bufsize_t length) {
    const unsigned char *line_end = memchr(rest, '\n', (size_t)length);
    assert(line_end);
    const bufsize_t pos = (bufsize_t)(line_end - rest);
    if (parser->sink) {
        S_report_info(parser, rest, pos);
    }
    b->as.code->literal_start = pos + 1;
    markdown_core_strbuf tmp = MARKDOWN_CORE_BUF_INIT();
    bufsize_t info_end = markdown_core_attributes_attach_tail(parser, b, rest, pos);
    bufsize_t info_start = 0;
    while (info_start < info_end && markdown_core_is_space_or_tab(rest[info_start])) {
        info_start++;
    }
    while (info_end > info_start && markdown_core_is_space_or_tab(rest[info_end - 1])) {
        info_end--;
    }
    houdini_unescape_html_f(&tmp, rest + info_start, info_end - info_start);
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
}
static bool open_fenced(const markdown_core_element_instance *self, markdown_core_parser *parser,
                        markdown_core_node **container, markdown_core_chunk *input, block_start *start) {
    (void)self;
    bufsize_t matched = start->matched;

    *container = markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_CODE_BLOCK,
                                                parser->at.first_nonspace + 1);
    if (!*container) {
        return false;
    }
    (*container)->as.code->fenced = true;
    (*container)->as.code->fence_char = peek_at(input, parser->at.first_nonspace);
    (*container)->as.code->fence_length = (matched > 255) ? 255 : (uint8_t)matched;
    (*container)->as.code->fence_offset = (int8_t)(parser->at.first_nonspace - parser->at.offset);
    (*container)->as.code->fence_closed = false;
    markdown_core_block_advance_offset(parser, input, parser->at.first_nonspace + matched - parser->at.offset, false);
    S_fence_info(parser, *container, input->data + parser->at.offset, input->len - parser->at.offset);

    return true;
}
static bool open_indented(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_node **container, markdown_core_chunk *input, block_start *start) {
    (void)self;

    markdown_core_block_advance_offset(parser, input, CODE_INDENT, true);
    *container =
        markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_CODE_BLOCK, parser->at.offset + 1);
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
    (*container)->as.code->literal_start = 0;

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
