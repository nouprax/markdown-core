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

static void check_queries(const uint8_t *source, size_t length, const char *name);

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
    check_queries(source, length, name);
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

/* A resource is a value, so the message names each value once whatever
 * storage its occurrences hold: after an append, the Link taken whole from
 * the previous document and the new occurrence of the same definition share
 * one resource ordinal, and the long definition crosses once. */
static void check_session_shared_resource(void) {
    const size_t size = 4096;
    char *source = (char *)malloc(size + 32);
    markdown_core_session *session;
    uint8_t *message;
    uint32_t before = 0, after = 0;
    size_t length = 0, index;
    if (source == NULL) {
        check(0, "the session source allocates");
        return;
    }
    length += (size_t)snprintf(source, size + 32, "[a]: /");
    for (index = 0; index < size; ++index) {
        source[length++] = 'u';
    }
    length += (size_t)snprintf(source + length, size + 32 - length, "\n\n[a]\n");
    message = markdown_core_wire_session_new((const uint8_t *)source, length, MARKDOWN_CORE_TEXT_UNIT_UTF8, &session);
    free(source);
    if (message != NULL && message_status(message) == 0) {
        before = message_length(message);
    }
    markdown_core_wire_free(message);
    if (session == NULL) {
        check(0, "a session starts with a document");
        return;
    }
    message = markdown_core_wire_session_append(session, (const uint8_t *)"\n[a]\n", 5);
    if (message != NULL && message_status(message) == 0) {
        after = message_length(message);
    }
    markdown_core_wire_free(message);
    markdown_core_session_free(session);
    check(before != 0 && after > before && after - before < size,
          "a session's new occurrence of a definition shares the resource of the occurrence it took");
}

/* A session's steps each answer with the message of the document they
 * published, and the session's text follows the appends. */
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
    message = markdown_core_wire_session_append(session, (const uint8_t *)"\nmore", 5);
    check(message != NULL && message_status(message) == 0, "an append answers with its document");
    markdown_core_wire_free(message);
    const char expected[] = "# One\n\npair \xf0\x9f\x98\x80 here\n\nmore";
    size_t size = markdown_core_session_text_size(session);
    uint8_t text[64] = {0};
    if (size < sizeof(text)) {
        markdown_core_session_text(session, text);
    }
    check(size == strlen(expected) && memcmp(text, expected, size) == 0, "the session's text follows its steps");
    message = markdown_core_wire_session_finalize(session);
    check(message != NULL && message_status(message) == 0, "finalize answers with the final document");
    markdown_core_wire_free(message);
    message = markdown_core_wire_session_append(session, (const uint8_t *)"x", 1);
    check(message != NULL && message_status(message) == MARKDOWN_CORE_SESSION_FINALIZED,
          "an append after finalize answers SESSION_FINALIZED");
    markdown_core_wire_free(message);
    message = markdown_core_wire_session_finalize(session);
    check(message != NULL && message_status(message) == MARKDOWN_CORE_SESSION_FINALIZED,
          "a finalize after finalize answers SESSION_FINALIZED");
    markdown_core_wire_free(message);
    markdown_core_session_free(session);
}

static int32_t read_i32(const uint8_t *bytes) { return (int32_t)read_u32(bytes); }

/* Whether `message` is a [Scope] answer that lists exactly `count` `scopes`. */
static int same_scopes(const uint8_t *message, const markdown_core_scope *scopes, size_t count) {
    size_t index;
    const uint8_t *at = message + 13;
    if (message == NULL || message_status(message) != 0 || read_u32(message + 9) != count ||
        message_length(message) != 13 + 16 * count) {
        return 0;
    }
    for (index = 0; index < count; ++index, at += 16) {
        if (read_i32(at) != scopes[index].start.line || read_i32(at + 4) != scopes[index].start.column ||
            read_i32(at + 8) != scopes[index].end.line || read_i32(at + 12) != scopes[index].end.column) {
            return 0;
        }
    }
    return 1;
}

