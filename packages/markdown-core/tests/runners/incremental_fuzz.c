/* Fuzz inputs as a document and the appends that follow it
 * (docs/plans/2026-09-29-incremental-gates.md, 4.10), checked with the
 * oracles of incremental_oracles.h in both units.
 *
 * An input is a u32 document length, that many document bytes, and then the
 * appends: each reads a u8 whose value modulo 32 is the length of its text,
 * which follows. The texts are views of the input, and their bytes are never
 * checked for being well formed, so an append may split a scalar. */
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

void check_script_bytes(run *state, const uint8_t *data, size_t size) {
    reader input = {data, size, 0};
    size_t length = read_u16(&input);
    length |= (size_t)read_u16(&input) << 16;
    const uint8_t *document;
    ih_step *steps = NULL;
    size_t count = 0, capacity = 0;
    length = read_bytes(&input, length, &document);
    while (input.at < input.size) {
        if (count == capacity) {
            ih_step *grown;
            capacity = capacity ? capacity * 2 : 64;
            grown = (ih_step *)realloc(steps, capacity * sizeof(*steps));
            if (!grown) {
                fail(state, "harness", "fuzz input: out of memory");
                free(steps);
                return;
            }
            steps = grown;
        }
        memset(&steps[count], 0, sizeof(*steps));
        steps[count].length = read_bytes(&input, read_u8(&input) % 32, &steps[count].text);
        count++;
    }
    run_appends(state, "fuzz input", IH_UTF8, document, length, steps, count);
    run_appends(state, "fuzz input", IH_UTF16, document, length, steps, count);
    free(steps);
}
