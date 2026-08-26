#include "markdown_core_kotlin_bridge.h"

#include "markdown_core.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* THE ENVELOPE, and nothing else: the document's bytes are the facade's
 * `markdown_core_document_wire` -- the canonical wire, written once beside the
 * canonical dump and decoded by every binding -- and what this bridge adds is
 * the four magic bytes and the status that say whether a tree or an error
 * follows. The magic is versioned (`MKC6`) because THIS transport can skew: a
 * prebuilt native library can sit beside newer Kotlin code, and a payload the
 * decoder misreads must fail closed rather than decode as garbage. */

typedef struct bridge_buffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
    bool failed;
} bridge_buffer;

static void put_bytes(bridge_buffer *buffer, const uint8_t *bytes, size_t length) {
    if (buffer->failed || length > SIZE_MAX - buffer->size) {
        buffer->failed = true;
        return;
    }
    if (buffer->size + length > buffer->capacity) {
        size_t capacity = buffer->capacity == 0 ? 64 : buffer->capacity;
        uint8_t *data;
        while (capacity < buffer->size + length) {
            if (capacity > SIZE_MAX / 2) {
                capacity = buffer->size + length;
                break;
            }
            capacity *= 2;
        }
        data = (uint8_t *)realloc(buffer->data, capacity);
        if (data == NULL) {
            buffer->failed = true;
            return;
        }
        buffer->data = data;
        buffer->capacity = capacity;
    }
    if (length != 0) {
        memcpy(buffer->data + buffer->size, bytes, length);
        buffer->size += length;
    }
}

static void put_u8(bridge_buffer *buffer, uint8_t value) { put_bytes(buffer, &value, 1); }

static void put_i32(bridge_buffer *buffer, int32_t value) {
    uint32_t bits = (uint32_t)value;
    size_t index;
    for (index = 0; index < 4; ++index) {
        put_u8(buffer, (uint8_t)(bits >> (index * 8)));
    }
}

static void put_string(bridge_buffer *buffer, markdown_core_string value) {
    if (value.length > INT32_MAX) {
        buffer->failed = true;
        return;
    }
    put_i32(buffer, (int32_t)value.length);
    put_bytes(buffer, value.data, value.length);
}

static void apply_options(markdown_core_parse_options *options, uint32_t mask) {
    options->smart_punctuation = (mask & (1u << 0)) != 0;
    options->footnotes = (mask & (1u << 1)) != 0;
    options->strip_html_comments = (mask & (1u << 2)) != 0;
    options->tables = (mask & (1u << 3)) != 0;
    options->strikethrough = (mask & (1u << 4)) != 0;
    options->autolinks = (mask & (1u << 5)) != 0;
    options->task_lists = (mask & (1u << 6)) != 0;
    options->formulas = (mask & (1u << 7)) != 0;
    options->directives = (mask & (1u << 8)) != 0;
}

/* THE ONE PAYLOAD WRITER. A session's `feed` and its `finish` both answer
 * through here, so the wire has a single contract and a streamed document
 * crosses on exactly one path. Takes ownership of both `document` and
 * `error`; the caller keeps neither. */
static bool deliver(
    markdown_core_document *document,
    markdown_core_error *error,
    uint8_t **output,
    size_t *output_length
) {
    static const uint8_t magic[] = {'M', 'K', 'C', '6'};
    bridge_buffer buffer = {0};

    put_bytes(&buffer, magic, sizeof(magic));
    if (document == NULL) {
        put_u8(&buffer, 1);
        put_i32(&buffer, error == NULL ? MARKDOWN_CORE_ERROR_INTERNAL : markdown_core_error_get_code(error));
        if (error == NULL) {
            markdown_core_string fallback = {(const uint8_t *)"markdown parsing failed", 23};
            put_string(&buffer, fallback);
        } else {
            put_string(&buffer, markdown_core_error_get_message(error));
        }
    } else {
        uint8_t *wire = NULL;
        size_t wire_length = 0;
        put_u8(&buffer, 0);
        if (!markdown_core_document_wire(document, &wire, &wire_length, NULL)) {
            buffer.failed = true;
        } else {
            put_bytes(&buffer, wire, wire_length);
            markdown_core_wire_free(wire);
        }
    }
    markdown_core_error_free(error);
    markdown_core_document_free(document);

    if (buffer.failed) {
        free(buffer.data);
        return false;
    }
    *output = buffer.data;
    *output_length = buffer.size;
    return true;
}

/* The one failure `markdown_core_session_new` can report is an allocation
 * failure, so NULL is the whole answer and the error it came with -- which had
 * to be allocated too -- is released here rather than crossing the wire. */
markdown_core_kotlin_session *markdown_core_kotlin_session_new(uint32_t options_mask) {
    markdown_core_parse_options options;
    markdown_core_error *error = NULL;
    markdown_core_session *session;

    markdown_core_parse_options_init(&options);
    apply_options(&options, options_mask);
    session = markdown_core_session_new(&options, &error);
    markdown_core_error_free(error);
    return session;
}

bool markdown_core_kotlin_session_feed(
    markdown_core_kotlin_session *session,
    const uint8_t *chunk,
    size_t length,
    uint8_t **output,
    size_t *output_length
) {
    markdown_core_error *error = NULL;
    markdown_core_document *document;

    if (output == NULL || output_length == NULL) {
        return false;
    }
    *output = NULL;
    *output_length = 0;
    document = markdown_core_session_feed(session, chunk, length, &error);
    return deliver(document, error, output, output_length);
}

bool markdown_core_kotlin_session_finish(
    markdown_core_kotlin_session *session,
    uint8_t **output,
    size_t *output_length
) {
    markdown_core_error *error = NULL;
    markdown_core_document *document;

    if (output == NULL || output_length == NULL) {
        return false;
    }
    *output = NULL;
    *output_length = 0;
    document = markdown_core_session_finish(session, &error);
    return deliver(document, error, output, output_length);
}

void markdown_core_kotlin_session_free(markdown_core_kotlin_session *session) { markdown_core_session_free(session); }

void markdown_core_kotlin_free(uint8_t *output) { free(output); }
