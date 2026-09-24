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

/* A definition marker is ':' or '~' followed by a space, a tab or the line
 * end. Asking whether the NEXT line carries one used to mean opening a full
 * lookahead transaction -- chain walk, reserve, snapshot, then a line pulled
 * at 548 Ir -- on every line of every document, because a definition TERM is
 * arbitrary prose and nothing about the term's own line can rule the grammar
 * out. This is that question answered from raw source first, as the
 * grammar's own necessary condition.
 *
 * The transaction accepts only when the next line -- one blank line skipped
 * at most -- begins, once its container prefix is stripped, with a marker.
 * A container continuation strips nothing but indentation and the bytes its
 * element declares (`container_prefix_bytes`, projected to one table in
 * the dialect's `container_prefix`: quote markers, today), so when a marker is
 * there the raw bytes before it are all in that table: walking over them
 * lands on the byte the stripped line would show first, and a raw line made
 * of nothing else is blank once stripped (or a bare quote opener, which the
 * transaction refuses; yielding to the line after it only over-admits). So
 * the key reads a prefix, never a line: its cost is the container depth,
 * whatever the prose's length, where the `memchr` search it replaces read
 * both lines end to end (97% of this hook's own cost on long prose) and
 * admitted any line with ': ' in it. Every answer of false is a line the
 * transaction would refuse too. */
static bool definition_next_lines_admit(markdown_core_parser *parser) {
    const unsigned char *cursor = parser->lookahead_cursor, *end = parser->lookahead_end;
    for (int line = 0; line < 2 && cursor && cursor < end; line++) {
        const unsigned char *at = cursor;
        while (at < end && parser->dialect->container_prefix[*at]) {
            /* A declared prefix byte that is also a marker byte -- a
             * container whose continuation strips ':' or '~' -- cannot be
             * told from the marker here; only the transaction can, so the
             * key admits. No element declares one today; the rule is what
             * lets one do so without this key silently refusing the
             * definitions inside it. */
            if (*at == ':' || *at == '~') {
                return true;
            }
            at++;
        }
        if (at < end && !markdown_core_is_line_end((char)*at)) {
            return (*at == ':' || *at == '~') && (at + 1 == end || markdown_core_block_is_space_or_tab(at[1]) ||
                                                  markdown_core_is_line_end((char)at[1]));
        }
        /* Blank once stripped: the transaction skips one such line. */
        cursor = at;
        if (cursor < end && *cursor == '\r') {
            cursor++;
        }
        if (cursor < end && *cursor == '\n') {
            cursor++;
        }
    }
    return false;
}

static bool markdown_core_block_definition_prefix(markdown_core_parser *parser, markdown_core_node *parent,
                                                  markdown_core_chunk *input, bool *compact) {
    parser->definition_list_work++;
    if (parser->blank || parser->indent >= 4 ||
        markdown_core_block_definition_marker(input, parser->first_nonspace, parser->indent)) {
        return false;
    }
    /* The next line first: it refuses almost every line, and it costs the
     * container depth where the reference re-parse below costs the line. Both
     * are conjuncts of one decision, so the order changes nothing else. */
    if (!definition_next_lines_admit(parser)) {
        return false;
    }
    markdown_core_chunk term = {input->data + parser->first_nonspace, input->len - parser->first_nonspace, 0};
    if (term.data[0] == '[') {
        parser->definition_list_work += term.len;
        markdown_core_attribute_parser attributes = {
            .data = term.data, .length = term.len, .scratch = &parser->attribute_scratch};
        bool reference = markdown_core_parse_reference_inline(&term, NULL, &attributes, 0) != 0;
        parser->attribute_work += attributes.work;
        if (attributes.oom) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
        markdown_core_attribute_parser_free(&attributes);
        if (reference || parser->error) {
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
    markdown_core_node *term = markdown_core_parser_make_node(parser, MARKDOWN_CORE_NODE_PARAGRAPH);
    if (!term) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
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
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
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
/* A definition list, a definition and a body end where their last child
 * ends: taken at each one's EXIT, from inside the one finish walk, where the
 * children are complete. */
static markdown_core_finish_result finish_step(const markdown_core_element *element, markdown_core_parser *parser,
                                               markdown_core_node *node, markdown_core_event_type event, int is_root,
                                               void **state) {
    (void)element;
    (void)parser;
    (void)event;
    (void)is_root;
    (void)state;
    assert(event == MARKDOWN_CORE_EVENT_EXIT);
    markdown_core_definition_list_complete(node);
    return MARKDOWN_CORE_FINISH_CONTINUE;
}
static const markdown_core_node_type DEFINITION_LIST_EXIT_KINDS[] = {
    MARKDOWN_CORE_NODE_DEFINITION_LIST, MARKDOWN_CORE_NODE_DEFINITION, MARKDOWN_CORE_NODE_DEFINITION_BODY,
    MARKDOWN_CORE_NODE_NONE};
static void finalize_block(markdown_core_parser *parser, markdown_core_node *node) {
    if (node->kind == MARKDOWN_CORE_NODE_DEFINITION_BODY) {
        markdown_core_definition_list_close_body(node);
    }
}

const markdown_core_element MARKDOWN_CORE_ELEMENT_DEFINITION_LIST = {
    .finish_step = finish_step,
    .finish_exit_kinds = DEFINITION_LIST_EXIT_KINDS,
    .finalize_block = finalize_block,

    .accepts_blank = markdown_core_block_definition_body_blank_continues,

    .name = "definition_list",
    .continue_container = continue_container,
    .maximum_block_indent = 3,
    .scan_block_start = markdown_core_definition_list_scan,
    .scan_block_gate = {.bytes = ":~"},
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
