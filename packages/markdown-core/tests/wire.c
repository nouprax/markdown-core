/* The MCB3 encoder as its bindings consume it (docs/architecture/wire-format.md).
 * The bindings' decoders own the record-level checks; this suite runs the
 * encoder over the canonical corpus under the C sanitizers and checks what
 * holds for every message: the header, determinism, and that a shared
 * resource crosses once however often it is used. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <markdown_core.h>
#include <markdown_core_wire.h>

static int failures = 0;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAILED: %s\n", message);
        failures++;
    }
}

static uint32_t read_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

/* The message length its header states. */
static uint32_t message_length(const uint8_t *message) { return read_u32(message + 4); }

static uint8_t *read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    long size;
    uint8_t *bytes;
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
    return bytes;
}

/* A document message: status 0, and encoding the same source again yields the
 * same bytes. */
static void check_document(const uint8_t *source, size_t length, const char *name) {
    uint8_t *first = markdown_core_wire_parse(source, length);
    uint8_t *second = markdown_core_wire_parse(source, length);
    uint32_t size;
    if (first == NULL || second == NULL) {
        fprintf(stderr, "FAILED: %s: no message\n", name);
        failures++;
    } else {
        size = message_length(first);
        if (size < 9 || first[8] != 0) {
            fprintf(stderr, "FAILED: %s: not a document message\n", name);
            failures++;
        } else if (size != message_length(second) || memcmp(first, second, size) != 0) {
            fprintf(stderr, "FAILED: %s: the encoding is not deterministic\n", name);
            failures++;
        }
    }
    markdown_core_wire_free(first);
    markdown_core_wire_free(second);
}

static void check_fixture(const char *fixture_dir, const char *name) {
    char path[1024];
    size_t length = 0;
    uint8_t *source;
    snprintf(path, sizeof(path), "%s/%s.md", fixture_dir, name);
    source = read_file(path, &length);
    if (source == NULL) {
        fprintf(stderr, "FAILED: cannot read %s\n", path);
        failures++;
        return;
    }
    check_document(source, length, name);
    free(source);
}

/* One definition referenced many times: growing the definition grows the
 * message by the definition, not by the definition times its references. */
static uint32_t references_message_length(size_t definition_size, size_t references) {
    const char *reference = "[a]\n\n";
    size_t capacity = 64 + 4 * definition_size + references * strlen(reference);
    char *source = (char *)malloc(capacity);
    size_t length = 0, index;
    uint8_t *message;
    uint32_t size;
    if (source == NULL) {
        return 0;
    }
    length += (size_t)snprintf(source, capacity, "[a]: /");
    for (index = 0; index < definition_size; ++index) {
        source[length++] = 'u';
    }
    length += (size_t)snprintf(source + length, capacity - length, " {#");
    for (index = 0; index < definition_size; ++index) {
        source[length++] = 'a';
    }
    length += (size_t)snprintf(source + length, capacity - length, " .c}\n\n");
    for (index = 0; index < references; ++index) {
        memcpy(source + length, reference, strlen(reference));
        length += strlen(reference);
    }
    message = markdown_core_wire_parse((const uint8_t *)source, length);
    size = message == NULL ? 0 : message_length(message);
    markdown_core_wire_free(message);
    free(source);
    return size;
}

static void check_shared_resource(void) {
    const size_t references = 4096, size = 4096;
    uint32_t small = references_message_length(1, references);
    uint32_t large = references_message_length(size, references);
    check(small != 0 && large != 0, "reference documents encode");
    check(large - small < 3 * size, "a definition crosses the wire once however often it is referenced");
}

/* The status a message carries: 0 for a document, the failure's otherwise. */
static uint32_t message_status(const uint8_t *message) { return message[8] == 0 ? 0 : read_u32(message + 9); }

/* A session's steps each answer with the message of the document they
 * published or of their failure, and the session's text follows the edits,
 * which a batch packs as sizes and one run of texts. */
static void check_session(void) {
    static const char source[] = "# One\n\npair \xf0\x9f\x98\x80 here\n";
    markdown_core_session *session;
    uint8_t *message = markdown_core_wire_session_new((const uint8_t *)source, strlen(source),
                                                      MARKDOWN_CORE_TEXT_UNIT_UTF16, &session);
    check(message != NULL && session != NULL && message_status(message) == 0, "a session starts with a document");
    markdown_core_wire_free(message);
    if (session == NULL) {
        return;
    }
    /* In UTF-16 the pair is offsets 12 and 13, so " here" starts at 14. */
    const size_t edits[] = {14, 19, 3, 2, 5, 3};
    message = markdown_core_wire_session_edit(session, edits, 2,
                                              (const uint8_t *)" ok"
                                                               "Two");
    check(message != NULL && message_status(message) == 0, "a batch answers with its document");
    markdown_core_wire_free(message);
    message = markdown_core_wire_session_append(session, (const uint8_t *)"\nmore", 5);
    check(message != NULL && message_status(message) == 0, "an append answers with its document");
    markdown_core_wire_free(message);
    const char expected[] = "# Two\n\npair \xf0\x9f\x98\x80 ok\n\nmore";
    size_t size = markdown_core_session_text_size(session);
    uint8_t text[64] = {0};
    if (size < sizeof(text)) {
        markdown_core_session_text(session, text);
    }
    check(size == strlen(expected) && memcmp(text, expected, size) == 0, "the session's text follows its steps");
    const size_t between[] = {13, 13, 0};
    message = markdown_core_wire_session_edit(session, between, 1, NULL);
    check(message != NULL && message_status(message) == MARKDOWN_CORE_OUT_OF_BOUNDS,
          "an offset between the units of one scalar answers OUT_OF_BOUNDS");
    markdown_core_wire_free(message);
    markdown_core_session_free(session);
}

int main(int argc, char **argv) {
    int i;
    if (argc < 4 || strcmp(argv[1], "--fixtures") != 0) {
        fputs("usage: wire_test --fixtures DIR NAME [NAME ...]\n", stderr);
        return 2;
    }
    check_shared_resource();
    check_session();
    check_document((const uint8_t *)"", 0, "the empty document");
    check_document(NULL, 0, "the empty document from a NULL source");
    for (i = 3; i < argc; i++) {
        check_fixture(argv[2], argv[i]);
    }
    if (failures) {
        fprintf(stderr, "%d wire test(s) failed\n", failures);
        return 1;
    }
    fprintf(stderr, "wire messages passed\n");
    return 0;
}
