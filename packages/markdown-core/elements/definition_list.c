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
    MARKDOWN_CORE_DIAGNOSTIC(parser->definition_list_work++;)
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

/* A term written as a reference definition is that definition, not a term. */
static bool term_is_reference(markdown_core_parser *parser, markdown_core_chunk term) {
    if (term.len == 0 || term.data[0] != '[' || !markdown_core_reference_definition_possible(term.data, term.len)) {
        return false;
    }
    MARKDOWN_CORE_DIAGNOSTIC(parser->definition_list_work += term.len;)
    markdown_core_attribute_parser attributes = {.mem = parser->mem, .data = term.data, .length = term.len};
    MARKDOWN_CORE_DIAGNOSTIC(parser->reference_probe_work++;)
    bool reference = markdown_core_parse_reference_inline(parser->mem, &term, NULL, &attributes, 0) != 0;
    MARKDOWN_CORE_DIAGNOSTIC(parser->attribute_work += attributes.work;)
    parser->oom |= attributes.oom;
    markdown_core_attribute_parser_free(&attributes);
    return reference || parser->oom;
}

/* A line continuing an open definition list is its next term when the line
 * after it is a marker: the list is open now and must decide now whether
 * it goes on, so this is the one place a term looks ahead. */
static bool markdown_core_block_definition_prefix(markdown_core_parser *parser, markdown_core_node *parent,
                                                  markdown_core_chunk *input, bool *compact) {
    MARKDOWN_CORE_DIAGNOSTIC(parser->definition_list_work++;)
    if (parser->blank || parser->indent >= 4 ||
        markdown_core_block_definition_marker(input, parser->first_nonspace, parser->indent)) {
        return false;
    }
    const markdown_core_block_peek *peek =
        markdown_core_parser_peek_block_line(parser, parent, MARKDOWN_CORE_NODE_DEFINITION_LIST);
    markdown_core_chunk next = peek->input;
    int first = peek->first, indent = peek->indent, blanks = peek->blanks;
    if (!peek->available || blanks > 1 || !markdown_core_block_definition_marker(&next, first, indent)) {
        return false;
    }
    markdown_core_chunk term = {input->data + parser->first_nonspace, input->len - parser->first_nonspace, 0};
    if (term_is_reference(parser, term)) {
        return false;
    }
    if (next.data[first] == ':') {
        /* A caption candidate needs the remaining stream. Resume through the
         * shared lookahead algorithm only after the marker is established. */
        markdown_core_block_lookahead lookahead;
        if (!markdown_core_parser_lookahead_begin(parser, parent, MARKDOWN_CORE_NODE_DEFINITION_LIST, &lookahead)) {
            return false;
        }
        bool caption = markdown_core_parser_lookahead_next(&lookahead, &next, &first, &indent, &blanks) &&
                       markdown_core_table_caption_probe(&lookahead, &next, first, indent);
        markdown_core_parser_lookahead_end(&lookahead);
        if (caption || parser->oom) {
            return false;
        }
    }
    *compact = blanks == 0;
    return true;
}

/* The paragraph a marker line on the parser's current line makes a term of:
 * the open one-line paragraph the line follows directly, or the one-line
 * paragraph that closed on the line before the single blank line above (a
 * term takes at most one blank line before its first definition). */
static markdown_core_node *definition_term_candidate(markdown_core_parser *parser, markdown_core_node *container) {
    markdown_core_node *term = container;
    if (term->kind != MARKDOWN_CORE_NODE_PARAGRAPH) {
        /* A closed paragraph that was only reference definitions has no
         * text left to be a term: its content went to the reference map
         * when it closed, and an open one is refused the same way once its
         * text is read (term_is_reference). */
        term = container->last_child;
        if (!term || term->kind != MARKDOWN_CORE_NODE_PARAGRAPH ||
            (term->flags & (MARKDOWN_CORE_NODE__OPEN | MARKDOWN_CORE_NODE__REFERENCE_DEFINITION_ONLY)) ||
            term->end_line != parser->line_number - 2) {
            return NULL;
        }
    }
    if (!markdown_core_parser_paragraph_line(parser, term)) {
        return NULL;
    }
    /* A marker line that opened no definition is a paragraph, not a term. */
    markdown_core_chunk text = {term->content->ptr, term->content->size, 0};
    return markdown_core_block_definition_marker(&text, 0, 0) ? NULL : term;
}

/* Whether the marker line, which is the parser's current line, is instead
 * the caption of a table it leads. */
static bool marker_leads_table(markdown_core_parser *parser, markdown_core_node *parent, markdown_core_chunk *input,
                               int first, int indent) {
    markdown_core_block_lookahead lookahead;
    if (!markdown_core_parser_lookahead_begin(parser, parent, MARKDOWN_CORE_NODE_DEFINITION_LIST, &lookahead)) {
        return true;
    }
    bool caption = markdown_core_table_caption_probe(&lookahead, input, first, indent);
    markdown_core_parser_lookahead_end(&lookahead);
    return caption || parser->oom;
}

static bool markdown_core_definition_list_open(markdown_core_parser *parser, markdown_core_node **container,
                                               markdown_core_chunk *input, block_start *start);

