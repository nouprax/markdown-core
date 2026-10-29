#include "callout.h"
#define BLOCK_PEEK(input, at) ((input)->data[(at)])
#include "block_internal.h"
#include "parts.h"
#include "sink.h"

static bool markdown_core_block_parse_callout_metadata(markdown_core_callout_work *counts, markdown_core_parser *parser,
                                                       markdown_core_node *node, markdown_core_chunk *input);
static bool markdown_core_callout_scan(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                       block_start_context *context, block_start *start);
bool markdown_core_block_parse_callout_prefix(markdown_core_parser *parser, markdown_core_chunk *input) {
    bool res = false;
    bufsize_t matched = 0;

    matched = parser->at.indent <= 3 && BLOCK_PEEK(input, parser->at.first_nonspace) == '>';
    if (matched) {

        markdown_core_block_advance_offset(parser, input, parser->at.indent + 1, true);

        if (markdown_core_is_space_or_tab(BLOCK_PEEK(input, parser->at.offset))) {
            markdown_core_block_advance_offset(parser, input, 1, true);
        }

        res = true;
    }
    return res;
}

static bool variant_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

/* Where a variant goes on past a cursor. */
static bufsize_t variant_end(const unsigned char *data, bufsize_t length) {
    bufsize_t end = 0;
    while (end < length && variant_byte(data[end])) {
        end++;
    }
    return end;
}

/* A `[!` at `open` whose variant the line's end cuts short, to the parse's
 * context sink. */
static void S_report_variant(markdown_core_parser *parser, const markdown_core_chunk *input, bufsize_t open) {
    bufsize_t end = input->len;
    while (end > open && markdown_core_is_line_end(input->data[end - 1])) {
        end--;
    }
    if (end - open < 2 || input->data[open] != '[' || input->data[open + 1] != '!' ||
        open + 2 + variant_end(input->data + open + 2, end - open - 2) != end) {
        return;
    }
    const markdown_core_field variant = {
        MARKDOWN_CORE_SLOT_VARIANT, MARKDOWN_CORE_KIND_CALLOUT, open + 2, variant_end, {{0}}, 0, false, 0};
    markdown_core_parser_open(parser, MARKDOWN_CORE_KIND_CALLOUT, open, open + 2, &variant, 1);
}

static bool markdown_core_block_parse_callout_metadata(markdown_core_callout_work *counts, markdown_core_parser *parser,
                                                       markdown_core_node *node, markdown_core_chunk *input) {
    bufsize_t pos = parser->at.offset;
    bufsize_t begin = pos;
    while (pos < input->len && input->data[pos] == ' ' && pos - begin < 3) {
        pos++;
        counts->scan++;
    }
    counts->scan++;
    if (parser->sink && !parser->at.partially_consumed_tab) {
        S_report_variant(parser, input, pos);
    }
    if (parser->at.partially_consumed_tab || pos + 2 >= input->len || input->data[pos] != '[' ||
        input->data[pos + 1] != '!') {
        return false;
    }
    pos += 2;
    begin = pos;
    while (pos < input->len) {
        unsigned char c = input->data[pos];
        counts->scan++;
        if (!variant_byte(c)) {
            break;
        }
        pos++;
    }
    if (pos == begin || pos >= input->len || input->data[pos] != ']') {
        return false;
    }
    markdown_core_chunk variant = {input->data + begin, pos - begin, 0};
    pos++;
    bool has_fold = pos < input->len && (input->data[pos] == '+' || input->data[pos] == '-');
    bool collapsed = has_fold && input->data[pos] == '-';
    pos += has_fold;
    if (pos < input->len && !markdown_core_is_whitespace(input->data[pos])) {
        return false;
    }
    while (pos < input->len && markdown_core_is_space_or_tab(input->data[pos])) {
        pos++;
        counts->scan++;
    }
    bufsize_t end = input->len;
    while (end > pos && markdown_core_is_whitespace(input->data[end - 1])) {
        end--;
        counts->scan++;
    }
    if (!markdown_core_chunk_to_cstr(&variant)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return true;
    }
    node->as.callout->variant = markdown_core_optional_chunk_present(variant);
    node->as.callout->collapsed = (markdown_core_optional_bool){has_fold, collapsed};
    if (end > pos) {
        markdown_core_node *title = markdown_core_parser_make_node(parser, MARKDOWN_CORE_NODE_PARAGRAPH);
        if (!title) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return true;
        }
        node->as.callout->title = title;
        title->place =
            (markdown_core_place){(uint32_t)markdown_core_parser_source_offset(parser, parser->at.line_number, pos + 1),
                                  (uint32_t)markdown_core_parser_source_end(parser, parser->at.line_number, end)};
        markdown_core_strbuf_put(&title->content, input->data + pos, end - pos);
        if (title->content.oom || !markdown_core_parser_append_source_marks(parser, title, parser->at.line_number,
                                                                            pos + 1, title->content.size, 0)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
        markdown_core_block_settle_field(parser, title, node);
    }
    markdown_core_block_advance_offset(parser, input, input->len - 1 - parser->at.offset, false);
    return true;
}

