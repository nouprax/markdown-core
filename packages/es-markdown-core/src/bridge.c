#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "markdown_core.h"

/* THE ENVELOPE, and nothing else: the document's bytes are the facade's
 * `markdown_core_document_wire` -- the canonical wire, written once beside the
 * canonical dump and decoded by every binding -- and what this bridge adds is
 * the four magic bytes and the status that say whether a tree or an error
 * follows. One buffer per read is the whole point: the wasm boundary used to
 * be crossed once per FIELD, which was thousands of calls for a document a
 * single decode pass reads in one. */

typedef struct es_buffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
    bool failed;
} es_buffer;

static void put_bytes(es_buffer *buffer, const uint8_t *bytes, size_t length) {
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

static void put_u8(es_buffer *buffer, uint8_t value) { put_bytes(buffer, &value, 1); }

static void put_i32(es_buffer *buffer, int32_t value) {
    uint32_t bits = (uint32_t)value;
    size_t index;
    for (index = 0; index < 4; ++index) {
        put_u8(buffer, (uint8_t)(bits >> (index * 8)));
    }
}

static void put_string(es_buffer *buffer, markdown_core_string value) {
    if (value.length > INT32_MAX) {
        buffer->failed = true;
        return;
    }
    put_i32(buffer, (int32_t)value.length);
    put_bytes(buffer, value.data, value.length);
}

static void es_apply_options(markdown_core_parse_options *options, uint32_t flags) {
    markdown_core_parse_options_init(options);
    options->smart_punctuation = (flags & (1u << 0)) != 0;
    options->footnotes = (flags & (1u << 1)) != 0;
    options->strip_html_comments = (flags & (1u << 2)) != 0;
    options->tables = (flags & (1u << 3)) != 0;
    options->strikethrough = (flags & (1u << 4)) != 0;
    options->autolinks = (flags & (1u << 5)) != 0;
    options->task_lists = (flags & (1u << 6)) != 0;
    options->formulas = (flags & (1u << 7)) != 0;
    options->directives = (flags & (1u << 8)) != 0;
}

static const uint8_t envelope_magic[] = {'M', 'K', 'C', '6'};

/* The five envelope bytes, written into room the buffer already holds --
 * `markdown_core_document_wire` reserved it in the payload's own allocation,
 * so a successful read costs no second buffer and no copy. */
static void stamp_envelope(uint8_t *buffer, uint8_t status) {
    memcpy(buffer, envelope_magic, sizeof(envelope_magic));
    buffer[sizeof(envelope_magic)] = status;
}

/* THE ONE PAYLOAD WRITER. A session's `feed`, its `advance` and its `finish`
 * all answer through here, so the wire has a single contract and a streamed
 * document crosses on exactly one path. Takes ownership of both `document`
 * and `error`; the caller keeps neither. `document == NULL` with no error is
 * an ADVANCE's healthy answer: the envelope alone, nothing behind it. */
static bool deliver(
    markdown_core_document *document,
    markdown_core_error *error,
    uintptr_t *output,
    size_t *output_length
) {
    es_buffer buffer = {0};

    if (document != NULL) {
        uint8_t *wire = NULL;
        size_t wire_length = 0;
        bool serialized = markdown_core_document_wire(document, sizeof(envelope_magic) + 1, &wire, &wire_length, NULL);
        markdown_core_error_free(error);
        markdown_core_document_free(document);
        if (!serialized) {
            return false;
        }
        stamp_envelope(wire, 0);
        *output = (uintptr_t)wire;
        *output_length = wire_length;
        return true;
    }

    put_bytes(&buffer, envelope_magic, sizeof(envelope_magic));
    if (error == NULL) {
        put_u8(&buffer, 0);
    } else {
        put_u8(&buffer, 1);
        put_i32(&buffer, markdown_core_error_get_code(error));
        put_string(&buffer, markdown_core_error_get_message(error));
    }
    markdown_core_error_free(error);

    if (buffer.failed) {
        free(buffer.data);
        return false;
    }
    *output = (uintptr_t)buffer.data;
    *output_length = buffer.size;
    return true;
}

/* THE STREAM (docs/STREAMING.md §4 D5) is the one entry this bridge has.
 * The one failure `markdown_core_session_new` can report is an allocation
 * failure, so NULL is the whole answer and the error it came with -- which
 * had to be allocated too -- is released here rather than crossing the wire. */
markdown_core_session *es_session_new(uint32_t flags) {
    markdown_core_parse_options options;
    markdown_core_error *error = NULL;
    markdown_core_session *session;
    es_apply_options(&options, flags);
    session = markdown_core_session_new(&options, &error);
    markdown_core_error_free(error);
    return session;
}

bool es_session_feed(
    markdown_core_session *session,
    const uint8_t *chunk,
    size_t length,
    uintptr_t *output,
    size_t *output_length
) {
    markdown_core_error *error = NULL;
    markdown_core_document *document;

    *output = 0;
    *output_length = 0;
    document = markdown_core_session_feed(session, chunk, length, &error);
    if (document == NULL && error == NULL) {
        return false;
    }
    return deliver(document, error, output, output_length);
}

bool es_session_finish(markdown_core_session *session, uintptr_t *output, size_t *output_length) {
    markdown_core_error *error = NULL;
    markdown_core_document *document;

    *output = 0;
    *output_length = 0;
    document = markdown_core_session_finish(session, &error);
    if (document == NULL && error == NULL) {
        return false;
    }
    return deliver(document, error, output, output_length);
}

bool es_session_advance(
    markdown_core_session *session,
    const uint8_t *chunk,
    size_t length,
    uintptr_t *output,
    size_t *output_length
) {
    markdown_core_error *error = NULL;

    *output = 0;
    *output_length = 0;
    /* The read this call would have produced is DISCARDED BY CONTRACT -- the
     * constructor's initial feed -- so nothing is projected and nothing is
     * serialized: the envelope alone answers, or the error rides in it. */
    markdown_core_session_advance(session, chunk, length, &error);
    return deliver(NULL, error, output, output_length);
}

void es_session_free(markdown_core_session *session) { markdown_core_session_free(session); }

void es_wire_free(uint8_t *output) { free(output); }
