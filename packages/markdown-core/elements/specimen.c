#include "alloc.h"
#include "specimen.h"
#include "block_internal.h"

const markdown_core_space MARKDOWN_CORE_SPECIMEN_SPACE = {.name = "specimen"};
/* The spaces this element files labels in, as `self->spaces` numbers them. */
enum { SPECIMEN_LABELS };
static const markdown_core_space *const SPECIMEN_SPACES[] = {[SPECIMEN_LABELS] = &MARKDOWN_CORE_SPECIMEN_SPACE, NULL};

MARKDOWN_CORE_BLOCK_FOUND_FITS(markdown_core_specimen_value, specimen_marker_fits);

static bufsize_t markdown_core_block_parse_specimen_marker(markdown_core_specimen_state *state,
                                                           markdown_core_reader *reader, bufsize_t pos,
                                                           markdown_core_specimen_value *value);
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
    markdown_core_specimen_value specimen = *MARKDOWN_CORE_BLOCK_FOUND(start, markdown_core_specimen_value);
    /* Its start number, a field of no slot kind, and its label, after the
     * `(` and the `@`. */
    const bufsize_t digits = reader->nonspace + 1;
    bufsize_t sign = digits;
    while (markdown_core_isdigit(markdown_core_reader_peek(reader, sign))) {
        sign++;
    }
    const bufsize_t label = specimen.label.has_value ? sign + 1 + specimen.label.value.len : sign + 1;

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
    const markdown_core_field_to fields = {parser, *container, NULL, reader, 0, NULL};
    markdown_core_field_add(&fields, MARKDOWN_CORE_SLOT_NONE, digits, sign);
    markdown_core_field_add(&fields, MARKDOWN_CORE_SLOT_SPECIMEN, sign + 1, label);
    markdown_core_specimen_state *state = self->state;
    /* A citation names a specimen by its label as written; one of no label
     * is listed all the same. */
    const markdown_core_chunk none = {0};
    if (!markdown_core_parser_declare(parser, self->spaces[SPECIMEN_LABELS],
                                      specimen.label.has_value ? &specimen.label.value : &none,
                                      (*container)->place.start, NULL, *container)) {
        return false;
    }
    S_specimen_marker(state, reader, matched);
    return true;
}

enum { SPECIMEN_OPEN, SPECIMEN_BLOCKS };

static void S_specimen_open(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)token;
    (void)index;
    markdown_core_block_turn *turn = markdown_core_block_turned(compiler);
    turn->opened = markdown_core_specimen_open(markdown_core_compile_self(compiler), turn->parser, turn->container,
                                               turn->context->reader, turn->start);
}

static markdown_core_state markdown_core_specimen_scan(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    markdown_core_parser *parser = markdown_core_block_turn_of(tokenizer)->parser;
    block_start_context *context = markdown_core_block_turn_of(tokenizer)->context;
    block_start *start = markdown_core_block_turn_of(tokenizer)->start;
    (void)parser;
    (void)parser;
    markdown_core_reader *reader = context->reader;
    int first = reader->nonspace;
    if (!(!context->paragraph &&
          (start->matched = markdown_core_block_parse_specimen_marker(
               self->state, reader, first, MARKDOWN_CORE_BLOCK_FOUND(start, markdown_core_specimen_value))))) {
        return markdown_core_nok(tokenizer);
    }
    start->kind = MARKDOWN_CORE_NODE_SPECIMEN;
    return markdown_core_block_found(tokenizer, SPECIMEN_OPEN);
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

static const markdown_core_block_construct SPECIMEN_STARTS[] = {
    {.bytes = "(", .indent = 3, .construct = {.start = markdown_core_specimen_scan}},
};

static const markdown_core_compile_ops SPECIMEN_BLOCK_TOKENS[SPECIMEN_BLOCKS] = {
    [SPECIMEN_OPEN] = {.enter = S_specimen_open},
};

static const markdown_core_open_ops SPECIMEN_OPENS = {
    .starts = SPECIMEN_STARTS,
    .start_count = 1,
    .tokens = SPECIMEN_BLOCK_TOKENS,
    .token_count = SPECIMEN_BLOCKS,
};

static const markdown_core_line_ops SPECIMEN_LINES = {
    .prefix = continue_container,
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_SPECIMEN = {
    .edit = &SPECIMEN_EDIT,
    .name = "specimen",
    .spaces = SPECIMEN_SPACES,
    .state_size = sizeof(markdown_core_specimen_state),
    .opens = &SPECIMEN_OPENS,
    .lines = &SPECIMEN_LINES,
};

/* A specimen definition's record (markdown_core_record_ops): its label. */
static void free_specimen(markdown_core_node_pool *pool, markdown_core_node *node) {
    (void)pool;
    markdown_core_optional_chunk_free(&node->as.specimen->label);
}

const markdown_core_record_ops MARKDOWN_CORE_RECORD_SPECIMEN = {
    .size = sizeof(markdown_core_specimen_value),
    .free = free_specimen,
};
