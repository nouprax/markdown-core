#include "definition_list.h"
#include "link.h"
#include "table.h"
#include "block_internal.h"

static bool markdown_core_block_definition_marker(markdown_core_chunk *input, int at, int indent);
static bool markdown_core_block_definition_prefix(markdown_core_parser *parser, markdown_core_node *parent,
                                                  markdown_core_chunk *input, bool *compact);
static markdown_core_node *markdown_core_block_open_definition(markdown_core_parser *parser, markdown_core_node *parent,
                                                               markdown_core_chunk *input, bool compact);
static bool markdown_core_definition_list_scan(markdown_core_parser *parser, block_start_context *context,
                                               block_start *start);
bool markdown_core_block_definition_body_blank_continues(markdown_core_parser *parser, markdown_core_node *body) {
    if (body->kind != MARKDOWN_CORE_NODE_DEFINITION_BODY) {
        return true;
    }
    parser->definition_list_work++;
    if (body->as.definition_body->continuation_line > parser->line_number) {
        return true;
    }
    markdown_core_block_lookahead lookahead;
    if (!markdown_core_parser_lookahead_begin(parser, body->parent, MARKDOWN_CORE_NODE_PARAGRAPH, &lookahead)) {
        return false;
    }
    markdown_core_chunk next;
    int first, indent, blanks;
    bool continues = markdown_core_parser_lookahead_next(&lookahead, &next, &first, &indent, &blanks) &&
                     indent >= body->as.definition_body->continuation;
    if (continues) {
        body->as.definition_body->continuation_line = lookahead.line - 1;
    }
    markdown_core_parser_lookahead_end(&lookahead);
    return continues;
}

static bool markdown_core_block_definition_marker(markdown_core_chunk *input, int at, int indent) {
    return indent < 4 && at + 1 < input->len && (input->data[at] == ':' || input->data[at] == '~') &&
           (markdown_core_block_is_space_or_tab(input->data[at + 1]) || markdown_core_is_line_end(input->data[at + 1]));
}

static bool markdown_core_block_definition_prefix(markdown_core_parser *parser, markdown_core_node *parent,
                                                  markdown_core_chunk *input, bool *compact) {
    parser->definition_list_work++;
    if (parser->blank || parser->indent >= 4 ||
        markdown_core_block_definition_marker(input, parser->first_nonspace, parser->indent)) {
        return false;
    }
    markdown_core_chunk term = {input->data + parser->first_nonspace, input->len - parser->first_nonspace, 0};
    if (term.data[0] == '[') {
        parser->definition_list_work += term.len;
        markdown_core_attribute_parser attributes = {.mem = parser->mem, .data = term.data, .length = term.len};
        bool reference = markdown_core_parse_reference_inline(parser->mem, &term, NULL, &attributes, 0) != 0;
        parser->attribute_work += attributes.work;
        parser->oom |= attributes.oom;
        markdown_core_attribute_parser_free(&attributes);
        if (reference || parser->oom) {
            return false;
        }
    }
    markdown_core_block_lookahead lookahead;
    if (!markdown_core_parser_lookahead_begin(parser, parent, MARKDOWN_CORE_NODE_DEFINITION_LIST, &lookahead)) {
        return false;
    }
    markdown_core_chunk next;
    int first, indent, blanks;
    bool matched = markdown_core_parser_lookahead_next(&lookahead, &next, &first, &indent, &blanks) && blanks <= 1 &&
                   markdown_core_block_definition_marker(&next, first, indent);
    if (matched && next.data[first] == ':') {
        matched = !markdown_core_table_caption_probe(&lookahead, &next, first, indent);
    }
    if (matched) {
        *compact = blanks == 0;
    }
    markdown_core_parser_lookahead_end(&lookahead);
    return matched;
}

