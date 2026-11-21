#include "alloc.h"
#include "specimen.h"
#include "block_internal.h"
#include "parts.h"

static bufsize_t markdown_core_block_parse_specimen_marker(markdown_core_specimen_state *state,
                                                           markdown_core_reader *reader, bufsize_t pos,
                                                           markdown_core_specimen_value *value);
static bool markdown_core_specimen_scan(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                        block_start_context *context, block_start *start);
/* The width of the letter or number at `at`, a scalar the reader reads
 * whole; 0 for any other scalar. */
static inline int S_key_width(markdown_core_reader *reader, bufsize_t at) {
    int32_t scalar;
    const int width = markdown_core_reader_decode(reader, at, &scalar);
    return markdown_core_utf8proc_classes(scalar) & (MARKDOWN_CORE_UNICODE_LETTER | MARKDOWN_CORE_UNICODE_NUMBER)
               ? width
               : 0;
}

static bufsize_t markdown_core_block_parse_specimen_marker(markdown_core_specimen_state *state,
                                                           markdown_core_reader *reader, bufsize_t pos,
                                                           markdown_core_specimen_value *value) {
    bufsize_t begin = pos;
    *value = (markdown_core_specimen_value){0};
    state->work++;
    if (markdown_core_reader_peek(reader, pos++) != '(') {
        return 0;
    }
    int digits = 0;
    while (digits < 9 && markdown_core_isdigit(markdown_core_reader_peek(reader, pos))) {
        state->work++;
        value->start = value->start * 10 + markdown_core_reader_peek(reader, pos++) - '0';
        digits++;
    }
    value->has_start = digits > 0;
    if ((digits && !value->start) || markdown_core_reader_peek(reader, pos++) != '@') {
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
    while (!markdown_core_reader_ends(reader, pos)) {
        int32_t scalar;
        int width = markdown_core_reader_decode(reader, pos, &scalar);
        const int32_t c = scalar;
        width = c == '_' || markdown_core_utf8proc_classes(scalar) &
                                (MARKDOWN_CORE_UNICODE_LETTER | MARKDOWN_CORE_UNICODE_NUMBER)
                    ? width
                    : 0;
        state->work++;
        if (!width && c == '-' && pos > label && !markdown_core_reader_ends(reader, pos + 1)) {
            unsigned char next = markdown_core_reader_peek(reader, pos + 1);
            int following = next == '_' ? 1 : S_key_width(reader, pos + 1);
            width = following ? 1 + following : 0;
        }
        if (!width) {
            break;
        }
        pos += width;
    }
    if (markdown_core_reader_peek(reader, pos) != ')' ||
        !markdown_core_is_whitespace(markdown_core_reader_peek(reader, pos + 1))) {
        return 0;
    }
    if (pos > label) {
        value->label = markdown_core_optional_chunk_present(
            (markdown_core_chunk){(unsigned char *)markdown_core_reader_bytes(reader, label), pos - label, 0});
    }
    return pos + 1 - begin;
}

/* The marker's motion: past the marker and the spaces after it. */
static void S_specimen_marker(markdown_core_specimen_state *state, markdown_core_reader *reader, bufsize_t matched) {
    markdown_core_reader_advance(reader, reader->nonspace + matched - reader->offset, false);
    while (markdown_core_is_space_or_tab(markdown_core_reader_peek(reader, reader->offset))) {
        markdown_core_reader_advance(reader, 1, true);
        state->work++;
    }
}

static bool markdown_core_specimen_open(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                        markdown_core_node **container, markdown_core_reader *reader,
                                        block_start *start) {
    bufsize_t matched = start->matched;
    markdown_core_specimen_value specimen = start->specimen;

    if (specimen.label.has_value && !markdown_core_chunk_to_cstr(&specimen.label.value)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    *container = markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_SPECIMEN, reader->nonspace + 1);
    if (!*container) {
        markdown_core_optional_chunk_free(&specimen.label);
        return false;
    }
    /* What it numbers from is the block before it, which another reading
     * may make another kind or another block: its opener reads it. */
    const markdown_core_node *previous = markdown_core_reader_before(reader, parser, *container);
    if (previous && previous->kind == MARKDOWN_CORE_NODE_SPECIMEN) {
        specimen.has_start = false;
        specimen.start = 0;
    }
    *(*container)->as.specimen = specimen;
    markdown_core_specimen_state *state = self->state;
    /* A citation names a specimen by its label as written. */
    if (specimen.label.has_value &&
        !markdown_core_parser_declare(parser, MARKDOWN_CORE_ROSTER_SPECIMEN, &specimen.label.value,
                                      (*container)->place.start, NULL)) {
        return false;
    }
    S_specimen_marker(state, reader, matched);
    return true;
}

/* What the open reads past the marker (block_start.reads). */
static void S_specimen_reads(const markdown_core_element_instance *self, markdown_core_reader *reader,
                             const block_start *start) {
    S_specimen_marker(self->state, reader, start->matched);
}

static bool markdown_core_specimen_scan(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                        block_start_context *context, block_start *start) {
    (void)parser;
    markdown_core_reader *reader = context->reader;
    int first = reader->nonspace;
    if (!(!context->paragraph &&
          (start->matched = markdown_core_block_parse_specimen_marker(self->state, reader, first, &start->specimen)))) {
        return false;
    }
    start->kind = MARKDOWN_CORE_NODE_SPECIMEN;
    start->open = markdown_core_specimen_open;
    start->reads = S_specimen_reads;
    return true;
}

bool markdown_core_specimen_continue(markdown_core_parser *parser, markdown_core_node *container,
                                     markdown_core_reader *reader) {
    return markdown_core_reader_indented(reader, 4, true);
}

static bool continue_container(const markdown_core_element_instance *self, markdown_core_parser *parser,
                               markdown_core_node *node, markdown_core_reader *reader,
                               const markdown_core_node *joining, bool *taken) {
    (void)self;
    return markdown_core_specimen_continue(parser, node, reader);
}
static const markdown_core_edit_ops SPECIMEN_EDIT = {
    .reopens = true,
};

static const markdown_core_open_ops SPECIMEN_OPENS = {
    .indent = 3,
    .scan = markdown_core_specimen_scan,
    .scan_needs = {.bytes = "("},
};

static const markdown_core_line_ops SPECIMEN_LINES = {
    .prefix = continue_container,
};

const markdown_core_facet MARKDOWN_CORE_SPECIMEN_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_SPECIMEN, .parts = markdown_core_specimen_parts},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_SPECIMEN = {
    .facets = MARKDOWN_CORE_SPECIMEN_FACETS,
    .edit = &SPECIMEN_EDIT,
    .name = "specimen",
    .state_size = sizeof(markdown_core_specimen_state),
    .opens = &SPECIMEN_OPENS,
    .lines = &SPECIMEN_LINES,
};

/* A specimen's parts: its start number, a field of no slot kind, and its
 * label, between the `@` and the `)`. */
void markdown_core_specimen_parts(markdown_core_parts *parts) {
    const unsigned char *text = parts->text;
    bufsize_t at = 0;
    while (at < parts->length && text[at] != '(') {
        at++;
    }
    const bufsize_t digits = ++at;
    while (at < parts->length && markdown_core_isdigit(text[at])) {
        at++;
    }
    if (at >= parts->length || text[at] != '@') {
        return;
    }
    markdown_core_parts_add(parts, MARKDOWN_CORE_SLOT_NONE, digits, at);
    const bufsize_t label = ++at;
    while (at < parts->length && text[at] != ')') {
        at++;
    }
    if (at < parts->length) {
        markdown_core_parts_add(parts, MARKDOWN_CORE_SLOT_SPECIMEN, label, at);
    }
}
