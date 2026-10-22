#include "comment_scanners.h"
#include "html_scanners.h"
#include "html_block.h"
#include "block_internal.h"

#include "comment.h"
static int continue_html(const markdown_core_element_instance *self, markdown_core_parser *parser, unsigned char *data,
                         int length, markdown_core_node *container) {
    bool res = false;
    int html_block_type = container->as.html_block->block_type;

    assert(html_block_type >= 1 && html_block_type <= 7);
    switch (html_block_type) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
        // these types of blocks can accept blanks
        res = true;
        break;
    case 6:
    case 7:
        res = !parser->at.blank;
        break;
    }

    return res;
}

/* An HTML block's literal is its content; a type-2 block its own end
 * condition closed and that holds one comment and nothing else is that
 * comment. */
static bool close_html(const markdown_core_element_instance *self, markdown_core_parser *parser, markdown_core_node *b,
                       markdown_core_node *parent) {
    (void)self;
    b->as.html_block->literal = (markdown_core_chunk){b->content.ptr, b->content.size, 0};
    if (b->as.html_block->block_type == 2 && (b->flags & MARKDOWN_CORE_NODE__CLOSED_BY_END_CONDITION) != 0) {
        markdown_core_block_convert_comment_block(parser, b, parent);
    }
    return true;
}
static bool open_html(const markdown_core_element_instance *self, markdown_core_parser *parser,
                      markdown_core_node **container, markdown_core_chunk *input, block_start *start) {
    (void)self;
    bufsize_t matched = start->matched;

    *container = markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_HTML_BLOCK,
                                                parser->at.first_nonspace + 1);
    if (!*container) {
        return false;
    }
    (*container)->as.html_block->block_type = matched;
    // note, we don't adjust parser->at.offset because the tag is part of the
    // text

    return true;
}
static bool scan_html(const markdown_core_element_instance *self, markdown_core_parser *parser,
                      block_start_context *context, block_start *start) {
    (void)self;
    if (!(start->matched = scan_html_block_start(context->input->data, context->input->len, context->first)) &&
        !(!context->paragraph && !context->lazy &&
          (start->matched = scan_html_block_start_7(context->input->data, context->input->len, context->first)))) {
        return false;
    }
    start->kind = MARKDOWN_CORE_NODE_HTML_BLOCK;
    start->open = open_html;
    return true;
}
static bool ends_html(const markdown_core_element_instance *self, markdown_core_parser *parser,
                      markdown_core_node *container, markdown_core_chunk *input) {
    (void)self;
    switch (container->as.html_block->block_type) {
    case 1:
        return scan_html_block_end_1(input->data, input->len, parser->at.first_nonspace);
    case 2:
        return scan_html_block_end_2(input->data, input->len, parser->at.first_nonspace);
    case 3:
        return scan_html_block_end_3(input->data, input->len, parser->at.first_nonspace);
    case 4:
        return scan_html_block_end_4(input->data, input->len, parser->at.first_nonspace);
    case 5:
        return scan_html_block_end_5(input->data, input->len, parser->at.first_nonspace);
    default:
        return false;
    }
}
static bool blank_line(const markdown_core_element_instance *self, markdown_core_parser *parser,
                       markdown_core_node *node) {
    (void)self;
    return true;
}

const markdown_core_element MARKDOWN_CORE_ELEMENT_HTML_BLOCK = {
    .blank_line = blank_line,

    .name = "html_block",
    .maximum_block_indent = 3,
    .scan_block_start = scan_html,
    .scan_block_gate = {.bytes = "<"},
    .last_block_matches = continue_html,
    .content_mode = MARKDOWN_CORE_CONTENT_LITERAL,
    .close_block = close_html,
    .ends_block = ends_html,
};