static bool markdown_core_callout_open(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                       markdown_core_node **container, markdown_core_chunk *input, block_start *start) {

    bufsize_t blockquote_startpos = parser->at.first_nonspace;

    markdown_core_block_advance_offset(parser, input, parser->at.first_nonspace + 1 - parser->at.offset, false);
    // optional following character
    if (markdown_core_is_space_or_tab(input->data[parser->at.offset])) {
        markdown_core_block_advance_offset(parser, input, 1, true);
    }
    *container =
        markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_CALLOUT, blockquote_startpos + 1);
    if (!*container) {
        return false;
    }

    if (markdown_core_block_parse_callout_metadata(self->state, parser, *container, input)) {
        return false;
    }

    return true;
}

static bool markdown_core_callout_scan(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                       block_start_context *context, block_start *start) {
    (void)self;
    markdown_core_chunk *input = context->input;
    int first = context->first;
    if (!(input->data[first] == '>')) {
        return false;
    }
    start->kind = MARKDOWN_CORE_NODE_CALLOUT;
    start->open = markdown_core_callout_open;
    return true;
}

static bool accepts_lazy(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         markdown_core_node *node) {
    (void)self;
    return node->kind == MARKDOWN_CORE_NODE_CALLOUT && node->as.callout->variant.has_value &&
           !markdown_core_parser_last_child(node) &&
           markdown_core_parser_starts_on_line(parser, node, parser->at.line_number - 1);
}

static bool continue_container(const markdown_core_element_instance *self, markdown_core_parser *parser,
                               markdown_core_node *node, markdown_core_chunk *input, const markdown_core_node *joining,
                               bool *taken) {
    (void)self;
    return markdown_core_block_parse_callout_prefix(parser, input);
}
/* The prefix reads the first non-space byte. Its column-counted advance
 * over a `>` reads the byte after the `>`'s scalar, and takes it when it is
 * a space or a tab -- a space's scalar too (markdown_core_block_scalar_end). */
static bufsize_t continue_reach(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                const markdown_core_node *node, const markdown_core_chunk *input) {
    (void)self;
    (void)node;
    const bufsize_t first = parser->at.first_nonspace;
    if (parser->at.indent > 3 || input->data[first] != '>') {
        return first;
    }
    const bufsize_t after = markdown_core_block_scalar_end(input, first + 1);
    return input->data[after] == ' ' ? markdown_core_block_scalar_end(input, after + 1) : after;
}
/* The scan reads the first non-space byte. The open reads the byte after
 * the `>`, which it takes when it is a space -- with the space's scalar
 * (markdown_core_block_scalar_end) -- or one column of a tab; then, unless
 * that tab is only partly taken, up to three spaces and the two bytes after
 * them, which a metadata line opens with `[!`, and whose header reads to the
 * line's end (markdown_core_block_parse_callout_metadata). */
static bufsize_t start_reach(const markdown_core_element_instance *self, markdown_core_parser *parser,
                             const block_start_context *context, bool *opens) {
    (void)self;
    (void)parser;
    const markdown_core_chunk *input = context->input;
    const bufsize_t end = input->len - 1;
    bufsize_t at = context->first;
    if (input->data[at] != '>') {
        return at;
    }
    *opens = true;
    at++;
    if (input->data[at] == '\t') {
        if (TAB_STOP - (context->column + 1) % TAB_STOP > 1) {
            return at;
        }
        at++;
    } else if (input->data[at] == ' ') {
        at = markdown_core_block_scalar_end(input, at + 1);
    }
    for (int spaces = 0; spaces < 3 && input->data[at] == ' '; spaces++) {
        at++;
    }
    if (at + 1 >= end || (input->data[at] == '[' && input->data[at + 1] == '!')) {
        return end;
    }
    return at + 1;
}
/* The marker line's text is the callout's title, so a lazy line after it
 * cannot continue a paragraph: it starts the body's first one, as the same
 * text would with the quote's prefix. */
static markdown_core_node *open_lazy(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                     markdown_core_node *node, markdown_core_chunk *input) {
    (void)self;
    const markdown_core_element_instance *text_block = parser->dialect->text_block_structure;
    return text_block->element->open_text_block(text_block, parser, node, input);
}

const markdown_core_element MARKDOWN_CORE_ELEMENT_CALLOUT = {
    .state_size = sizeof(markdown_core_callout_work),
    .accepts_lazy = accepts_lazy,
    .open_lazy = open_lazy,

    .name = "callout",
    .continue_container = continue_container,
    .continue_reach = continue_reach,
    .container_prefix_bytes = ">",
    .blank_opaque = true,
    /* A block identifier line after it names it (block_identifier.c). */
    .trailed = true,
    .maximum_block_indent = 3,
    .scan_block_start = markdown_core_callout_scan,
    .start_reach = start_reach,
    .scan_block_gate = {.bytes = ">"},
};

/* A callout's parts: its variant, inside `[!` and `]` on its first line, and
 * a block identifier on the line after it. The fold sign is a marker. */
void markdown_core_callout_parts(markdown_core_parts *parts) {
    const unsigned char *text = parts->text;
    bufsize_t at = parts->node->as.callout->variant.has_value ? 0 : parts->length;
    while (at < parts->length && text[at] != '\n' && text[at] != '\r' &&
           !(text[at] == '[' && at + 1 < parts->length && text[at + 1] == '!')) {
        at++;
    }
    if (at + 1 < parts->length && text[at] == '[' && text[at + 1] == '!') {
        const bufsize_t end = at + 2 + variant_end(text + at + 2, parts->length - at - 2);
        if (end < parts->length && text[end] == ']') {
            markdown_core_parts_add(parts, MARKDOWN_CORE_SLOT_VARIANT, at + 2, end);
        }
    }
    markdown_core_block_identifier_parts(parts);
}
