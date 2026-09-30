#include "callout.h"
#define BLOCK_PEEK(input, at) ((input)->data[(at)])
#include "block_internal.h"

static bool markdown_core_block_parse_callout_metadata(markdown_core_callout_work *counts, markdown_core_parser *parser,
                                                       markdown_core_node *node, markdown_core_chunk *input);
static bool markdown_core_callout_scan(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                       block_start_context *context, block_start *start);
bool markdown_core_block_parse_callout_prefix(markdown_core_parser *parser, markdown_core_chunk *input) {
    bool res = false;
    bufsize_t matched = 0;

    matched = parser->indent <= 3 && BLOCK_PEEK(input, parser->first_nonspace) == '>';
    if (matched) {

        markdown_core_block_advance_offset(parser, input, parser->indent + 1, true);

        if (markdown_core_is_space_or_tab(BLOCK_PEEK(input, parser->offset))) {
            markdown_core_block_advance_offset(parser, input, 1, true);
        }

        res = true;
    }
    return res;
}

static bool markdown_core_block_parse_callout_metadata(markdown_core_callout_work *counts, markdown_core_parser *parser,
                                                       markdown_core_node *node, markdown_core_chunk *input) {
    bufsize_t pos = parser->offset;
    bufsize_t begin = pos;
    while (pos < input->len && input->data[pos] == ' ' && pos - begin < 3) {
        pos++;
        counts->scan++;
    }
    counts->scan++;
    if (parser->partially_consumed_tab || pos + 2 >= input->len || input->data[pos] != '[' ||
        input->data[pos + 1] != '!') {
        return false;
    }
    pos += 2;
    begin = pos;
    while (pos < input->len) {
        unsigned char c = input->data[pos];
        counts->scan++;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
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
        title->where.place =
            (markdown_core_place){(uint32_t)markdown_core_parser_source_offset(parser, parser->line_number, pos + 1),
                                  (uint32_t)markdown_core_parser_source_end(parser, parser->line_number, end)};
        markdown_core_strbuf_put(&title->content, input->data + pos, end - pos);
        if (title->content.oom || !markdown_core_parser_append_source_marks(parser, title, parser->line_number, pos + 1,
                                                                            title->content.size, 0)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
    }
    markdown_core_block_advance_offset(parser, input, input->len - 1 - parser->offset, false);
    return true;
}

static bool markdown_core_callout_open(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                       markdown_core_node **container, markdown_core_chunk *input, block_start *start) {

    bufsize_t blockquote_startpos = parser->first_nonspace;

    markdown_core_block_advance_offset(parser, input, parser->first_nonspace + 1 - parser->offset, false);
    // optional following character
    if (markdown_core_is_space_or_tab(input->data[parser->offset])) {
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
    return node->kind == MARKDOWN_CORE_NODE_CALLOUT && node->as.callout->variant.has_value && !node->first_child &&
           markdown_core_parser_starts_on_line(parser, node, parser->line_number - 1);
}

static bool continue_container(const markdown_core_element_instance *self, markdown_core_parser *parser,
                               markdown_core_node *node, markdown_core_chunk *input, const markdown_core_node *joining,
                               bool *taken) {
    (void)self;
    return markdown_core_block_parse_callout_prefix(parser, input);
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
    .container_prefix_bytes = ">",
    .blank_opaque = true,
    .maximum_block_indent = 3,
    .scan_block_start = markdown_core_callout_scan,
    .scan_block_gate = {.bytes = ">"},
};
