/* Fuzz inputs as a document and an edit script (docs/plans/2026-09-29-
 * incremental-gates.md, 4.11), checked with the oracles of
 * incremental_oracles.h in both units.
 *
 * An input is a u32 document length, that many document bytes, and then the
 * script: each step opens with an operation byte. Its low two bits name the
 * step -- 0 and 1 an edit batch of one to three edits, 2 an append, 3 a
 * rejection -- and the rest of the byte its variant. An edit reads a u16
 * start and a u16 extent, which the decoder maps into the text and onto the
 * boundaries of what the text's bytes begin, and a u8 whose value modulo 32
 * is the length of its text, which follows; the edits of a batch follow one
 * another in the text. An append reads its text the same way. A rejection is an edit batch
 * the session refuses: out of bounds, a reversed range, a range past the end
 * or two overlapping edits; inside a scalar, the UTF-8 offset of a
 * continuation byte or, when the text holds a four-byte scalar, the UTF-16
 * offset between its two units. The decoder keeps its own copy of the text, so
 * every accepted step is valid against the text before it; the bytes are
 * never checked for being well formed. */
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

/* `offset` moved back onto a scalar boundary, or forward past a leading
 * continuation byte, which has none before it. */
static size_t snap(const eh_text *text, size_t offset) {
    while (offset && !eh_utf8_boundary(text->bytes, text->length, offset)) {
        offset--;
    }
    while (!eh_utf8_boundary(text->bytes, text->length, offset)) {
        offset++;
    }
    return offset;
}

static bool push_step(eh_script *script, eh_step step) {
    eh_step *grown = (eh_step *)realloc(script->steps, (script->count + 1) * sizeof(*grown));
    if (!grown) {
        free(step.edits);
        return false;
    }
    script->steps = grown;
    script->steps[script->count++] = step;
    return true;
}

/* Edits whose texts point into the input, which outlives the script. */
static bool decode_batch(reader *input, unsigned variant, eh_text *model, eh_script *script) {
    size_t count = 1 + variant % 3, from = 0, index;
    eh_edit *edits = (eh_edit *)calloc(count, sizeof(*edits));
    const uint8_t *text;
    if (!edits) {
        return false;
    }
    for (index = 0; index < count && from <= model->length; index++) {
        size_t start = snap(model, from + read_u16(input) % (model->length - from + 1));
        size_t end = snap(model, start + read_u16(input) % (model->length - start + 1));
        if (index && start <= edits[index - 1].end) {
            break;
        }
        edits[index].start = start;
        edits[index].end = end;
        edits[index].length = read_bytes(input, read_u8(input) % 32, &text);
        edits[index].text = (uint8_t *)text;
        from = end + 1;
    }
    count = index;
    /* The model takes the batch last edit first, so earlier offsets hold. */
    for (index = count; index--;) {
        if (!eh_text_replace(model, edits[index].start, edits[index].end, edits[index].text, edits[index].length)) {
            free(edits);
            return false;
        }
    }
    return push_step(script, (eh_step){.kind = EH_STEP_EDIT, .edits = edits, .count = count});
}

static bool decode_append(reader *input, eh_text *model, eh_script *script) {
    eh_edit *edit = (eh_edit *)calloc(1, sizeof(*edit));
    const uint8_t *text;
    if (!edit) {
        return false;
    }
    edit->length = read_bytes(input, read_u8(input) % 32, &text);
    edit->text = (uint8_t *)text;
    if (!eh_text_replace(model, model->length, model->length, text, edit->length)) {
        free(edit);
        return false;
    }
    return push_step(script, (eh_step){.kind = EH_STEP_APPEND, .edits = edit, .count = 1});
}

static bool decode_rejection(unsigned variant, const eh_text *model, eh_script *script) {
    eh_edit *edits = (eh_edit *)calloc(2, sizeof(*edits));
    eh_step step = {.kind = EH_STEP_REJECT, .unit = EH_UTF8, .refusal = EH_OUT_OF_BOUNDS, .edits = edits, .count = 1};
    size_t length = model->length, at;
    if (!edits) {
        return false;
    }
    switch (variant % 5) {
    case 0:
        edits[0] = (eh_edit){length ? length : 1, length ? length - 1 : 0, NULL, 0};
        break;
    case 1:
        edits[0] = (eh_edit){length, length + 1, NULL, 0};
        break;
    case 2:
        at = snap(model, 0);
        if (at < length) {
            edits[0] = (eh_edit){at, length, NULL, 0};
            edits[1] = (eh_edit){at, length, NULL, 0};
            step.count = 2;
            break;
        }
        edits[0] = (eh_edit){length + 1, length + 1, NULL, 0};
        break;
    case 3:
        for (at = 0; at < length && eh_utf8_boundary(model->bytes, length, at); at++) {
        }
        if (at == length) {
            free(edits);
            return true;
        }
        edits[0] = (eh_edit){at, at, NULL, 0};
        step.refusal = EH_INSIDE_SCALAR;
        break;
    default:
        for (at = 0; at < length && model->bytes[at] < 0xf0; at++) {
        }
        if (at == length) {
            free(edits);
            return true;
        }
        at = eh_utf16_units(model->bytes, at) + 1;
        edits[0] = (eh_edit){at, at, NULL, 0};
        step.unit = EH_UTF16;
        step.refusal = EH_INSIDE_SCALAR;
        break;
    }
    return push_step(script, step);
}

void check_script_bytes(run *state, const uint8_t *data, size_t size) {
    reader input = {data, size, 0};
    size_t length = read_u16(&input);
    length |= (size_t)read_u16(&input) << 16;
    const uint8_t *document;
    eh_text model = {0};
    eh_script script = {(char *)"fuzz", (char *)"fuzz", NULL, 0};
    bool decoded;
    size_t index;
    length = read_bytes(&input, length, &document);
    decoded = eh_text_assign(&model, document, length);
    while (decoded && input.at < input.size) {
        unsigned operation = read_u8(&input);
        switch (operation & 3) {
        case 0:
        case 1:
            decoded = decode_batch(&input, operation >> 2, &model, &script);
            break;
        case 2:
            decoded = decode_append(&input, &model, &script);
            break;
        default:
            decoded = decode_rejection(operation >> 2, &model, &script);
            break;
        }
    }
    if (!decoded) {
        fail(state, "harness", "fuzz input: out of memory");
    } else {
        run_edits(state, "fuzz input", EH_UTF8, document, length, &script);
        run_edits(state, "fuzz input", EH_UTF16, document, length, &script);
    }
    for (index = 0; index < script.count; index++) {
        free(script.steps[index].edits);
    }
    free(script.steps);
    eh_text_free(&model);
}
