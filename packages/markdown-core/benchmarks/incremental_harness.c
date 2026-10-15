/* The incremental harness; see incremental_harness.h. */
#include "incremental_harness.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- text model */

static bool text_reserve(ih_text *text, size_t length) {
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

bool ih_text_assign(ih_text *text, const uint8_t *bytes, size_t length) {
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

bool ih_text_append(ih_text *text, const uint8_t *bytes, size_t length) {
    if (!text_reserve(text, text->length + length)) {
        return false;
    }
    if (length) {
        memcpy(text->bytes + text->length, bytes, length);
    }
    text->length += length;
    text->bytes[text->length] = 0;
    return true;
}

void ih_text_free(ih_text *text) {
    free(text->bytes);
    text->bytes = NULL;
    text->length = text->capacity = 0;
}

const char *ih_unit_name(ih_unit unit) { return unit == IH_UTF16 ? "utf16" : "utf8"; }

bool ih_utf8_boundary(const uint8_t *bytes, size_t length, size_t offset) {
    return offset == length || (offset < length && (bytes[offset] & 0xc0) != 0x80);
}

size_t ih_utf16_units(const uint8_t *bytes, size_t offset) {
    size_t units = 0;
    size_t at;
    for (at = 0; at < offset; at++) {
        if ((bytes[at] & 0xc0) != 0x80) {
            units += bytes[at] >= 0xf0 ? 2 : 1;
        }
    }
    return units;
}

/* ------------------------------------------------------------------ files */

uint8_t *ih_read_file(const char *path, size_t *length) {
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

static const char SCRIPT_HEADER[] = "markdown-core stream script 1";
static const char MANIFEST_HEADER[] = "markdown-core incremental manifest 2";

static void script_free(ih_script *script) {
    size_t index, at;
    for (index = 0; index < script->count; index++) {
        ih_step *step = &script->steps[index];
        for (at = 0; at < step->expectation_count; at++) {
            free(step->expectations[at].verb);
            free(step->expectations[at].kind);
        }
        free(step->expectations);
    }
    free(script->steps);
    free(script->name);
    free(script->family);
    ih_text_free(&script->bytes);
}

void ih_scripts_free(ih_scripts *scripts) {
    size_t index;
    for (index = 0; index < scripts->count; index++) {
        script_free(&scripts->scripts[index]);
    }
    free(scripts->scripts);
    scripts->scripts = NULL;
    scripts->count = 0;
}

/* Appends the bytes a field spells to `bytes`: `-` is the empty text, and
 * anything else is lowercase hex. */
static bool append_hex(const char *field, ih_text *bytes, size_t *length) {
    size_t digits = strlen(field);
    size_t index;
    if (strcmp(field, "-") == 0) {
        *length = 0;
        return ih_text_append(bytes, NULL, 0);
    }
    if (!digits || digits % 2) {
        return false;
    }
    for (index = 0; index < digits / 2; index++) {
        int high = hex_digit(field[2 * index]);
        int low = hex_digit(field[2 * index + 1]);
        uint8_t byte;
        if (high < 0 || low < 0) {
            return false;
        }
        byte = (uint8_t)(high * 16 + low);
        if (!ih_text_append(bytes, &byte, 1)) {
            return false;
        }
    }
    *length = digits / 2;
    return true;
}

static bool parse_expectation(char **fields, size_t count, ih_step *step) {
    ih_expectation *grown;
    ih_expectation *expectation;
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
    grown = (ih_expectation *)realloc(step->expectations, (step->expectation_count + 1) * sizeof(*grown));
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

/* An `append HEX` line, whose bytes the script keeps, or an `expect` line,
 * which belongs to the step before it. */
static bool parse_step(char **fields, size_t count, ih_script *script) {
    ih_step *grown;
    if (strcmp(fields[0], "expect") == 0) {
        return script->count && parse_expectation(fields + 1, count - 1, &script->steps[script->count - 1]);
    }
    if (strcmp(fields[0], "append") != 0 || count != 2) {
        return false;
    }
    grown = (ih_step *)realloc(script->steps, (script->count + 1) * sizeof(*grown));
    if (!grown) {
        return false;
    }
    script->steps = grown;
    memset(&script->steps[script->count], 0, sizeof(*grown));
    return append_hex(fields[1], &script->bytes, &script->steps[script->count++].length);
}

static bool parse_line(char **fields, size_t count, ih_scripts *scripts) {
    if (strcmp(fields[0], "script") == 0) {
        ih_script *grown;
        ih_script *script;
        if (count != 3) {
            return false;
        }
        grown = (ih_script *)realloc(scripts->scripts, (scripts->count + 1) * sizeof(*grown));
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

bool ih_scripts_load(const char *path, ih_scripts *scripts) {
    size_t length = 0;
    char *text = (char *)ih_read_file(path, &length);
    char *cursor = text;
    char *line;
    size_t number = 1, index, step;
    memset(scripts, 0, sizeof(*scripts));
    if (!text) {
        fprintf(stderr, "%s: cannot read\n", path);
        return false;
    }
    line = next_line(&cursor);
    if (!line || strcmp(line, SCRIPT_HEADER) != 0) {
        fprintf(stderr, "%s:1: not a stream script\n", path);
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
            ih_scripts_free(scripts);
            return false;
        }
        free(fields);
    }
    free(text);
    /* The steps' texts lie in their script's bytes in step order; the bytes
     * no longer move. */
    for (index = 0; index < scripts->count; index++) {
        ih_script *script = &scripts->scripts[index];
        size_t at = 0;
        for (step = 0; step < script->count; step++) {
            script->steps[step].text = script->bytes.bytes + at;
            at += script->steps[step].length;
        }
    }
    return true;
}

/* ---------------------------------------------------------------- streams */

bool ih_sizes_load(const char *path, ih_sizes *sizes) {
    size_t length = 0;
    char *text = (char *)ih_read_file(path, &length);
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
            ih_sizes_free(sizes);
            return false;
        }
    }
    sizes->count = count;
    free(fields);
    free(text);
    return true;
}

void ih_sizes_free(ih_sizes *sizes) {
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

bool ih_stream_ends(const uint8_t *document, size_t length, const char *family, const ih_sizes *tokens, size_t **ends,
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
            while (!ih_utf8_boundary(document, length, at)) {
                at++;
            }
            if (!push_end(ends, count, &capacity, at)) {
                return false;
            }
        }
    } else if (strcmp(family, "scalars") == 0) {
        for (at = 1; at <= length; at++) {
            if (ih_utf8_boundary(document, length, at) && !push_end(ends, count, &capacity, at)) {
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

ih_step *ih_chunks(const uint8_t *document, const size_t *ends, size_t count) {
    ih_step *steps = (ih_step *)calloc(count ? count : 1, sizeof(*steps));
    size_t index, from = 0;
    for (index = 0; steps && index < count; index++) {
        steps[index].text = document + from;
        steps[index].length = ends[index] - from;
        from = ends[index];
    }
    return steps;
}

/* --------------------------------------------------------------- manifest */

void ih_manifest_free(ih_manifest *manifest) {
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

size_t ih_family_index(const ih_manifest *manifest, const char *family) {
    size_t index;
    for (index = 0; index < manifest->family_count; index++) {
        if (strcmp(manifest->families[index], family) == 0) {
            break;
        }
    }
    return index;
}

const char *ih_case_family(const ih_case *entry) {
    return entry->kind == IH_CASE_DOCUMENT ? "documents" : entry->kind == IH_CASE_STREAM ? entry->script : NULL;
}

/* A `family NAME` line: one more declared category. */
static bool declare_family(ih_manifest *manifest, const char *name) {
    char **grown = (char **)realloc(manifest->families, (manifest->family_count + 1) * sizeof(*grown));
    if (!grown) {
        return false;
    }
    manifest->families = grown;
    manifest->families[manifest->family_count] = copy_string(name);
    return manifest->families[manifest->family_count++] != NULL;
}

static bool parse_case(char **fields, size_t count, ih_case *entry) {
    memset(entry, 0, sizeof(*entry));
    if (count >= 2 && strcmp(fields[0], "document") == 0) {
        size_t index;
        entry->kind = IH_CASE_DOCUMENT;
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
    if (count == 3 && (strcmp(fields[0], "script") == 0 || strcmp(fields[0], "stream") == 0)) {
        entry->kind = strcmp(fields[0], "script") == 0 ? IH_CASE_SCRIPT : IH_CASE_STREAM;
        entry->document = copy_string(fields[1]);
        entry->script = copy_string(fields[2]);
        return entry->document && entry->script;
    }
    return false;
}

bool ih_manifest_load(const char *path, ih_manifest *manifest) {
    size_t length = 0;
    char *text = (char *)ih_read_file(path, &length);
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
                ih_family_index(manifest, fields[1]) == manifest->family_count && declare_family(manifest, fields[1]);
            free(fields);
            if (!declared) {
                fprintf(stderr, "%s:%zu: malformed or repeated family\n", path, number);
                free(text);
                ih_manifest_free(manifest);
                return false;
            }
            continue;
        }
        if (manifest->count == capacity) {
            ih_case *grown;
            capacity = capacity ? capacity * 2 : 256;
            grown = (ih_case *)realloc(manifest->cases, capacity * sizeof(*grown));
            if (!grown) {
                free(fields);
                free(text);
                ih_manifest_free(manifest);
                return false;
            }
            manifest->cases = grown;
        }
        if (count == (size_t)-1 || !parse_case(fields, count, &manifest->cases[manifest->count])) {
            fprintf(stderr, "%s:%zu: malformed entry\n", path, number);
            manifest->count++;
            free(fields);
            free(text);
            ih_manifest_free(manifest);
            return false;
        }
        manifest->count++;
        free(fields);
        family = ih_case_family(&manifest->cases[manifest->count - 1]);
        if (family && ih_family_index(manifest, family) == manifest->family_count) {
            fprintf(stderr, "%s:%zu: the entry's family is not declared\n", path, number);
            free(text);
            ih_manifest_free(manifest);
            return false;
        }
    }
    free(text);
    return true;
}

/* ---------------------------------------------------------------- session */

typedef struct session_subject {
    markdown_core_session *session;
    ih_text text;
} session_subject;

static void *session_open(ih_unit unit, const uint8_t *text, size_t length, const markdown_core_document **document) {
    session_subject *subject = (session_subject *)calloc(1, sizeof(*subject));
    *document = NULL;
    if (!subject) {
        return NULL;
    }
    if (markdown_core_session_new(text, length,
                                  unit == IH_UTF16 ? MARKDOWN_CORE_TEXT_UNIT_UTF16 : MARKDOWN_CORE_TEXT_UNIT_UTF8,
                                  &subject->session) != MARKDOWN_CORE_OK) {
        free(subject);
        return NULL;
    }
    *document = markdown_core_session_document(subject->session);
    return subject;
}

static bool session_append(void *handle, const uint8_t *text, size_t length, const markdown_core_document **document) {
    session_subject *subject = (session_subject *)handle;
    *document = NULL;
    return markdown_core_session_append(subject->session, text, length, document) == MARKDOWN_CORE_OK;
}

static const uint8_t *session_text(const void *handle, size_t *length) {
    session_subject *subject = (session_subject *)handle;
    *length = markdown_core_session_text_size(subject->session);
    if (!text_reserve(&subject->text, *length)) {
        return NULL;
    }
    markdown_core_session_text(subject->session, subject->text.bytes);
    subject->text.length = *length;
    return subject->text.bytes;
}

static void session_close(void *handle) {
    session_subject *subject = (session_subject *)handle;
    if (subject) {
        markdown_core_session_free(subject->session);
        ih_text_free(&subject->text);
        free(subject);
    }
}

const ih_subject_class ih_session = {"session", session_open, session_append, session_text, session_close};

/* ---------------------------------------------------------------- reparse */

typedef struct reparse_subject {
    ih_unit unit;
    ih_text text;
    markdown_core_document *document;
} reparse_subject;

bool ih_reparse_extend(void *handle, const uint8_t *text, size_t length) {
    return ih_text_append(&((reparse_subject *)handle)->text, text, length);
}

bool ih_reparse_parse(void *handle, const markdown_core_document **document) {
    reparse_subject *subject = (reparse_subject *)handle;
    markdown_core_document *parsed = NULL;
    *document = NULL;
    if (markdown_core_document_parse_in(subject->text.bytes, subject->text.length,
                                        subject->unit == IH_UTF16 ? MARKDOWN_CORE_TEXT_UNIT_UTF16
                                                                  : MARKDOWN_CORE_TEXT_UNIT_UTF8,
                                        &parsed) != MARKDOWN_CORE_OK) {
        return false;
    }
    markdown_core_document_free(subject->document);
    subject->document = parsed;
    *document = parsed;
    return true;
}

static void *reparse_open(ih_unit unit, const uint8_t *text, size_t length, const markdown_core_document **document) {
    reparse_subject *subject;
    *document = NULL;
    subject = (reparse_subject *)calloc(1, sizeof(*subject));
    if (!subject) {
        return NULL;
    }
    subject->unit = unit;
    if (!ih_text_assign(&subject->text, text, length) || !ih_reparse_parse(subject, document)) {
        ih_text_free(&subject->text);
        free(subject);
        return NULL;
    }
    return subject;
}

static bool reparse_append(void *subject, const uint8_t *text, size_t length, const markdown_core_document **document) {
    *document = NULL;
    return ih_reparse_extend(subject, text, length) && ih_reparse_parse(subject, document);
}

static const uint8_t *reparse_text(const void *handle, size_t *length) {
    const reparse_subject *subject = (const reparse_subject *)handle;
    *length = subject->text.length;
    return subject->text.bytes;
}

static void reparse_close(void *handle) {
    reparse_subject *subject = (reparse_subject *)handle;
    if (subject) {
        markdown_core_document_free(subject->document);
        ih_text_free(&subject->text);
        free(subject);
    }
}

const ih_subject_class ih_reparse = {"reparse", reparse_open, reparse_append, reparse_text, reparse_close};
