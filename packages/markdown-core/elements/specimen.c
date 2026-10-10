#include "alloc.h"
#include "specimen.h"
#define BLOCK_PEEK(input, at) ((input)->data[(at)])
#include "block_internal.h"

static bufsize_t markdown_core_block_parse_specimen_marker(markdown_core_specimen_state *state,
                                                           markdown_core_chunk *input, bufsize_t pos,
                                                           markdown_core_specimen_value *value);
static bool markdown_core_specimen_scan(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                        block_start_context *context, block_start *start);
static bufsize_t markdown_core_block_parse_specimen_marker(markdown_core_specimen_state *state,
                                                           markdown_core_chunk *input, bufsize_t pos,
                                                           markdown_core_specimen_value *value) {
    bufsize_t begin = pos;
    *value = (markdown_core_specimen_value){0};
    state->work++;
    if (BLOCK_PEEK(input, pos++) != '(') {
        return 0;
    }
    int digits = 0;
    while (digits < 9 && markdown_core_isdigit(BLOCK_PEEK(input, pos))) {
        state->work++;
        value->start = value->start * 10 + input->data[pos++] - '0';
        digits++;
    }
    value->has_start = digits > 0;
    if ((digits && !value->start) || BLOCK_PEEK(input, pos++) != '@') {
        return 0;
    }
    /* A LABEL IS WHAT A BARE REFERENCE READS WHOLE. A reference `@label` is
     * scanned as a citation key (citation.c): Unicode letters, numbers and
     * `_` are key characters, and internal punctuation stands singly between
     * them. Pandoc's manual gives the definition "alphanumeric characters,
     * underscores, or hyphens", and the hyphen is the one punctuation mark of
     * that class, so a label is key characters with `-` singly between them,
     * read by the same rule the reference applies -- a definition the
     * reference could not name whole would be unreachable, so it is not a
     * definition: `(@a--b)` and `(@x-)` are text. */
    bufsize_t label = pos;
    while (pos < input->len) {
        unsigned char c = input->data[pos];
        int width = c == '_' ? 1 : markdown_core_utf8proc_alnum_width(input->data + pos, input->len - pos);
        state->work++;
        if (!width && c == '-' && pos > label && pos + 1 < input->len) {
            unsigned char next = input->data[pos + 1];
            int following =
                next == '_' ? 1 : markdown_core_utf8proc_alnum_width(input->data + pos + 1, input->len - pos - 1);
            width = following ? 1 + following : 0;
        }
        if (!width) {
            break;
        }
        pos += width;
    }
    if (BLOCK_PEEK(input, pos) != ')' || !markdown_core_is_whitespace(BLOCK_PEEK(input, pos + 1))) {
        return 0;
    }
    if (pos > label) {
        value->label = markdown_core_optional_chunk_present(markdown_core_chunk_dup(input, label, pos - label));
    }
    return pos + 1 - begin;
}

static bool markdown_core_specimen_open(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                        markdown_core_member **container, markdown_core_chunk *input,
                                        block_start *start) {
    bufsize_t matched = start->matched;
    markdown_core_specimen_value specimen = start->specimen;

    if (specimen.label.has_value && !markdown_core_chunk_to_cstr(&specimen.label.value)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    *container =
        markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_SPECIMEN, parser->first_nonspace + 1);
    if (!*container) {
        markdown_core_optional_chunk_free(&specimen.label);
        return false;
    }
    if ((*container)->prev && (*container)->prev->node->kind == MARKDOWN_CORE_NODE_SPECIMEN) {
        specimen.has_start = false;
        specimen.start = 0;
    }
    *(*container)->node->as.specimen = specimen;
    markdown_core_specimen_state *state = self->state;
    /* The document defines this label while the definition is in the tree. */
    if (specimen.label.has_value) {
        markdown_core_parser_declare(parser, (*container)->node, MARKDOWN_CORE_KEY_SPECIMEN, &specimen.label.value);
    }
    markdown_core_block_advance_offset(parser, input, parser->first_nonspace + matched - parser->offset, false);
    while (markdown_core_is_space_or_tab(input->data[parser->offset])) {
        markdown_core_block_advance_offset(parser, input, 1, true);
        state->work++;
    }
    return true;
}

static bool markdown_core_specimen_scan(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                        block_start_context *context, block_start *start) {
    (void)parser;
    markdown_core_chunk *input = context->input;
    int first = context->first;
    if (!(start->matched = markdown_core_block_parse_specimen_marker(self->state, input, first, &start->specimen)) ||
        markdown_core_block_start_refuses(context, true, false)) {
        return false;
    }
    start->kind = MARKDOWN_CORE_NODE_SPECIMEN;
    start->open = markdown_core_specimen_open;
    return true;
}

bool markdown_core_specimen_continue(markdown_core_parser *parser, markdown_core_member *container,
                                     markdown_core_chunk *input) {
    return markdown_core_block_continue_indented(parser, input, 4, true);
}

static bool continue_container(const markdown_core_element_instance *self, markdown_core_parser *parser,
                               markdown_core_member *node, markdown_core_chunk *input,
                               const markdown_core_member *joining, bool *taken) {
    (void)self;
    return markdown_core_specimen_continue(parser, node, input);
}
/* A specimen carries nothing its lines read (E3). */
static uint32_t carry_save(const markdown_core_element_instance *self, const markdown_core_member *member) {
    (void)self;
    (void)member;
    return 0;
}

const markdown_core_element MARKDOWN_CORE_ELEMENT_SPECIMEN = {
    .name = "specimen",
    .state_size = sizeof(markdown_core_specimen_state),
    .continue_container = continue_container,
    .maximum_block_indent = 3,
    .scan_block_start = markdown_core_specimen_scan,
    .carry_save = carry_save,
    .scan_block_gate = {.bytes = "("},
};
