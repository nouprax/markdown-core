#include "alloc.h"
#include "specimen.h"
#include "block_internal.h"

const markdown_core_space MARKDOWN_CORE_SPECIMEN_SPACE = {.name = "specimen"};
/* The spaces this element files labels in, as `self->spaces` numbers them. */
enum { SPECIMEN_LABELS };
static const markdown_core_space *const SPECIMEN_SPACES[] = {[SPECIMEN_LABELS] = &MARKDOWN_CORE_SPECIMEN_SPACE, NULL};

/* What a specimen's start found (WHAT THE START FOUND, block_internal.h):
 * its value, where its `(`, its `@` and its label's end stand, and the
 * digits read. */
typedef struct {
    markdown_core_specimen_value value;
    bufsize_t marker, sign, end;
    int digits;
} specimen_found;
MARKDOWN_CORE_BLOCK_FOUND_FITS(specimen_found, specimen_found_fits);

static bool markdown_core_specimen_open(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                        markdown_core_node **container, markdown_core_reader *reader,
                                        const unsigned char *line, block_start *start) {
    const specimen_found *found = MARKDOWN_CORE_BLOCK_FOUND(start, specimen_found);
    markdown_core_specimen_value specimen = found->value;
    /* Its start number, a field of no slot kind, and its label, after the
     * `(` and the `@`. */
    const bufsize_t digits = found->marker + 1;
    const bufsize_t sign = found->sign;
    const bufsize_t label = found->end;
    if (label > sign + 1) {
        specimen.label = markdown_core_optional_chunk_present(
            (markdown_core_chunk){(unsigned char *)line + sign + 1, label - sign - 1, 0});
        if (!markdown_core_chunk_to_cstr(&specimen.label.value)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return false;
        }
    }
    *container = markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_SPECIMEN, found->marker + 1);
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
    /* A citation names a specimen by its label as written; one of no label
     * is listed all the same. */
    const markdown_core_chunk none = {0};
    if (!markdown_core_parser_declare(parser, self->spaces[SPECIMEN_LABELS],
                                      specimen.label.has_value ? &specimen.label.value : &none,
                                      (*container)->place.start, NULL, *container)) {
        return false;
    }
    return true;
}

enum { SPECIMEN_OPEN, SPECIMEN_BLOCKS };

static void S_specimen_open(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)token;
    (void)index;
    markdown_core_block_turn *turn = markdown_core_block_turned(compiler);
    turn->opened = markdown_core_specimen_open(markdown_core_compile_self(compiler), turn->parser, turn->container,
                                               turn->context->reader, compiler->data + turn->base, turn->start);
}

/* THE MARKER (code by code): up to three spaces, `(`, up to nine digits
 * of a start that is not 0, `@`, a label, `)`, whitespace, and the spaces
 * after it. A LABEL IS WHAT A BARE REFERENCE READS WHOLE. A reference
 * `@label` is scanned as a citation key (citation.c): Unicode letters,
 * numbers and `_` are key characters, and internal punctuation stands
 * singly between them. Pandoc's manual gives the definition "alphanumeric
 * characters, underscores, or hyphens", and the hyphen is the one
 * punctuation mark of that class, so a label is key characters with `-`
 * singly between them, read by the same rule the reference applies -- a
 * definition the reference could not name whole would be unreachable, so
 * it is not a definition: `(@a--b)` and `(@x-)` are text. */
static inline specimen_found *S_found(const markdown_core_tokenizer *tokenizer) {
    return MARKDOWN_CORE_BLOCK_FOUND(markdown_core_block_turn_of(tokenizer)->start, specimen_found);
}

static inline bufsize_t S_offset(const markdown_core_tokenizer *tokenizer) {
    return (bufsize_t)(tokenizer->position - tokenizer->base);
}

static inline void S_work(markdown_core_tokenizer *tokenizer) {
    ((markdown_core_specimen_state *)((const markdown_core_element_instance *)markdown_core_self(tokenizer))->state)
        ->work++;
}

