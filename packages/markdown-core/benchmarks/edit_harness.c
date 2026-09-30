/* The incremental harness; see edit_harness.h. */
#include "edit_harness.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- text model */

static bool text_reserve(eh_text *text, size_t length) {
    size_t capacity = text->capacity ? text->capacity : 64;
    uint8_t *bytes;
    if (length + 1 <= text->capacity) {
        return true;
    }
    while (capacity < length + 1) {
        if (capacity > SIZE_MAX / 2) {
            return false;
        }
        capacity *= 2;
    }
    bytes = (uint8_t *)realloc(text->bytes, capacity);
    if (!bytes) {
        return false;
    }
    text->bytes = bytes;
    text->capacity = capacity;
    return true;
}

bool eh_text_assign(eh_text *text, const uint8_t *bytes, size_t length) {
    if (!text_reserve(text, length)) {
        return false;
    }
    if (length) {
        memcpy(text->bytes, bytes, length);
    }
    text->length = length;
    text->bytes[length] = 0;
    return true;
}

bool eh_text_replace(eh_text *text, size_t start, size_t end, const uint8_t *bytes, size_t length) {
    size_t tail;
    size_t grown;
    if (start > end || end > text->length) {
        return false;
    }
    tail = text->length - end;
    grown = text->length - (end - start) + length;
    if (!text_reserve(text, grown)) {
        return false;
    }
    memmove(text->bytes + start + length, text->bytes + end, tail);
    if (length) {
        memcpy(text->bytes + start, bytes, length);
    }
    text->length = grown;
    text->bytes[grown] = 0;
    return true;
}

void eh_text_free(eh_text *text) {
    free(text->bytes);
    text->bytes = NULL;
    text->length = text->capacity = 0;
}

const char *eh_unit_name(eh_unit unit) { return unit == EH_UTF16 ? "utf16" : "utf8"; }

bool eh_unit_parse(const char *name, eh_unit *unit) {
    if (strcmp(name, "utf8") == 0) {
        *unit = EH_UTF8;
    } else if (strcmp(name, "utf16") == 0) {
        *unit = EH_UTF16;
    } else {
        return false;
    }
    return true;
}

/* The length of the scalar a lead byte opens, or 0 for a continuation byte
 * or a byte no scalar starts with. */
static size_t lead_length(uint8_t byte) {
    if (byte < 0x80) {
        return 1;
    }
    if (byte >= 0xc2 && byte <= 0xdf) {
        return 2;
    }
    if (byte >= 0xe0 && byte <= 0xef) {
        return 3;
    }
    if (byte >= 0xf0 && byte <= 0xf4) {
        return 4;
    }
    return 0;
}

bool eh_utf8_valid(const uint8_t *bytes, size_t length) {
    size_t at = 0;
    while (at < length) {
        uint8_t lead = bytes[at];
        size_t width = lead_length(lead);
        size_t index;
        if (!width || width > length - at) {
            return false;
        }
        for (index = 1; index < width; index++) {
            if ((bytes[at + index] & 0xc0) != 0x80) {
                return false;
            }
        }
        /* Shortest form, no surrogates, nothing above U+10FFFF. */
        if ((lead == 0xe0 && bytes[at + 1] < 0xa0) || (lead == 0xed && bytes[at + 1] > 0x9f) ||
            (lead == 0xf0 && bytes[at + 1] < 0x90) || (lead == 0xf4 && bytes[at + 1] > 0x8f)) {
            return false;
        }
        at += width;
    }
    return true;
}

bool eh_utf8_boundary(const uint8_t *bytes, size_t length, size_t offset) {
    return offset == 0 || offset == length || (offset < length && (bytes[offset] & 0xc0) != 0x80);
}

size_t eh_utf16_units(const uint8_t *bytes, size_t offset) {
    size_t units = 0;
    size_t at;
    for (at = 0; at < offset; at++) {
        if ((bytes[at] & 0xc0) != 0x80) {
            units += bytes[at] >= 0xf0 ? 2 : 1;
        }
    }
    return units;
}

