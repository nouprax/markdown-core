#include "comment_scanners.h"
#include "html_scanners.h"
#include "html_block.h"
#include "block_internal.h"

static int continue_html(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         markdown_core_reader *reader, markdown_core_node *container) {
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
        res = !reader->blank;
        break;
    }

    return res;
}

/* An HTML block's literal is its content; a type-2 block its own end
 * condition closed and that holds one comment and nothing else is that
 * comment. */
static bool close_html(const markdown_core_element_instance *self, markdown_core_parser *parser, markdown_core_node *b,
                       markdown_core_node *parent, markdown_core_reader *reader) {
    (void)self;
    (void)parser;
    (void)parent;
    (void)reader;
    markdown_core_literal_set(b, &b->as.html_block->literal, 0, b->content.size);
    return true;
}
static bool open_html(const markdown_core_element_instance *self, markdown_core_parser *parser,
                      markdown_core_node **container, markdown_core_reader *reader, block_start *start) {
    (void)self;
    bufsize_t matched = start->matched;

    *container =
        markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_HTML_BLOCK, reader->nonspace + 1);
    if (!*container) {
        return false;
    }
    (*container)->as.html_block->block_type = matched;
    // note, we don't adjust reader->offset because the tag is part of the
    // text

    return true;
}
static bool scan_html(const markdown_core_element_instance *self, markdown_core_parser *parser,
                      block_start_context *context, block_start *start) {
    (void)self;
    if (!(start->matched =
              scan_html_block_start(markdown_core_reader_rest(context->reader, 0),
                                    markdown_core_reader_length(context->reader), context->reader->nonspace)) &&
        !(!context->paragraph && !context->lazy &&
          (start->matched =
               scan_html_block_start_7(markdown_core_reader_rest(context->reader, 0),
                                       markdown_core_reader_length(context->reader), context->reader->nonspace)))) {
        return false;
    }
    start->kind = MARKDOWN_CORE_NODE_HTML_BLOCK;
    start->open = open_html;
    return true;
}
static bool ends_html(const markdown_core_element_instance *self, markdown_core_parser *parser,
                      markdown_core_node *container, markdown_core_reader *reader) {
    (void)self;
    switch (container->as.html_block->block_type) {
    case 1:
        return scan_html_block_end_1(markdown_core_reader_rest(reader, 0), markdown_core_reader_length(reader),
                                     reader->nonspace);
    case 2:
        return scan_html_block_end_2(markdown_core_reader_rest(reader, 0), markdown_core_reader_length(reader),
                                     reader->nonspace);
    case 3:
        return scan_html_block_end_3(markdown_core_reader_rest(reader, 0), markdown_core_reader_length(reader),
                                     reader->nonspace);
    case 4:
        return scan_html_block_end_4(markdown_core_reader_rest(reader, 0), markdown_core_reader_length(reader),
                                     reader->nonspace);
    case 5:
        return scan_html_block_end_5(markdown_core_reader_rest(reader, 0), markdown_core_reader_length(reader),
                                     reader->nonspace);
    default:
        return false;
    }
}
static bool blank_line(const markdown_core_element_instance *self, markdown_core_parser *parser,
                       markdown_core_node *node, markdown_core_reader *reader) {
    (void)reader;
    (void)self;
    return true;
}

static const markdown_core_content_ops HTML_BLOCK_CONTENT = {
    .mode = MARKDOWN_CORE_CONTENT_LITERAL,
};

static const markdown_core_open_ops HTML_BLOCK_OPENS = {
    .indent = 3,
    .scan = scan_html,
    .scan_needs = {.bytes = "<"},
};

static const markdown_core_line_ops HTML_BLOCK_LINES = {
    .blank_last = blank_line,
    .matches = continue_html,
    .close = close_html,
    .ends = ends_html,
};

const markdown_core_facet MARKDOWN_CORE_HTML_BLOCK_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_HTML_BLOCK = {
    .facets = MARKDOWN_CORE_HTML_BLOCK_FACETS,
    .name = "html_block",
    .content = &HTML_BLOCK_CONTENT,
    .opens = &HTML_BLOCK_OPENS,
    .lines = &HTML_BLOCK_LINES,
};

/* An HTML block's record (markdown_core_record_ops): its literal, which may
 * be pending (markdown_core_literal_set). */
static void free_html_block(markdown_core_node_pool *pool, markdown_core_node *node) {
    (void)pool;
    markdown_core_chunk_free(&node->as.html_block->literal);
}

static bool view_html_block(markdown_core_node_pool *pool, markdown_core_node *copy, const markdown_core_node *node,
                            bool failed) {
    (void)pool;
    (void)node;
    (void)failed;
    markdown_core_literal_view(&copy->as.html_block->literal);
    return true;
}

static bool own_html_block(markdown_core_node *copy, const markdown_core_node *node) {
    return markdown_core_chunk_own(&copy->as.html_block->literal, &node->as.html_block->literal);
}

static void adopt_html_block(markdown_core_node *view, markdown_core_node *node) {
    markdown_core_chunk_adopt(&view->as.html_block->literal, &node->as.html_block->literal);
}

const markdown_core_record_ops MARKDOWN_CORE_RECORD_HTML_BLOCK = {
    .size = sizeof(markdown_core_html_block),
    .free = free_html_block,
    .view = view_html_block,
    .own = own_html_block,
    .adopt = adopt_html_block,
};