/* Whether the code at the reading's place, a scalar with the bytes that
 * continue it, as the reading consumes it, is a key character. */
static bool S_key(const markdown_core_tokenizer *tokenizer, int code) {
    if (code == '_') {
        return true;
    }
    const uint32_t end = tokenizer->ending < tokenizer->size ? tokenizer->ending : tokenizer->size;
    if (code < 0 || code >= 0x100 || code == '\n' || tokenizer->position >= end) {
        return false;
    }
    uint32_t at = tokenizer->position + 1;
    while (at < end && (tokenizer->data[at] & 0xC0) == 0x80) {
        at++;
    }
    int32_t scalar;
    markdown_core_utf8proc_decode(tokenizer->data + tokenizer->position, (bufsize_t)(at - tokenizer->position),
                                  &scalar);
    return markdown_core_utf8proc_classes(scalar) & (MARKDOWN_CORE_UNICODE_LETTER | MARKDOWN_CORE_UNICODE_NUMBER);
}

static markdown_core_state S_spaced(markdown_core_tokenizer *tokenizer, int code) {
    if (markdown_core_code_space(code)) {
        S_work(tokenizer);
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_spaced);
    }
    return markdown_core_block_found(tokenizer, SPECIMEN_OPEN);
}

static markdown_core_state S_closed(markdown_core_tokenizer *tokenizer, int code) {
    if (!markdown_core_code_space(code) && !markdown_core_code_ending(code)) {
        return markdown_core_nok(tokenizer);
    }
    return S_spaced(tokenizer, code);
}

static markdown_core_state S_label(markdown_core_tokenizer *tokenizer, int code);

static markdown_core_state S_dash(markdown_core_tokenizer *tokenizer, int code) {
    if (!S_key(tokenizer, code)) {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_label);
}

static markdown_core_state S_label(markdown_core_tokenizer *tokenizer, int code) {
    specimen_found *found = S_found(tokenizer);
    S_work(tokenizer);
    if (S_key(tokenizer, code)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_label);
    }
    if (code == '-' && S_offset(tokenizer) > found->sign + 1) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_dash);
    }
    if (code != ')') {
        return markdown_core_nok(tokenizer);
    }
    found->end = S_offset(tokenizer);
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_closed);
}

static markdown_core_state S_digits(markdown_core_tokenizer *tokenizer, int code) {
    specimen_found *found = S_found(tokenizer);
    if (found->digits < 9 && code >= '0' && code <= '9') {
        S_work(tokenizer);
        found->value.start = found->value.start * 10 + code - '0';
        found->digits++;
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_digits);
    }
    found->value.has_start = found->digits > 0;
    if ((found->digits && !found->value.start) || code != '@') {
        return markdown_core_nok(tokenizer);
    }
    found->sign = S_offset(tokenizer);
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_label);
}

static markdown_core_state S_marker(markdown_core_tokenizer *tokenizer, int code) {
    S_work(tokenizer);
    if (code != '(') {
        return markdown_core_nok(tokenizer);
    }
    *S_found(tokenizer) = (specimen_found){.marker = S_offset(tokenizer)};
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_digits);
}

static markdown_core_state markdown_core_specimen_scan(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    if (turn->context->paragraph) {
        return markdown_core_nok(tokenizer);
    }
    turn->start->kind = MARKDOWN_CORE_NODE_SPECIMEN;
    return markdown_core_spaces(tokenizer, markdown_core_state_of(S_marker), MARKDOWN_CORE_TOKEN_SPACE, 3, code);
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

/* THE CONTINUATION (element.h, markdown_core_line_ops `continuation`): four
 * columns of indentation, or a blank line. */
static markdown_core_state S_continue(markdown_core_tokenizer *tokenizer, int code) {
    return markdown_core_block_indented(tokenizer, 4, true, code);
}

static const markdown_core_line_ops SPECIMEN_LINES = {
    .continuation = {.start = S_continue},

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