bool eh_utf8_offset(const uint8_t *bytes, size_t length, size_t units, size_t *offset) {
    size_t at = 0;
    size_t counted = 0;
    while (counted < units) {
        size_t width;
        if (at >= length) {
            return false;
        }
        width = lead_length(bytes[at]);
        if (!width) {
            return false;
        }
        counted += width == 4 ? 2 : 1;
        at += width;
    }
    if (counted != units || at > length) {
        return false;
    }
    *offset = at;
    return true;
}

/* ------------------------------------------------------------------ files */

uint8_t *eh_read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    uint8_t *bytes;
    long size;
    if (!file) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    bytes = (uint8_t *)malloc((size_t)size + 1);
    if (!bytes) {
        fclose(file);
        return NULL;
    }
    *length = fread(bytes, 1, (size_t)size, file);
    fclose(file);
    if (*length != (size_t)size) {
        free(bytes);
        return NULL;
    }
    bytes[*length] = 0;
    return bytes;
}

/* Split `line` into space-separated fields in place. */
static size_t split_fields(char *line, char ***fields) {
    size_t count = 0;
    size_t capacity = 0;
    char **list = NULL;
    char *cursor = line;
    while (*cursor) {
        char *start;
        while (*cursor == ' ') {
            cursor++;
        }
        if (!*cursor) {
            break;
        }
        start = cursor;
        while (*cursor && *cursor != ' ') {
            cursor++;
        }
        if (*cursor) {
            *cursor++ = 0;
        }
        if (count == capacity) {
            char **grown;
            capacity = capacity ? capacity * 2 : 16;
            grown = (char **)realloc(list, capacity * sizeof(*list));
            if (!grown) {
                free(list);
                *fields = NULL;
                return (size_t)-1;
            }
            list = grown;
        }
        list[count++] = start;
    }
    *fields = list;
    return count;
}

