#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "../include/markdown_core.h"

#include "ast_internal.h"
#include "markdown-core-elements.h"

#include <parser.h>

/* A SESSION: the text, the parser instance that reads it, the document
 * parsed from it, the storage its nodes live in, the last id it issued, the
 * document's node count and whether it was finalized. Each append adds to the end of the text and
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
    bool finalized;
};

/* The one parse of a session's text: a start, a commit of the whole text
 * and a finalize. An empty text is the empty buffer. */
static markdown_core_status session_parse(markdown_core_session *session) {
    static const uint8_t empty[1] = {0};
    markdown_core_revision revision = {
        .pool = &session->pool,
        .previous = session->document.root,
        .last_id = session->last_id,
        .node_count = session->node_count,
    };
    markdown_core_parser_start(session->parser, &revision);
    markdown_core_parser_commit(session->parser, (const char *)(session->size ? session->text : empty), session->size);
    markdown_core_node *root = markdown_core_parser_finalize(session->parser);
    if (!root) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    session->document.root = root;
    session->last_id = revision.last_id;
    session->node_count = revision.node_count;
    return MARKDOWN_CORE_OK;
}

/* Adds `size` bytes to the end of the text and parses it. This is where text
 * enters the library, so the capacity is checked here and nowhere below:
 * offsets are int32 and every buffer derived from the text stays under half
 * of that. The buffer at least doubles when it grows, so a stream of appends
 * copies each byte O(1) times. */
static markdown_core_status session_append(markdown_core_session *session, const uint8_t *text, size_t size) {
    if (size > MARKDOWN_CORE_SOURCE_CAPACITY - session->size) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    const size_t needed = session->size + size;
    if (needed > session->capacity) {
        size_t grown = session->capacity * 2;
        grown = grown < needed ? needed : grown > MARKDOWN_CORE_SOURCE_CAPACITY ? MARKDOWN_CORE_SOURCE_CAPACITY : grown;
        uint8_t *resized = markdown_core_realloc(session->text, grown);
        if (!resized) {
            return MARKDOWN_CORE_ALLOCATION_FAILED;
        }
        session->text = resized;
        session->capacity = grown;
    }
    if (size) {
        memcpy(session->text + session->size, text, size);
    }
    session->size = needed;
    return session_parse(session);
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

/* Opens a zeroed session in place: makes its parser and appends the source,
 * which an empty session reads as a fresh parse. */
static markdown_core_status session_open(markdown_core_session *session, const uint8_t *source, size_t size,
                                         markdown_core_text_unit unit) {
    session->document.unit = unit;
    session->parser = markdown_core_core_parser(NULL, NULL);
    markdown_core_status status =
        session->parser ? session_append(session, source, size) : MARKDOWN_CORE_ALLOCATION_FAILED;
    if (status != MARKDOWN_CORE_OK) {
        session_close(session);
    }
    return status;
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
    if (session->finalized) {
        return MARKDOWN_CORE_SESSION_FINALIZED;
    }
    markdown_core_status status = session_append(session, text, size);
    if (status == MARKDOWN_CORE_OK) {
        *document = &session->document;
    }
    return status;
}

/* Every parse of a session reads the whole text, so its document is already
 * the final one. */
markdown_core_status markdown_core_session_finalize(markdown_core_session *session,
                                                    const markdown_core_document **document) {
    if (session->finalized) {
        return MARKDOWN_CORE_SESSION_FINALIZED;
    }
    session->finalized = true;
    *document = &session->document;
    return MARKDOWN_CORE_OK;
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