/* The marker line arrived under a one-line paragraph: that paragraph is the
 * definition's term, moved from its container into the definition the
 * marker opens, with the scope a term has always had -- its content without
 * trailing whitespace. The list and definition start where the term did. */
static bool open_definition_after_term(markdown_core_parser *parser, markdown_core_node **container,
                                       markdown_core_chunk *input, block_start *start) {
    markdown_core_node *term = definition_term_candidate(parser, *container);
    assert(term);
    markdown_core_node *parent = term->parent;
    bool compact = (term->flags & MARKDOWN_CORE_NODE__OPEN) != 0;
    bufsize_t end = term->content->size;
    while (end > 0 && (markdown_core_block_is_space_or_tab((char)term->content->ptr[end - 1]) ||
                       markdown_core_is_line_end(term->content->ptr[end - 1]))) {
        end--;
    }
    int end_line, end_column;
    if (end > 0 && markdown_core_parser_content_place(parser, term, end - 1, &end_line, &end_column)) {
        term->end_line = end_line;
        term->end_column = end_column;
    }
    MARKDOWN_CORE_DIAGNOSTIC(parser->definition_list_work += end;)
    term->flags &= ~MARKDOWN_CORE_NODE__OPEN;
    if (parser->current == term) {
        parser->current = parent;
    }
    if (parser->matched_container == term) {
        parser->matched_container = parent;
    }
    markdown_core_node_unlink(term);
    parser->paragraph_line.node = NULL;
    markdown_core_node *list = parent;
    if (list->kind != MARKDOWN_CORE_NODE_DEFINITION_LIST) {
        list = markdown_core_parser_add_child(parser, parent, MARKDOWN_CORE_NODE_DEFINITION_LIST,
                                              parser->first_nonspace + 1);
        if (!list) {
            markdown_core_node_recycle(parser->arena, term);
            return false;
        }
        list->start_line = list->end_line = term->start_line;
        list->start_column = term->start_column;
    }
    markdown_core_node *definition =
        markdown_core_parser_add_child(parser, list, MARKDOWN_CORE_NODE_DEFINITION, parser->first_nonspace + 1);
    if (!definition) {
        markdown_core_node_recycle(parser->arena, term);
        return false;
    }
    definition->start_line = definition->end_line = term->start_line;
    definition->start_column = term->start_column;
    definition->as.definition->compact = compact;
    definition->as.definition->term = term;
    definition->flags |= MARKDOWN_CORE_NODE__OWNS_FIELDS;
    *container = definition;
    return markdown_core_definition_list_open(parser, container, input, start);
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
    markdown_core_node *term =
        markdown_core_node_create(parser->arena, parser->mem, MARKDOWN_CORE_NODE_PARAGRAPH, NULL);
    if (!term) {
        parser->oom = true;
        return definition;
    }
    definition->as.definition->term = term;
    definition->flags |= MARKDOWN_CORE_NODE__OWNS_FIELDS;
    int begin = parser->first_nonspace, end = input->len;
    while (end > begin && (markdown_core_block_is_space_or_tab(input->data[end - 1]) ||
                           markdown_core_is_line_end(input->data[end - 1]))) {
        end--;
    }
    MARKDOWN_CORE_DIAGNOSTIC(parser->definition_list_work += end - begin;)
    term->start_line = term->end_line = parser->line_number;
    term->start_column = markdown_core_parser_source_column(parser, parser->line_number, begin + 1);
    term->end_column = markdown_core_parser_source_column(parser, parser->line_number, end);
    markdown_core_strbuf_put(term->content, input->data + begin, end - begin);
    if (term->content->oom || !markdown_core_parser_append_source_marks(parser, term, parser->line_number, begin + 1,
                                                                        term->content->size, 0)) {
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
    if (!markdown_core_block_definition_marker(input, first, context->indent)) {
        return false;
    }
    if (context->container->kind == MARKDOWN_CORE_NODE_DEFINITION) {
        start->kind = MARKDOWN_CORE_NODE_DEFINITION_BODY;
        start->open = markdown_core_definition_list_open;
        return true;
    }
    /* A marker line under a one-line paragraph makes that line the term: the
     * grammar is decided here, at its second line, not by every paragraph
     * looking ahead at its first. A query about a later line has no such
     * predecessor to read. */
    if (context->speculative) {
        return false;
    }
    markdown_core_node *term = definition_term_candidate(parser, context->container);
    if (!term || term_is_reference(parser, (markdown_core_chunk){term->content->ptr, term->content->size, 0}) ||
        (input->data[first] == ':' && marker_leads_table(parser, term->parent, input, first, context->indent))) {
        return false;
    }
    start->kind = MARKDOWN_CORE_NODE_DEFINITION;
    start->open = open_definition_after_term;
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
    /* Only a list that is open decides here; everywhere else the marker line
     * makes the term of the paragraph above it (markdown_core_definition_list_scan). */
    if (parent->kind != MARKDOWN_CORE_NODE_DEFINITION ||
        !markdown_core_block_definition_prefix(parser, parent, &input, &compact)) {
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
