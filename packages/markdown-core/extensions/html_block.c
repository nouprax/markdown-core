#include "comment_scanners.h"
#include "html_scanners.h"
#include "html_block.h"
#include "block_internal.h"

#include "comment.h"
static int continue_html(const markdown_core_extension *self, markdown_core_parser *parser, unsigned char *data,
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
        res = !parser->blank;
        break;
    }

    return res;
}

static void finalize_html(markdown_core_parser *parser, markdown_core_node *b) {
    markdown_core_strbuf *node_content = &b->content;

    int html_block_type = b->as.html_block->block_type;
    b->as.html_block->literal = markdown_core_chunk_buf_detach(node_content);
    if (!b->as.html_block->literal.data) {
        parser->oom = true;
        return;
    }
    if (html_block_type == 2 && (b->flags & MARKDOWN_CORE_NODE__CLOSED_BY_END_CONDITION) != 0) {
        markdown_core_block_convert_comment_block(parser, b);
    }
}
static bool open_html(markdown_core_parser *parser, markdown_core_node **container, markdown_core_chunk *input,
                      block_start *start) {
    bufsize_t matched = start->matched;

    *container =
        markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_HTML_BLOCK, parser->first_nonspace + 1);
    if (!*container) {
        return false;
    }
    (*container)->as.html_block->block_type = matched;
    // note, we don't adjust parser->offset because the tag is part of the
    // text

    return true;
}
static bool scan_html(markdown_core_parser *parser, block_start_context *context, block_start *start) {
    if (!(start->matched = scan_html_block_start(context->input, context->first)) &&
        !(!context->paragraph && !context->lazy &&
          (start->matched = scan_html_block_start_7(context->input, context->first)))) {
        return false;
    }
    start->kind = MARKDOWN_CORE_NODE_HTML_BLOCK;
    start->open = open_html;
    return true;
}
static bool ends_html(markdown_core_parser *parser, markdown_core_node *container, markdown_core_chunk *input) {
    switch (container->as.html_block->block_type) {
    case 1:
        return scan_html_block_end_1(input, parser->first_nonspace);
    case 2:
        return scan_html_block_end_2(input, parser->first_nonspace);
    case 3:
        return scan_html_block_end_3(input, parser->first_nonspace);
    case 4:
        return scan_html_block_end_4(input, parser->first_nonspace);
    case 5:
        return scan_html_block_end_5(input, parser->first_nonspace);
    default:
        return false;
    }
}
static bool blank_line(markdown_core_parser *parser, markdown_core_node *node) { return true; }

const markdown_core_extension MARKDOWN_CORE_EXTENSION_HTML_BLOCK = {
    .blank_line = blank_line,

    .name = "html_block",
    .maximum_block_indent = 3,
    .scan_block_start = scan_html,
    .last_block_matches = continue_html,
    .content_mode = MARKDOWN_CORE_CONTENT_LITERAL,
    .finalize_block = finalize_html,
    .ends_block = ends_html,
};
