#include "comment.h"
#include "extension.h"
#include "node.h"
#include "parser.h"

/* THE `%%` COMMENT (O3), both of its forms, producing the `Comment` kind that
 * M0 gave the HTML comment. Nothing is stripped: a consumer that does not want
 * comments drops the nodes.
 *
 * INLINE, step A5 of the recognition order: `%%` at the cursor opens, and the
 * body runs to the first later `%%`, so `%%%%` is an empty comment and
 * `%%%a%%%` is the comment `%a` followed by the text `%`. The body is opaque,
 * which the scanner gets for free by consuming it: no later step and no other
 * module sees a byte of it. An escaped percent sign is the base language's
 * (step A1 runs first), so `\%%` is text. An opener with no closer is text and
 * hides nothing, and its failed search is cached under the comment's rule id,
 * so a run of percent signs that never closes costs one scan of its suffix and
 * not one per sign.
 *
 * BLOCK, step 4 of the block-start order: a line whose content after the
 * container prefixes is `%%` at indentation zero to three, followed only by
 * spaces or tabs, is a candidate, and it commits only if a later line whose
 * content is the same fence under the same prefixes closes it. That later line
 * is found by the parser's block-start lookahead before anything opens, so a
 * candidate with no closer consumes nothing: its line is paragraph text and
 * the inline rule applies to it. A committed block accepts every line until
 * the fence that closes it, interpreting no block syntax in between, and its
 * literal is those lines as written after container-prefix removal.
 *
 * The block form is the `%%` grammar's alone; the HTML comment block is the
 * inherited grammar's and never passes through here. */

static int is_fence_line(const unsigned char *data, int len, int first_nonspace) {
    int i = first_nonspace;

    if (i + 2 > len || data[i] != '%' || data[i + 1] != '%') {
        return 0;
    }
    for (i += 2; i < len && (data[i] == ' ' || data[i] == '\t'); i++) {
    }
    return i >= len || data[i] == '\n' || data[i] == '\r';
}

static int probe_comment_block(markdown_core_parser *parser, markdown_core_chunk *input, int first, int indent,
                               markdown_core_block_reader *reader) {
    if (indent >= 4 || !is_fence_line(input->data, input->len, first)) {
        return 0;
    }
    markdown_core_chunk line;
    while (reader->next(reader->context, &line, &first, &indent)) {
        if (indent < 4 && is_fence_line(line.data, line.len, first)) {
            return !parser->oom;
        }
    }
    return 0;
}

static int read_block_line(void *context, markdown_core_chunk *input, int *first, int *indent) {
    int blanks;
    return markdown_core_parser_lookahead_next(context, input, first, indent, &blanks);
}

static markdown_core_node *open_block(const markdown_core_extension *extension, int indented,
                                      markdown_core_parser *parser, markdown_core_node *parent_container,
                                      unsigned char *input, int len) {
    int first_nonspace = markdown_core_parser_get_first_nonspace(parser);
    markdown_core_block_lookahead lookahead;
    markdown_core_node *node;

    if (indented || !is_fence_line(input, len, first_nonspace)) {
        return NULL;
    }
    if (!markdown_core_parser_lookahead_begin(parser, parent_container, MARKDOWN_CORE_NODE_COMMENT_BLOCK, &lookahead)) {
        return NULL;
    }
    markdown_core_chunk line = {input, len, 0};
    markdown_core_block_reader reader = {&lookahead, read_block_line};
    bool closed = probe_comment_block(parser, &line, first_nonspace, parser->indent, &reader);
    markdown_core_parser_lookahead_end(&lookahead);
    if (!closed || parser->oom) {
        return NULL;
    }

    node =
        markdown_core_parser_add_child(parser, parent_container, MARKDOWN_CORE_NODE_COMMENT_BLOCK, first_nonspace + 1);
    if (!node) {
        return NULL;
    }
    markdown_core_node_set_extension(node, extension);
    /* The fence line is the block's marker and no part of its literal. */
    markdown_core_parser_advance_offset(parser, (char *)input, len - markdown_core_parser_get_offset(parser), false);
    return node;
}

/* Every line reaches the open block until its closer; the closer line closes
 * it and is not added. The lookahead saw this closer before the block opened,
 * through the same container matchers, so an open block always finds it. */
static int block_matches(const markdown_core_extension *extension, markdown_core_parser *parser, unsigned char *input,
                         int len, markdown_core_node *container) {
    if (markdown_core_parser_get_indent(parser) <= 3 &&
        is_fence_line(input, len, markdown_core_parser_get_first_nonspace(parser))) {
        return MARKDOWN_CORE_BLOCK_CLOSED;
    }
    return 1;
}

static int accepts_lines(const markdown_core_extension *extension, markdown_core_node *node) {
    return node && node->kind == MARKDOWN_CORE_NODE_COMMENT_BLOCK;
}

/* One closer unit: `%%` closes; any other byte is one byte of body. */
static int scan_closer(const unsigned char *data, int length, int at, markdown_core_delimiter_rule rule, bool *closes) {
    (void)rule;
    if (data[at] == '%' && at + 1 < length && data[at + 1] == '%') {
        *closes = true;
        return 2;
    }
    return 1;
}

static markdown_core_node *match(const markdown_core_extension *extension, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_parser *inlines) {
    markdown_core_chunk *input = markdown_core_inline_parser_get_chunk(inlines);
    int start = markdown_core_inline_parser_get_offset(inlines);
    int close;
    markdown_core_node *node;

    if (character != '%') {
        return NULL;
    }
    parser->comment_scan_work++;
    if (start + 1 >= input->len || input->data[start + 1] != '%') {
        return NULL;
    }
    close = markdown_core_inline_parser_find_opaque_close(inlines, MARKDOWN_CORE_DELIM_RULE_COMMENT, start + 2,
                                                          scan_closer);
    if (close < 0) {
        return NULL;
    }
    node = markdown_core_node_new_with_mem_and_ext(MARKDOWN_CORE_NODE_COMMENT, parser->mem, extension);
    if (!node) {
        parser->oom = true;
        return NULL;
    }
    *node->as.literal = markdown_core_chunk_dup(input, start + 2, close - start - 2);
    if (!markdown_core_chunk_to_cstr(parser->mem, node->as.literal)) {
        parser->oom = true;
        markdown_core_node_free(node);
        return NULL;
    }
    /* The scope covers both delimiters and the body. */
    markdown_core_parser_content_place(parser, parent, start, &node->start_line, &node->start_column);
    markdown_core_parser_content_end_place(parser, parent, close + 1, &node->end_line, &node->end_column);
    markdown_core_inline_parser_set_offset(inlines, close + 2);
    return node;
}

static const char *type_string(const markdown_core_extension *extension, markdown_core_node *node) {
    return node->kind == MARKDOWN_CORE_NODE_COMMENT_BLOCK ? "comment_block" : "comment";
}

/* `%` ends a text run and is offered to the scanner, and that is the whole set. */
const markdown_core_extension MARKDOWN_CORE_EXTENSION_COMMENT = {
    .name = "comment",
    .match_inline = match,
    .last_block_matches = block_matches,
    .try_opening_block = open_block,
    .probe_block = probe_comment_block,
    .get_type_string_func = type_string,
    .accepts_lines_func = accepts_lines,
    .terminates_text = "%",
    .dispatch = "%",
};
