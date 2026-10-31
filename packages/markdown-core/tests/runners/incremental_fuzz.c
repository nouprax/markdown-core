/* Fuzz inputs as a document and the appends and edits that follow it
 * (docs/plans/2026-09-29-incremental-gates.md, 4.10), checked with the
 * oracles of incremental_oracles.h in both units.
 *
 * An input is a u32 document length, that many document bytes, and then the
 * steps: each reads a u8 whose value modulo 32 is the length of its text,
 * which follows. Read once, every step is an append. Read again, a step is an
 * append until one whose u8 has its high bit set, and an edit from there on (plan 12.3: a flush comes before the first,
 * and no append after it): an edit reads a u16 start and a u8 width before its text, and replaces the bytes of the text
 * before it from the start, modulo the text's length and one, as far as the width reaches, each end moved back to the
 * start of the scalar it lies in (snap); an edit no cursor names in UTF-16 is passed over (named). The texts are views
 * of the input, and their bytes are never checked for being well formed, so an append or an edit may split a scalar. */
#include <stdlib.h>
#include <string.h>

#include "incremental_oracles.h"

typedef struct {
    const uint8_t *data;
    size_t size, at;
} reader;

static size_t read_bytes(reader *input, size_t count, const uint8_t **bytes) {
    size_t left = input->size - input->at;
    count = count < left ? count : left;
    *bytes = input->data + input->at;
    input->at += count;
    return count;
}

static unsigned read_u8(reader *input) {
    const uint8_t *byte;
    return read_bytes(input, 1, &byte) ? byte[0] : 0;
}

static unsigned read_u16(reader *input) {
    unsigned low = read_u8(input);
    return low | read_u8(input) << 8;
}

/* The start of the scalar `offset` lies in, or of the next when the text
 * begins in the middle of one. */
static size_t snap(const ih_text *text, size_t offset) {
    while (offset && !ih_utf8_boundary(text->bytes, text->length, offset)) {
        offset--;
    }
    while (!ih_utf8_boundary(text->bytes, text->length, offset)) {
        offset++;
    }
    return offset;
}

/* Whether a cursor names `offset` in UTF-16 too: a line that starts with
 * continuation bytes counts no column for them, so a cursor at its first
 * column stands before them. */
static bool named(const ih_text *text, size_t offset) {
    size_t at = offset;
    while (at && (text->bytes[at - 1] & 0xc0) == 0x80) {
        at--;
    }
    return at == offset || (at && text->bytes[at - 1] != '\n' && text->bytes[at - 1] != '\r');
}

/* The input's steps, appends only when `edits` is false. */
static void check_steps(run *state, const uint8_t *data, size_t size, bool edits) {
    reader input = {data, size, 0};
    size_t length = read_u16(&input);
    length |= (size_t)read_u16(&input) << 16;
    const uint8_t *document;
    ih_step *steps = NULL;
    size_t count = 0, capacity = 0;
    length = read_bytes(&input, length, &document);
    ih_text text = {0};
    bool editing = false;
    if (!ih_text_assign(&text, document, length)) {
        fail(state, "harness", "fuzz input: out of memory");
        return;
    }
    while (input.at < input.size) {
        if (count == capacity) {
            ih_step *grown;
            capacity = capacity ? capacity * 2 : 64;
            grown = (ih_step *)realloc(steps, capacity * sizeof(*steps));
            if (!grown) {
                fail(state, "harness", "fuzz input: out of memory");
                free(steps);
                ih_text_free(&text);
                return;
            }
            steps = grown;
        }
        ih_step *step = &steps[count];
        memset(step, 0, sizeof(*step));
        const unsigned head = read_u8(&input);
        editing |= edits && (head & 0x80) != 0;
        if (editing) {
            size_t start = read_u16(&input) % (text.length + 1);
            size_t end = start + read_u8(&input);
            end = end < text.length ? end : text.length;
            start = snap(&text, start);
            end = snap(&text, end);
            step->kind = IH_EDIT;
            step->start = start;
            step->end = end < start ? start : end;
        }
        step->length = read_bytes(&input, head % 32, &step->text);
        if (editing && (!named(&text, step->start) || !named(&text, step->end))) {
            continue;
        }
        const bool kept = editing ? ih_text_replace(&text, step->start, step->end, step->text, step->length)
                                  : ih_text_append(&text, step->text, step->length);
        if (!kept) {
            fail(state, "harness", "fuzz input: out of memory");
            free(steps);
            ih_text_free(&text);
            return;
        }
        count++;
    }
    ih_text_free(&text);
    run_steps(state, "fuzz input", IH_UTF8, document, length, steps, count);
    run_steps(state, "fuzz input", IH_UTF16, document, length, steps, count);
    free(steps);
}

void check_script_bytes(run *state, const uint8_t *data, size_t size) {
    check_steps(state, data, size, false);
    check_steps(state, data, size, true);
}