static markdown_core_node *markdown_core_block_open_definition(markdown_core_parser *parser, markdown_core_node *parent,
                                                               markdown_core_chunk *input, bool compact) {
    /* A new term requires the separating blank run. If the preceding body's
     * prefix declined it, append at the existing list's definition boundary. */
    if (parent->kind == MARKDOWN_CORE_NODE_DEFINITION) {
        parent = markdown_core_block_finalize(parser, parent);
    }
    if (parent->kind != MARKDOWN_CORE_NODE_DEFINITION_LIST) {
        parent = markdown_core_parser_add_child(parser, parent, MARKDOWN_CORE_NODE_DEFINITION_LIST,
                                                parser->first_nonspace + 1);
        if (!parent) {
            return NULL;
        }
    }
    markdown_core_node *definition =
        markdown_core_parser_add_child(parser, parent, MARKDOWN_CORE_NODE_DEFINITION, parser->first_nonspace + 1);
    if (!definition) {
        return NULL;
    }
    definition->as.definition->compact = compact;
    markdown_core_node *term = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_PARAGRAPH, parser->mem);
    if (!term) {
        parser->oom = true;
        return definition;
    }
    definition->as.definition->term = term;
    int begin = parser->first_nonspace, end = input->len;
    while (end > begin && (markdown_core_block_is_space_or_tab(input->data[end - 1]) ||
                           markdown_core_is_line_end(input->data[end - 1]))) {
        end--;
    }
    parser->definition_list_work += end - begin;
    term->start_line = term->end_line = parser->line_number;
    term->start_column = markdown_core_parser_source_column(parser, parser->line_number, begin + 1);
    term->end_column = markdown_core_parser_source_column(parser, parser->line_number, end);
    markdown_core_strbuf_put(&term->content, input->data + begin, end - begin);
    if (term->content.oom || !markdown_core_parser_append_source_marks(parser, term, parser->line_number, begin + 1,
                                                                       term->content.size, 0)) {
        parser->oom = true;
    }
    markdown_core_block_advance_offset(parser, input, input->len - 1 - parser->offset, false);
    return definition;
}

static bool markdown_core_definition_list_open(markdown_core_parser *parser, markdown_core_node **container,
                                               markdown_core_chunk *input, block_start *start) {

    int continuation = parser->indent + markdown_core_block_consume_item_marker(parser, input, 1);
    *container = markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_DEFINITION_BODY,
                                                parser->first_nonspace + 1);
    if (!*container) {
        return false;
    }
    (*container)->as.definition_body->continuation = continuation;
    (*container)->internal_offset = markdown_core_parser_source_column(parser, parser->line_number, input->len - 1);
    return true;
}

static bool markdown_core_definition_list_scan(markdown_core_parser *parser, block_start_context *context,
                                               block_start *start) {
    markdown_core_chunk *input = context->input;
    int first = context->first;
    if (!(context->container->kind == MARKDOWN_CORE_NODE_DEFINITION &&
          markdown_core_block_definition_marker(input, first, context->indent))) {
        return false;
    }
    start->kind = MARKDOWN_CORE_NODE_DEFINITION_BODY;
    start->open = markdown_core_definition_list_open;
    return true;
}

bool markdown_core_definition_list_continue(markdown_core_parser *parser, markdown_core_node *container,
                                            markdown_core_chunk *input) {
    return markdown_core_block_continue_indented(parser, input, container->as.definition_body->continuation, true);
}

static markdown_core_node *try_paragraph(const markdown_core_element *self, int indented, markdown_core_parser *parser,
                                         markdown_core_node *parent, unsigned char *data, int length) {
    markdown_core_chunk input = {data, length, 0};
    bool compact = false;
    if (!markdown_core_block_definition_prefix(parser, parent, &input, &compact)) {
        return NULL;
    }
    return markdown_core_block_open_definition(parser, parent, &input, compact);
}

static bool continue_container(markdown_core_parser *parser, markdown_core_node *node, markdown_core_chunk *input,
                               const markdown_core_node *joining, bool *taken) {
    return node->kind != MARKDOWN_CORE_NODE_DEFINITION_BODY ||
           markdown_core_definition_list_continue(parser, node, input);
}
static void complete_block(markdown_core_parser *parser, markdown_core_node *node) {
    markdown_core_definition_list_complete(node);
}
static void finalize_block(markdown_core_parser *parser, markdown_core_node *node) {
    if (node->kind == MARKDOWN_CORE_NODE_DEFINITION_BODY) {
        markdown_core_definition_list_close_body(node);
    }
}

const markdown_core_element MARKDOWN_CORE_ELEMENT_DEFINITION_LIST = {
    .complete_block = complete_block,
    .finalize_block = finalize_block,

    .accepts_blank = markdown_core_block_definition_body_blank_continues,

    .name = "definition_list",
    .continue_container = continue_container,
    .maximum_block_indent = 3,
    .scan_block_start = markdown_core_definition_list_scan,
    .try_opening_paragraph = try_paragraph,
};

void markdown_core_definition_list_close_body(markdown_core_node *node) {
    if (!node->last_child) {
        node->end_line = node->start_line;
        node->end_column = node->internal_offset;
    }
}

void markdown_core_definition_list_complete(markdown_core_node *node) {
    if ((node->kind == MARKDOWN_CORE_NODE_DEFINITION_LIST || node->kind == MARKDOWN_CORE_NODE_DEFINITION ||
         node->kind == MARKDOWN_CORE_NODE_DEFINITION_BODY) &&
        node->last_child) {
        node->end_line = node->last_child->end_line;
        node->end_column = node->last_child->end_column;
    }
}