/* The field and marker queries of `node`, asked through the wire by its
 * identifier and the byte at `position`, answer as the facade's do. */
static void check_parts(const markdown_core_session *session, const uint8_t *text, size_t size,
                        const markdown_core_node *node, markdown_core_position position, const char *name) {
    const markdown_core_document *document = markdown_core_session_document(session);
    uint64_t id = markdown_core_node_id(node);
    markdown_core_scope *scopes = NULL;
    size_t count = 0;
    int slot;
    uint8_t *message;
    for (slot = MARKDOWN_CORE_SLOT_NONE; slot <= MARKDOWN_CORE_SLOT_VALUE; ++slot) {
        markdown_core_document_field(document, node, (markdown_core_slot_kind)slot, text, size, &scopes, &count);
        message = markdown_core_wire_field(session, id, position, (markdown_core_slot_kind)slot);
        if (!same_scopes(message, scopes, count)) {
            fprintf(stderr, "FAILED: %s: node %llu's field %d differs through the wire\n", name, (unsigned long long)id,
                    slot);
            failures++;
        }
        markdown_core_wire_free(message);
        markdown_core_scopes_free(scopes);
    }
    markdown_core_document_markers(document, node, text, size, &scopes, &count);
    message = markdown_core_wire_markers(session, id, position);
    if (!same_scopes(message, scopes, count)) {
        fprintf(stderr, "FAILED: %s: node %llu's markers differ through the wire\n", name, (unsigned long long)id);
        failures++;
    }
    markdown_core_wire_free(message);
    markdown_core_scopes_free(scopes);
}

/* Every node a finalized session's document holds a byte of, asked through
 * the wire at the first byte that names it, answers its field and marker
 * queries as the facade does; an identifier the path at that byte lacks is
 * OUT_OF_BOUNDS. Columns count UTF-8 bytes, so a byte's position is its
 * line and its offset in the line. */
static void check_queries(const uint8_t *source, size_t length, const char *name) {
    markdown_core_session *session = NULL;
    const markdown_core_document *document;
    uint8_t *seen = NULL, *message;
    size_t limit = 0, offset, start = 0;
    int32_t line = 1;
    if (markdown_core_session_new(source, length, MARKDOWN_CORE_TEXT_UNIT_UTF8, &session) != MARKDOWN_CORE_OK ||
        markdown_core_session_finalize(session, &document) != MARKDOWN_CORE_OK) {
        fprintf(stderr, "FAILED: %s: no session\n", name);
        failures++;
        markdown_core_session_free(session);
        return;
    }
    for (offset = 0; offset < length; ++offset) {
        markdown_core_position position = {line, (int32_t)(offset - start + 1)};
        const markdown_core_node **path = NULL;
        size_t count = 0, index;
        markdown_core_document_path(document, position, source, length, &path, &count);
        for (index = 0; index < count; ++index) {
            uint64_t id = markdown_core_node_id(path[index]);
            if (id >= limit) {
                size_t wider = (size_t)id * 2 + 1;
                uint8_t *more = (uint8_t *)realloc(seen, wider);
                if (more == NULL) {
                    check(0, "the seen table allocates");
                    break;
                }
                memset(more + limit, 0, wider - limit);
                seen = more;
                limit = wider;
            }
            if (!seen[id]) {
                seen[id] = 1;
                check_parts(session, source, length, path[index], position, name);
            }
        }
        markdown_core_path_free(path);
        if (source[offset] == '\n' ||
            (source[offset] == '\r' && !(offset + 1 < length && source[offset + 1] == '\n'))) {
            line++;
            start = offset + 1;
        }
    }
    if (length != 0) {
        markdown_core_position first = {1, 1};
        message = markdown_core_wire_markers(session, 0, first);
        check(message != NULL && message_status(message) == MARKDOWN_CORE_OUT_OF_BOUNDS,
              "an identifier the path lacks is OUT_OF_BOUNDS");
        markdown_core_wire_free(message);
    }
    free(seen);
    markdown_core_session_free(session);
}

