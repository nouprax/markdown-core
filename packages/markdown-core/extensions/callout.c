#include "callout.h"
#define BLOCK_PEEK(input, at) ((input)->data[(at)])
#include "block_internal.h"

static bool markdown_core_block_parse_callout_metadata(markdown_core_parser *parser, markdown_core_node *node,
                                                       markdown_core_chunk *input);
static bool markdown_core_callout_scan(markdown_core_parser *parser, block_start_context *context, block_start *start);
bool markdown_core_block_parse_callout_prefix(markdown_core_parser *parser, markdown_core_chunk *input) {
    bool res = false;
    bufsize_t matched = 0;

    matched = parser->indent <= 3 && BLOCK_PEEK(input, parser->first_nonspace) == '>';
    if (matched) {

        markdown_core_block_advance_offset(parser, input, parser->indent + 1, true);

        if (markdown_core_block_is_space_or_tab(BLOCK_PEEK(input, parser->offset))) {
            markdown_core_block_advance_offset(parser, input, 1, true);
        }

        res = true;
    }
    return res;
}

static bool markdown_core_block_parse_callout_metadata(markdown_core_parser *parser, markdown_core_node *node,
                                                       markdown_core_chunk *input) {
    bufsize_t pos = parser->offset;
    bufsize_t begin = pos;
    while (pos < input->len && input->data[pos] == ' ' && pos - begin < 3) {
        pos++;
        parser->callout_scan_work++;
    }
    parser->callout_scan_work++;
    if (parser->partially_consumed_tab || pos + 2 >= input->len || input->data[pos] != '[' ||
        input->data[pos + 1] != '!') {
        return false;
    }
    pos += 2;
    begin = pos;
    while (pos < input->len) {
        unsigned char c = input->data[pos];
        parser->callout_scan_work++;
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
    if (pos < input->len && !markdown_core_block_is_space_or_tab(input->data[pos]) &&
        !markdown_core_is_line_end(input->data[pos])) {
        return false;
    }
    while (pos < input->len && markdown_core_block_is_space_or_tab(input->data[pos])) {
        pos++;
        parser->callout_scan_work++;
    }
    bufsize_t end = input->len;
    while (end > pos && (markdown_core_block_is_space_or_tab(input->data[end - 1]) ||
                         markdown_core_is_line_end(input->data[end - 1]))) {
        end--;
        parser->callout_scan_work++;
    }
    if (!markdown_core_chunk_to_cstr(parser->mem, &variant)) {
        parser->oom = true;
        return true;
    }
    node->as.callout->variant = markdown_core_optional_chunk_present(variant);
    node->as.callout->collapsed = (markdown_core_optional_bool){has_fold, collapsed};
    if (end > pos) {
        markdown_core_node *title = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_PARAGRAPH, parser->mem);
        if (!title) {
            parser->oom = true;
            return true;
        }
        node->as.callout->title = title;
        title->start_line = title->end_line = parser->line_number;
        title->start_column = markdown_core_parser_source_column(parser, parser->line_number, pos + 1);
        title->end_column = markdown_core_parser_source_column(parser, parser->line_number, end);
        markdown_core_strbuf_put(&title->content, input->data + pos, end - pos);
        if (title->content.oom || !markdown_core_parser_append_source_marks(parser, title, parser->line_number, pos + 1,
                                                                            title->content.size, 0)) {
            parser->oom = true;
        }
    }
    markdown_core_block_advance_offset(parser, input, input->len - 1 - parser->offset, false);
    return true;
}

static bool markdown_core_callout_open(markdown_core_parser *parser, markdown_core_node **container,
                                       markdown_core_chunk *input, block_start *start) {

    bufsize_t blockquote_startpos = parser->first_nonspace;

    markdown_core_block_advance_offset(parser, input, parser->first_nonspace + 1 - parser->offset, false);
    // optional following character
    if (markdown_core_block_is_space_or_tab(input->data[parser->offset])) {
        markdown_core_block_advance_offset(parser, input, 1, true);
    }
    *container =
        markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_CALLOUT, blockquote_startpos + 1);
    if (!*container) {
        return false;
    }

    if (markdown_core_block_parse_callout_metadata(parser, *container, input)) {
        return false;
    }

    return true;
}

static bool markdown_core_callout_scan(markdown_core_parser *parser, block_start_context *context, block_start *start) {
    markdown_core_chunk *input = context->input;
    int first = context->first;
    if (!(input->data[first] == '>')) {
        return false;
    }
    start->kind = MARKDOWN_CORE_NODE_CALLOUT;
    start->open = markdown_core_callout_open;
    return true;
}

bool markdown_core_callout_accepts_lazy_body(markdown_core_parser *parser, markdown_core_node *node) {
    return node->kind == MARKDOWN_CORE_NODE_CALLOUT && node->as.callout->variant.has_value &&
           node->start_line == parser->line_number - 1 && !node->first_child;
}
bool markdown_core_callout_open_lazy_body(markdown_core_parser *parser) {
    if (markdown_core_block_type(parser->current) == MARKDOWN_CORE_NODE_CALLOUT) {
        markdown_core_node *paragraph =
            markdown_core_parser_add_child(parser, parser->current, MARKDOWN_CORE_NODE_PARAGRAPH, parser->offset + 1);
        if (!paragraph) {
            return false;
        }
        parser->current = paragraph;
    }
    return true;
}

static bool continue_container(markdown_core_parser *parser, markdown_core_node *node, markdown_core_chunk *input,
                               const markdown_core_node *joining, bool *taken) {
    return markdown_core_block_parse_callout_prefix(parser, input);
}
static markdown_core_node *open_lazy(markdown_core_parser *parser, markdown_core_node *node) {
    return markdown_core_callout_open_lazy_body(parser) ? parser->current : NULL;
}

const markdown_core_extension MARKDOWN_CORE_EXTENSION_CALLOUT = {
    .accepts_lazy = markdown_core_callout_accepts_lazy_body,
    .open_lazy = open_lazy,

    .name = "callout",
    .continue_container = continue_container,
    .blank_opaque = true,
    .maximum_block_indent = 3,
    .scan_block_start = markdown_core_callout_scan,
};
