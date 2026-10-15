#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "../include/markdown_core.h"

#include "ast_internal.h"
#include "markdown-core-elements.h"

#include <parser.h>

/* A SESSION: the text, the parser instance that reads it, the document
 * parsed from it, the storage its nodes live in, the last id it issued and
 * the document's node count. Each append adds to the end of the text and
 * parses the whole text again as a revision of the document (parser.h), so
 * the new document continues the old one and the old one's nodes go back to
 * the session's pool. The text is one buffer of `capacity` bytes, `size` of
 * them written. */
struct markdown_core_session {
    uint8_t *text;
    size_t size, capacity;
    markdown_core_parser *parser;
    markdown_core_node_pool pool;
    markdown_core_document document;
    uint64_t last_id;
    size_t node_count;
};

/* The one parse of a session: it reads `text`, the session's whole text. An
 * empty text is the empty buffer. */
static markdown_core_status session_parse(markdown_core_session *session, const uint8_t *text, size_t size) {
    static const uint8_t empty[1] = {0};
    markdown_core_revision revision = {
        .pool = &session->pool,
        .previous = session->document.root,
        .last_id = session->last_id,
        .node_count = session->node_count,
    };
    markdown_core_node *root =
        markdown_core_parser_parse(session->parser, (const char *)(size ? text : empty), size, &revision);
    if (!root) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    session->document.root = root;
    session->last_id = revision.last_id;
    session->node_count = revision.node_count;
    return MARKDOWN_CORE_OK;
}

/* Whether the text stays within the capacity with `size` more bytes. This is
 * where text enters the library, so the capacity is checked here and nowhere
 * below: offsets are int32 and every buffer derived from the text stays under
 * half of that. */
static bool session_fits(const markdown_core_session *session, size_t size) {
    return size <= MARKDOWN_CORE_SOURCE_CAPACITY - session->size;
}

/* Adds `size` bytes to the end of the text the session keeps. The buffer at
 * least doubles when it grows, so a stream of appends copies each byte O(1)
 * times. */
static bool session_keep(markdown_core_session *session, const uint8_t *text, size_t size) {
    const size_t needed = session->size + size;
    if (needed > session->capacity) {
        size_t grown = session->capacity * 2;
        grown = grown < needed ? needed : grown > MARKDOWN_CORE_SOURCE_CAPACITY ? MARKDOWN_CORE_SOURCE_CAPACITY : grown;
        uint8_t *resized = markdown_core_realloc(session->text, grown);
        if (!resized) {
            return false;
        }
        session->text = resized;
        session->capacity = grown;
    }
    if (size) {
        memcpy(session->text + session->size, text, size);
    }
    session->size = needed;
    return true;
}

/* Gives back what a session holds, but not the session itself. */
static void session_close(markdown_core_session *session) {
    if (session->document.root) {
        markdown_core_node_pool_release(&session->pool, session->document.root);
    }
    markdown_core_node_pool_dispose(&session->pool);
    markdown_core_free(session->text);
    markdown_core_parser_destroy(session->parser);
}

/* Opens a zeroed session in place: makes its parser, parses the source, which
 * is the whole text of a session that has none yet, then keeps it for the
 * appends that follow. Nothing is copied before the parse needs it: only an
 * append, whose whole text is the kept text followed by its bytes, keeps
 * before it parses. */
static markdown_core_status session_open(markdown_core_session *session, const uint8_t *source, size_t size,
                                         markdown_core_text_unit unit) {
    if (!session_fits(session, size)) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    session->document.unit = unit;
    session->parser = markdown_core_core_parser(NULL, NULL);
    if (!session->parser || session_parse(session, source, size) != MARKDOWN_CORE_OK ||
        !session_keep(session, source, size)) {
        session_close(session);
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_session_new(const uint8_t *source, size_t size, markdown_core_text_unit unit,
                                               markdown_core_session **session) {
    markdown_core_session *made = markdown_core_alloc(1, sizeof(*made));
    if (!made) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    markdown_core_status status = session_open(made, source, size, unit);
    if (status != MARKDOWN_CORE_OK) {
        markdown_core_free(made);
        return status;
    }
    *session = made;
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_session_append(markdown_core_session *session, const uint8_t *text, size_t size,
                                                  const markdown_core_document **document) {
    if (!session_fits(session, size) || !session_keep(session, text, size)) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    markdown_core_status status = session_parse(session, session->text, session->size);
    if (status == MARKDOWN_CORE_OK) {
        *document = &session->document;
    }
    return status;
}

const markdown_core_document *markdown_core_session_document(const markdown_core_session *session) {
    return &session->document;
}

markdown_core_text_unit markdown_core_session_unit(const markdown_core_session *session) {
    return session->document.unit;
}

size_t markdown_core_session_text_size(const markdown_core_session *session) { return session->size; }

void markdown_core_session_text(const markdown_core_session *session, uint8_t *bytes) {
    if (session->size) {
        memcpy(bytes, session->text, session->size);
    }
}

void markdown_core_session_free(markdown_core_session *session) {
    if (!session) {
        return;
    }
    session_close(session);
    markdown_core_free(session);
}

/* A fresh parse is a session that reads its source once and is then
 * discarded: the document leaves the session with its nodes, which hold
 * their slabs. */
markdown_core_status markdown_core_document_parse_in(const uint8_t *source, size_t length, markdown_core_text_unit unit,
                                                     markdown_core_document **document) {
    markdown_core_document *parsed = markdown_core_alloc(1, sizeof(*parsed));
    if (!parsed) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    markdown_core_session session = {0};
    markdown_core_status status = session_open(&session, source, length, unit);
    if (status != MARKDOWN_CORE_OK) {
        markdown_core_free(parsed);
        return status;
    }
    *parsed = session.document;
    session.document.root = NULL;
    session_close(&session);
    *document = parsed;
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_document_parse(const uint8_t *source, size_t length,
                                                  markdown_core_document **document) {
    return markdown_core_document_parse_in(source, length, MARKDOWN_CORE_TEXT_UNIT_UTF8, document);
}
