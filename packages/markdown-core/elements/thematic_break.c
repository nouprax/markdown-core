#include "thematic_break.h"
#include "block_internal.h"
#define peek_at(input, at) ((input)->data[(at)])

static int S_scan_thematic_break(markdown_core_chunk *input, bufsize_t offset, bufsize_t *kill_pos) {
    bufsize_t i;
    char c;
    char nextc = '\0';
    int count;
    i = offset;
    c = peek_at(input, i);
    if (!(c == '*' || c == '_' || c == '-')) {
        *kill_pos = i;
        return 0;
    }
    count = 1;
    while ((nextc = peek_at(input, ++i))) {
        if (nextc == c) {
            count++;
        } else if (nextc != ' ' && nextc != '\t') {
            break;
        }
    }
    if (count >= 3 && (nextc == '\r' || nextc == '\n')) {
        return (i - offset) + 1;
    } else {
        *kill_pos = i;
        return 0;
    }
}

static bool open_thematic(markdown_core_parser *parser, markdown_core_node **container, markdown_core_chunk *input,
                          block_start *start) {

    // it's only now that we know the line is not part of a setext heading:
    *container = markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_THEMATIC_BREAK,
                                                parser->first_nonspace + 1);
    if (!*container) {
        return false;
    }
    markdown_core_block_advance_offset(parser, input, input->len - 1 - parser->offset, false);

    return true;
}
static bool scan_thematic(markdown_core_parser *parser, block_start_context *context, block_start *start) {
    if (context->paragraph && !context->all_matched) {
        return false;
    }
    if (context->thematic_kill > context->first ||
        !(start->matched = S_scan_thematic_break(context->input, context->first, &context->thematic_kill))) {
        return false;
    }
    start->kind = MARKDOWN_CORE_NODE_THEMATIC_BREAK;
    start->open = open_thematic;
    return true;
}
const markdown_core_element MARKDOWN_CORE_ELEMENT_THEMATIC_BREAK = {
    .name = "thematic_break",
    .maximum_block_indent = 3,
    .scan_block_start = scan_thematic,
    .blank_opaque = true,
};