static bool parse_size(const char *field, size_t *value) {
    char *end;
    unsigned long long parsed;
    if (!field || !*field || *field == '-') {
        return false;
    }
    errno = 0;
    parsed = strtoull(field, &end, 10);
    if (errno || *end || parsed > SIZE_MAX) {
        return false;
    }
    *value = (size_t)parsed;
    return true;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/* `-` is the empty text; anything else is lowercase hex. */
static bool parse_hex(const char *field, uint8_t **bytes, size_t *length) {
    size_t digits = strlen(field);
    size_t index;
    if (strcmp(field, "-") == 0) {
        *bytes = NULL;
        *length = 0;
        return true;
    }
    if (!digits || digits % 2) {
        return false;
    }
    *bytes = (uint8_t *)malloc(digits / 2);
    if (!*bytes) {
        return false;
    }
    for (index = 0; index < digits / 2; index++) {
        int high = hex_digit(field[2 * index]);
        int low = hex_digit(field[2 * index + 1]);
        if (high < 0 || low < 0) {
            free(*bytes);
            *bytes = NULL;
            return false;
        }
        (*bytes)[index] = (uint8_t)(high * 16 + low);
    }
    *length = digits / 2;
    return true;
}

/* Iterate the lines of a text file; each call NUL-terminates and returns the
 * next line, or NULL at the end. */
static char *next_line(char **cursor) {
    char *line = *cursor;
    char *end;
    if (!line || !*line) {
        return NULL;
    }
    end = strchr(line, '\n');
    if (end) {
        *end = 0;
        *cursor = end + 1;
    } else {
        *cursor = line + strlen(line);
    }
    return line;
}

static char *copy_string(const char *value) {
    size_t length = strlen(value);
    char *copy = (char *)malloc(length + 1);
    if (copy) {
        memcpy(copy, value, length + 1);
    }
    return copy;
}

/* ---------------------------------------------------------------- scripts */

static const char SCRIPT_HEADER[] = "markdown-core edit script 1";
static const char MANIFEST_HEADER[] = "markdown-core incremental manifest 1";

static void step_free(eh_step *step) {
    size_t index;
    for (index = 0; index < step->count; index++) {
        free(step->edits[index].text);
    }
    free(step->edits);
    for (index = 0; index < step->expectation_count; index++) {
        free(step->expectations[index].verb);
        free(step->expectations[index].kind);
    }
    free(step->expectations);
}

static void script_free(eh_script *script) {
    size_t index;
    for (index = 0; index < script->count; index++) {
        step_free(&script->steps[index]);
    }
    free(script->steps);
    free(script->name);
    free(script->family);
}

void eh_scripts_free(eh_scripts *scripts) {
    size_t index;
    for (index = 0; index < scripts->count; index++) {
        script_free(&scripts->scripts[index]);
    }
    free(scripts->scripts);
    scripts->scripts = NULL;
    scripts->count = 0;
}

/* Edits written as `start end hex` triples. */
static bool parse_edits(char **fields, size_t count, eh_step *step) {
    size_t index;
    if (!count || count % 3) {
        return false;
    }
    step->edits = (eh_edit *)calloc(count / 3, sizeof(*step->edits));
    if (!step->edits) {
        return false;
    }
    step->count = count / 3;
    for (index = 0; index < step->count; index++) {
        eh_edit *edit = &step->edits[index];
        if (!parse_size(fields[3 * index], &edit->start) || !parse_size(fields[3 * index + 1], &edit->end) ||
            !parse_hex(fields[3 * index + 2], &edit->text, &edit->length)) {
            return false;
        }
    }
    return true;
}

static bool parse_expectation(char **fields, size_t count, eh_step *step) {
    eh_expectation *grown;
    eh_expectation *expectation;
    bool only;
    if (!count) {
        return false;
    }
    only = strcmp(fields[0], "only") == 0;
    if (only ? count != 1 : count != (strcmp(fields[0], "kept") == 0 ? 4u : 3u)) {
        return false;
    }
    if (!only && strcmp(fields[0], "kept") != 0 && strcmp(fields[0], "new") != 0 && strcmp(fields[0], "retired") != 0 &&
        strcmp(fields[0], "changed") != 0) {
        return false;
    }
    grown = (eh_expectation *)realloc(step->expectations, (step->expectation_count + 1) * sizeof(*grown));
    if (!grown) {
        return false;
    }
    step->expectations = grown;
    expectation = &step->expectations[step->expectation_count++];
    memset(expectation, 0, sizeof(*expectation));
    expectation->verb = copy_string(fields[0]);
    if (!expectation->verb) {
        return false;
    }
    if (only) {
        return true;
    }
    expectation->kind = copy_string(fields[1]);
    if (!expectation->kind) {
        return false;
    }
    if (strcmp(fields[0], "kept") == 0) {
        return parse_size(fields[2], &expectation->from) && parse_size(fields[3], &expectation->at);
    }
    return parse_size(fields[2], &expectation->at);
}

static bool parse_step(char **fields, size_t count, eh_script *script) {
    eh_step step;
    eh_step *grown;
    memset(&step, 0, sizeof(step));
    if (strcmp(fields[0], "expect") == 0) {
        return script->count && parse_expectation(fields + 1, count - 1, &script->steps[script->count - 1]);
    }
    if (strcmp(fields[0], "edit") == 0) {
        step.kind = EH_STEP_EDIT;
        if (!parse_edits(fields + 1, count - 1, &step)) {
            step_free(&step);
            return false;
        }
    } else if (strcmp(fields[0], "append") == 0) {
        step.kind = EH_STEP_APPEND;
        step.edits = (eh_edit *)calloc(1, sizeof(*step.edits));
        step.count = 1;
        if (count != 2 || !step.edits || !parse_hex(fields[1], &step.edits[0].text, &step.edits[0].length)) {
            step_free(&step);
            return false;
        }
    } else if (strcmp(fields[0], "reject") == 0 && count >= 4 && eh_unit_parse(fields[1], &step.unit)) {
        if (strcmp(fields[2], "edit") == 0) {
            step.kind = EH_STEP_REJECT_EDIT;
            if (!parse_edits(fields + 3, count - 3, &step)) {
                step_free(&step);
                return false;
            }
        } else if (strcmp(fields[2], "append") == 0 && count == 4) {
            step.kind = EH_STEP_REJECT_APPEND;
            step.edits = (eh_edit *)calloc(1, sizeof(*step.edits));
            step.count = 1;
            if (!step.edits || !parse_hex(fields[3], &step.edits[0].text, &step.edits[0].length)) {
                step_free(&step);
                return false;
            }
        } else {
            return false;
        }
    } else {
        return false;
    }
    grown = (eh_step *)realloc(script->steps, (script->count + 1) * sizeof(*grown));
    if (!grown) {
        step_free(&step);
        return false;
    }
    script->steps = grown;
    script->steps[script->count++] = step;
    return true;
}

static bool parse_line(char **fields, size_t count, eh_scripts *scripts) {
    if (strcmp(fields[0], "script") == 0) {
        eh_script *grown;
        eh_script *script;
        if (count != 3) {
            return false;
        }
        grown = (eh_script *)realloc(scripts->scripts, (scripts->count + 1) * sizeof(*grown));
        if (!grown) {
            return false;
        }
        scripts->scripts = grown;
        script = &scripts->scripts[scripts->count++];
        memset(script, 0, sizeof(*script));
        script->name = copy_string(fields[1]);
        script->family = copy_string(fields[2]);
        return script->name && script->family;
    }
    return scripts->count && parse_step(fields, count, &scripts->scripts[scripts->count - 1]);
}

bool eh_scripts_load(const char *path, eh_scripts *scripts) {
    size_t length = 0;
    char *text = (char *)eh_read_file(path, &length);
    char *cursor = text;
    char *line;
    size_t number = 1;
    memset(scripts, 0, sizeof(*scripts));
    if (!text) {
        fprintf(stderr, "%s: cannot read\n", path);
        return false;
    }
    line = next_line(&cursor);
    if (!line || strcmp(line, SCRIPT_HEADER) != 0) {
        fprintf(stderr, "%s:1: not an edit script\n", path);
        free(text);
        return false;
    }
    while ((line = next_line(&cursor)) != NULL) {
        char **fields = NULL;
        size_t count = split_fields(line, &fields);
        number++;
        if (count == (size_t)-1 || !count || !parse_line(fields, count, scripts)) {
            fprintf(stderr, "%s:%zu: malformed line\n", path, number);
            free(fields);
            free(text);
            eh_scripts_free(scripts);
            return false;
        }
        free(fields);
    }
    free(text);
    return true;
}

/* ---------------------------------------------------------------- streams */

bool eh_sizes_load(const char *path, eh_sizes *sizes) {
    size_t length = 0;
    char *text = (char *)eh_read_file(path, &length);
    char **fields = NULL;
    size_t count;
    size_t index;
    memset(sizes, 0, sizeof(*sizes));
    if (!text) {
        return false;
    }
    if (length && text[length - 1] == '\n') {
        text[length - 1] = 0;
    }
    count = split_fields(text, &fields);
    if (count == (size_t)-1 || !count || !(sizes->values = (size_t *)malloc(count * sizeof(size_t)))) {
        free(fields);
        free(text);
        return false;
    }
    for (index = 0; index < count; index++) {
        if (!parse_size(fields[index], &sizes->values[index]) || !sizes->values[index]) {
            free(fields);
            free(text);
            eh_sizes_free(sizes);
            return false;
        }
    }
    sizes->count = count;
    free(fields);
    free(text);
    return true;
}

void eh_sizes_free(eh_sizes *sizes) {
    free(sizes->values);
    sizes->values = NULL;
    sizes->count = 0;
}

static bool push_end(size_t **ends, size_t *count, size_t *capacity, size_t value) {
    if (*count == *capacity) {
        size_t grown = *capacity ? *capacity * 2 : 256;
        size_t *list = (size_t *)realloc(*ends, grown * sizeof(size_t));
        if (!list) {
            return false;
        }
        *ends = list;
        *capacity = grown;
    }
    (*ends)[(*count)++] = value;
    return true;
}

bool eh_stream_ends(const uint8_t *document, size_t length, const char *family, const eh_sizes *tokens, size_t **ends,
                    size_t *count) {
    size_t capacity = 0;
    size_t at;
    *ends = NULL;
    *count = 0;
    if (strcmp(family, "tokens") == 0) {
        size_t index = 0;
        if (!tokens || !tokens->count) {
            return false;
        }
        for (at = 0; at < length; index++) {
            at += tokens->values[index % tokens->count];
            if (at > length) {
                at = length;
            }
            while (!eh_utf8_boundary(document, length, at)) {
                at++;
            }
            if (!push_end(ends, count, &capacity, at)) {
                return false;
            }
        }
    } else if (strcmp(family, "scalars") == 0) {
        for (at = 1; at <= length; at++) {
            if (eh_utf8_boundary(document, length, at) && !push_end(ends, count, &capacity, at)) {
                return false;
            }
        }
    } else if (strcmp(family, "rows") == 0) {
        for (at = 0; at < length; at++) {
            bool ending =
                document[at] == '\n' || (document[at] == '\r' && (at + 1 == length || document[at + 1] != '\n'));
            if (ending && !push_end(ends, count, &capacity, at + 1)) {
                return false;
            }
        }
        if ((*count == 0 || (*ends)[*count - 1] != length) && length && !push_end(ends, count, &capacity, length)) {
            return false;
        }
    } else {
        return false;
    }
    return true;
}

/* --------------------------------------------------------------- manifest */

void eh_manifest_free(eh_manifest *manifest) {
    size_t index;
    for (index = 0; index < manifest->count; index++) {
        free(manifest->cases[index].document);
        free(manifest->cases[index].script);
        free(manifest->cases[index].parts);
    }
    free(manifest->cases);
    for (index = 0; index < manifest->family_count; index++) {
        free(manifest->families[index]);
    }
    free(manifest->families);
    memset(manifest, 0, sizeof(*manifest));
}

size_t eh_family_index(const eh_manifest *manifest, const char *family) {
    size_t index;
    for (index = 0; index < manifest->family_count; index++) {
        if (strcmp(manifest->families[index], family) == 0) {
            break;
        }
    }
    return index;
}

const char *eh_case_family(const eh_case *entry) {
    return entry->kind == EH_CASE_DOCUMENT ? "documents" : entry->kind == EH_CASE_STREAM ? entry->script : NULL;
}

/* A `family NAME` line: one more declared category. */
static bool declare_family(eh_manifest *manifest, const char *name) {
    char **grown = (char **)realloc(manifest->families, (manifest->family_count + 1) * sizeof(*grown));
    if (!grown) {
        return false;
    }
    manifest->families = grown;
    manifest->families[manifest->family_count] = copy_string(name);
    return manifest->families[manifest->family_count++] != NULL;
}

static bool parse_case(char **fields, size_t count, eh_case *entry) {
    memset(entry, 0, sizeof(*entry));
    if (count >= 2 && strcmp(fields[0], "document") == 0) {
        size_t index;
        entry->kind = EH_CASE_DOCUMENT;
        entry->document = copy_string(fields[1]);
        if (!entry->document) {
            return false;
        }
        if (count == 2) {
            return true;
        }
        if (strcmp(fields[2], "parts") != 0 || count < 4) {
            return false;
        }
        entry->part_count = count - 3;
        entry->parts = (size_t *)malloc(entry->part_count * sizeof(size_t));
        if (!entry->parts) {
            return false;
        }
        for (index = 0; index < entry->part_count; index++) {
            if (!parse_size(fields[3 + index], &entry->parts[index]) || !entry->parts[index]) {
                return false;
            }
        }
        return true;
    }
    if (count == 3 && (strcmp(fields[0], "edits") == 0 || strcmp(fields[0], "stream") == 0)) {
        entry->kind = strcmp(fields[0], "edits") == 0 ? EH_CASE_EDITS : EH_CASE_STREAM;
        entry->document = copy_string(fields[1]);
        entry->script = copy_string(fields[2]);
        return entry->document && entry->script;
    }
    return false;
}

bool eh_manifest_load(const char *path, eh_manifest *manifest) {
    size_t length = 0;
    char *text = (char *)eh_read_file(path, &length);
    char *cursor = text;
    char *line;
    size_t number = 1;
    size_t capacity = 0;
    memset(manifest, 0, sizeof(*manifest));
    if (!text) {
        fprintf(stderr, "%s: cannot read\n", path);
        return false;
    }
    line = next_line(&cursor);
    if (!line || strcmp(line, MANIFEST_HEADER) != 0) {
        fprintf(stderr, "%s:1: not an incremental manifest\n", path);
        free(text);
        return false;
    }
    while ((line = next_line(&cursor)) != NULL) {
        char **fields = NULL;
        size_t count = split_fields(line, &fields);
        const char *family;
        number++;
        if (count == 2 && strcmp(fields[0], "family") == 0) {
            bool declared =
                eh_family_index(manifest, fields[1]) == manifest->family_count && declare_family(manifest, fields[1]);
            free(fields);
            if (!declared) {
                fprintf(stderr, "%s:%zu: malformed or repeated family\n", path, number);
                free(text);
                eh_manifest_free(manifest);
                return false;
            }
            continue;
        }
        if (manifest->count == capacity) {
            eh_case *grown;
            capacity = capacity ? capacity * 2 : 256;
            grown = (eh_case *)realloc(manifest->cases, capacity * sizeof(*grown));
            if (!grown) {
                free(fields);
                free(text);
                eh_manifest_free(manifest);
                return false;
            }
            manifest->cases = grown;
        }
        if (count == (size_t)-1 || !parse_case(fields, count, &manifest->cases[manifest->count])) {
            fprintf(stderr, "%s:%zu: malformed entry\n", path, number);
            manifest->count++;
            free(fields);
            free(text);
            eh_manifest_free(manifest);
            return false;
        }
        manifest->count++;
        free(fields);
        family = eh_case_family(&manifest->cases[manifest->count - 1]);
        if (family && eh_family_index(manifest, family) == manifest->family_count) {
            fprintf(stderr, "%s:%zu: the entry's family is not declared\n", path, number);
            free(text);
            eh_manifest_free(manifest);
            return false;
        }
    }
    free(text);
    return true;
}

/* ---------------------------------------------------------------- reparse */

typedef struct reparse_subject {
    eh_unit unit;
    eh_text text;
} reparse_subject;

/* Validate a batch in the subject's unit and resolve it to UTF-8 offsets in
 * ascending order. Offsets out of bounds, inside a scalar, reversed or
 * overlapping, and texts that are not well-formed UTF-8, are invalid. */
static eh_status resolve(const reparse_subject *subject, const eh_edit *edits, size_t count, eh_edit *resolved) {
    size_t index;
    for (index = 0; index < count; index++) {
        eh_edit edit = edits[index];
        if (edit.start > edit.end || !eh_utf8_valid(edit.text, edit.length)) {
            return EH_INVALID;
        }
        if (subject->unit == EH_UTF16) {
            if (!eh_utf8_offset(subject->text.bytes, subject->text.length, edits[index].start, &edit.start) ||
                !eh_utf8_offset(subject->text.bytes, subject->text.length, edits[index].end, &edit.end)) {
                return EH_INVALID;
            }
        } else if (edit.end > subject->text.length ||
                   !eh_utf8_boundary(subject->text.bytes, subject->text.length, edit.start) ||
                   !eh_utf8_boundary(subject->text.bytes, subject->text.length, edit.end)) {
            return EH_INVALID;
        }
        resolved[index] = edit;
    }
    /* A stable insertion sort by range: a batch is small. */
    for (index = 1; index < count; index++) {
        eh_edit edit = resolved[index];
        size_t at = index;
        while (at > 0 && (resolved[at - 1].start > edit.start ||
                          (resolved[at - 1].start == edit.start && resolved[at - 1].end > edit.end))) {
            resolved[at] = resolved[at - 1];
            at--;
        }
        resolved[at] = edit;
    }
    for (index = 1; index < count; index++) {
        if (resolved[index].start < resolved[index - 1].end) {
            return EH_INVALID;
        }
    }
    return EH_OK;
}

eh_status eh_reparse_apply_edit(void *handle, const eh_edit *edits, size_t count) {
    reparse_subject *subject = (reparse_subject *)handle;
    eh_edit *resolved;
    eh_status status;
    size_t index;
    if (!count) {
        return EH_INVALID;
    }
    resolved = (eh_edit *)malloc(count * sizeof(*resolved));
    if (!resolved) {
        return EH_FAILED;
    }
    status = resolve(subject, edits, count, resolved);
    /* Descending, so the offsets not yet applied stay valid. */
    for (index = count; status == EH_OK && index-- > 0;) {
        if (!eh_text_replace(&subject->text, resolved[index].start, resolved[index].end, resolved[index].text,
                             resolved[index].length)) {
            status = EH_FAILED;
        }
    }
    free(resolved);
    return status;
}

eh_status eh_reparse_apply_append(void *handle, const uint8_t *text, size_t length) {
    reparse_subject *subject = (reparse_subject *)handle;
    if (!eh_utf8_valid(text, length)) {
        return EH_INVALID;
    }
    return eh_text_replace(&subject->text, subject->text.length, subject->text.length, text, length) ? EH_OK
                                                                                                     : EH_FAILED;
}

eh_status eh_reparse_parse(void *handle, markdown_core_document **document) {
    reparse_subject *subject = (reparse_subject *)handle;
    *document = NULL;
    return markdown_core_document_parse(subject->text.bytes, subject->text.length, document) == MARKDOWN_CORE_OK
               ? EH_OK
               : EH_FAILED;
}

static void *reparse_open(eh_unit unit, const uint8_t *text, size_t length, markdown_core_document **document) {
    reparse_subject *subject;
    *document = NULL;
    if (!eh_utf8_valid(text, length)) {
        return NULL;
    }
    subject = (reparse_subject *)calloc(1, sizeof(*subject));
    if (!subject) {
        return NULL;
    }
    subject->unit = unit;
    if (!eh_text_assign(&subject->text, text, length) || eh_reparse_parse(subject, document) != EH_OK) {
        eh_text_free(&subject->text);
        free(subject);
        return NULL;
    }
    return subject;
}

static eh_status reparse_edit(void *subject, const eh_edit *edits, size_t count, markdown_core_document **document) {
    eh_status status = eh_reparse_apply_edit(subject, edits, count);
    *document = NULL;
    return status == EH_OK ? eh_reparse_parse(subject, document) : status;
}

static eh_status reparse_append(void *subject, const uint8_t *text, size_t length, markdown_core_document **document) {
    eh_status status = eh_reparse_apply_append(subject, text, length);
    *document = NULL;
    return status == EH_OK ? eh_reparse_parse(subject, document) : status;
}

static const uint8_t *reparse_text(const void *handle, size_t *length) {
    const reparse_subject *subject = (const reparse_subject *)handle;
    *length = subject->text.length;
    return subject->text.bytes;
}

static void reparse_close(void *handle) {
    reparse_subject *subject = (reparse_subject *)handle;
    if (subject) {
        eh_text_free(&subject->text);
        free(subject);
    }
}

const eh_subject_class eh_reparse = {"reparse",      reparse_open, reparse_edit,
                                     reparse_append, reparse_text, reparse_close};
