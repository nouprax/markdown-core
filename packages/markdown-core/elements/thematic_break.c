#include "thematic_break.h"
#include "block_internal.h"

static int S_scan_thematic_break(markdown_core_reader *reader, bufsize_t offset, bufsize_t *kill_pos) {
    bufsize_t i;
    char c;
    char nextc = '\0';
    int count;
    i = offset;
    c = markdown_core_reader_peek(reader, i);
    if (!(c == '*' || c == '_' || c == '-')) {
        *kill_pos = i;
        return 0;
    }
    count = 1;
    while ((nextc = markdown_core_reader_peek(reader, ++i))) {
        if (nextc == c) {
            count++;
        } else if (!markdown_core_is_space_or_tab(nextc)) {
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

static bool open_thematic(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_node **container, markdown_core_reader *reader, block_start *start) {
    (void)self;

    // it's only now that we know the line is not part of a setext heading:
    *container =
        markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_THEMATIC_BREAK, reader->nonspace + 1);
    if (!*container) {
        return false;
    }
    markdown_core_reader_end(reader);

    return true;
}
static bool scan_thematic(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          block_start_context *context, block_start *start) {
    (void)self;
    if (context->paragraph && !context->all_matched) {
        return false;
    }
    if (context->thematic_kill > context->reader->nonspace ||
        !(start->matched =
              S_scan_thematic_break(context->reader, context->reader->nonspace, &context->thematic_kill))) {
        return false;
    }
    start->kind = MARKDOWN_CORE_NODE_THEMATIC_BREAK;
    start->open = open_thematic;
    return true;
}
static const markdown_core_open_ops THEMATIC_BREAK_OPENS = {
    .indent = 3,
    .scan = scan_thematic,
    .scan_needs = {.bytes = "*-_"},
};

static const markdown_core_line_ops THEMATIC_BREAK_LINES = {
    .blank_opaque = true,
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_THEMATIC_BREAK = {
    .name = "thematic_break",
    .opens = &THEMATIC_BREAK_OPENS,
    .lines = &THEMATIC_BREAK_LINES,
};