/* A context crosses whole and spells after the session moves on: the open
 * cross link of `[[No` has one dest slot, which spells `x` with its closer
 * and refuses a `|`. */
static void check_context(void) {
    static const uint8_t expected[] = {
        1, 0, 0, 0, 3, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 4, 0, 0, 0, /* block Paragraph 1:1..1:4 */
    };
    markdown_core_session *session = NULL;
    markdown_core_context *context = NULL;
    markdown_core_position cursor = {1, 5}, outside = {3, 1};
    uint8_t *message =
        markdown_core_wire_session_new((const uint8_t *)"[[No", 4, MARKDOWN_CORE_TEXT_UNIT_UTF8, &session);
    markdown_core_wire_free(message);
    if (session == NULL) {
        check(0, "a session starts with a document");
        return;
    }
    message = markdown_core_wire_context(session, outside, &context);
    check(message != NULL && message_status(message) == MARKDOWN_CORE_OUT_OF_BOUNDS && context == NULL,
          "a cursor outside the text is OUT_OF_BOUNDS and makes no context");
    markdown_core_wire_free(message);
    message = markdown_core_wire_context(session, cursor, &context);
    check(message != NULL && message_status(message) == 0 && context != NULL, "a context answers");
    check(message != NULL && message_length(message) > 9 + sizeof(expected) &&
              memcmp(message + 9, expected, sizeof(expected)) == 0,
          "a context's first block is the open paragraph");
    markdown_core_wire_free(message);
    if (context == NULL) {
        markdown_core_session_free(session);
        return;
    }
    markdown_core_wire_free(markdown_core_wire_session_append(session, (const uint8_t *)"te]]\n", 5));
    markdown_core_session_free(session);
    message = markdown_core_wire_spell(context, 0, (const uint8_t *)"x", 1);
    check(message != NULL && message_status(message) == 0 && message_length(message) == 21 && message[9] == 1 &&
              read_u32(message + 10) == 3 && memcmp(message + 14, "x]]", 3) == 0 && read_u32(message + 17) == 1,
          "a slot spells its value and closer after the session is gone");
    markdown_core_wire_free(message);
    message = markdown_core_wire_spell(context, 0, (const uint8_t *)"a|b", 3);
    check(message != NULL && message_status(message) == 0 && message_length(message) == 10 && message[9] == 0,
          "a value the slot holds in no spelling answers none");
    markdown_core_wire_free(message);
    message = markdown_core_wire_spell(context, 1, (const uint8_t *)"x", 1);
    check(message != NULL && message_status(message) == MARKDOWN_CORE_OUT_OF_BOUNDS,
          "a slot the context lacks is OUT_OF_BOUNDS");
    markdown_core_wire_free(message);
    markdown_core_context_free(context);
}

/* The dialect's message lists the metadata field names, then the cues. */
static void check_dialect(void) {
    uint8_t *message = markdown_core_wire_dialect();
    markdown_core_string cues = markdown_core_slot_cues();
    size_t count = markdown_core_metadata_field_count(), index, at = 13;
    int same = message != NULL && message_status(message) == 0 && read_u32(message + 9) == count;
    for (index = 0; same && index < count; ++index) {
        markdown_core_string name;
        markdown_core_metadata_field_at(index, &name);
        same = read_u32(message + at) == name.length && memcmp(message + at + 4, name.data, name.length) == 0;
        at += 4 + name.length;
    }
    same = same && read_u32(message + at) == cues.length && memcmp(message + at + 4, cues.data, cues.length) == 0 &&
           message_length(message) == at + 4 + cues.length;
    check(same, "the dialect's message lists the metadata field names and the cues");
    markdown_core_wire_free(message);
}

int main(int argc, char **argv) {
    int i;
    if (argc < 4 || strcmp(argv[1], "--fixtures") != 0) {
        fputs("usage: wire_test --fixtures DIR NAME [NAME ...]\n", stderr);
        return 2;
    }
    check_shared_resource();
    check_session();
    check_session_shared_resource();
    check_context();
    check_dialect();
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
