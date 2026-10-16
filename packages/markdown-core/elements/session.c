#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "../include/markdown_core.h"

#include "ast_internal.h"
#include "markdown-core-elements.h"

#include <parser.h>

/* A SESSION: the text, the parse that has committed its lines, the document
 * published from it, the storage its nodes live in and whether it was
 * finalized. The text is one buffer of `capacity` bytes, `size` of them
 * written, and only grows at its end.
 *
 * THE COMMITTED PARSE is one transaction of `committed` across the
 * session's appends (plan 5.3): it has read the lines before `commit_point`,
 * each as a parse of any longer text reads it. An append commits the lines
 * up to the commit point the last snapshot left and then takes a snapshot of
 * the whole text, which leaves the next commit point; the finalize commits
 * every line that is left and ends the committed parse at the end of the
 * text. A snapshot reads the text whole, as a transaction of `snapshots`.
 * Each new document continues the old one (parser.h, `revision`): the
 * revision lends the session's pool, names the document published last and
 * counts the ids it issued. */
struct markdown_core_session {
    uint8_t *text;
    size_t size, capacity;
    markdown_core_parser *committed, *snapshots;
    size_t commit_point;
    markdown_core_node_pool pool;
    markdown_core_revision revision;
    markdown_core_document document;
    bool finalized;
};

/* The text a parser reads: an empty text is the empty buffer. */
static const char *session_source(const markdown_core_session *session) {
    static const uint8_t empty[1] = {0};
    return (const char *)(session->size ? session->text : empty);
}

/* Publishes `root`, the document a transaction returned, or NULL when the
 * transaction failed. */
static markdown_core_status session_publish(markdown_core_session *session, markdown_core_node *root) {
    if (!root) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    session->document.root = session->revision.previous = root;
    return MARKDOWN_CORE_OK;
}

/* Commits the lines before the commit point, then publishes a snapshot of
 * the whole text, which leaves the next commit point. */
static markdown_core_status session_snapshot(markdown_core_session *session) {
    if (!markdown_core_parser_commit(session->committed, session_source(session), session->size,
                                     session->commit_point)) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    /* The lines committed read nothing of the end, as they did in the
     * snapshot that left the commit point. */
    assert(markdown_core_parser_commit_point(session->committed) >= session->commit_point);
    if (!session->snapshots && !(session->snapshots = markdown_core_core_parser(NULL, NULL))) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    markdown_core_parser_start(session->snapshots, &session->revision);
    markdown_core_parser_commit(session->snapshots, session_source(session), session->size, session->size);
    size_t commit_point = markdown_core_parser_commit_point(session->snapshots);
    markdown_core_status status = session_publish(session, markdown_core_parser_snapshot(session->snapshots));
    if (status == MARKDOWN_CORE_OK) {
        session->commit_point = commit_point;
    }
    return status;
}

/* Commits every line of the text and publishes the committed parse ended
 * there, which ends the session's appends. */
static markdown_core_status session_finalize(markdown_core_session *session) {
    markdown_core_parser_commit(session->committed, session_source(session), session->size, session->size);
    session->finalized = true;
    return session_publish(session, markdown_core_parser_finalize(session->committed));
}

/* Adds `size` bytes to the end of the text. This is where text enters the
 * library, so the capacity is checked here and nowhere below: offsets are
 * int32 and every buffer derived from the text stays under half of that.
 * The buffer at least doubles when it grows, so a stream of appends copies
 * each byte O(1) times. */
static markdown_core_status session_take(markdown_core_session *session, const uint8_t *text, size_t size) {
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
    return MARKDOWN_CORE_OK;
}

/* Gives back what a session holds, but not the session itself. The
 * committed parse of a session that was not finalized ends with nothing. */
static void session_close(markdown_core_session *session) {
    if (session->committed && !session->finalized) {
        markdown_core_parser_discard(session->committed);
    }
    if (session->document.root) {
        markdown_core_node_pool_release(&session->pool, session->document.root);
    }
    markdown_core_node_pool_dispose(&session->pool);
    markdown_core_free(session->text);
    if (session->committed) {
        markdown_core_parser_destroy(session->committed);
    }
    if (session->snapshots) {
        markdown_core_parser_destroy(session->snapshots);
    }
}

/* Opens a zeroed session in place: makes its parser, starts its committed
 * parse and takes the source. The parser for snapshots is made with the
 * first. */
static markdown_core_status session_open(markdown_core_session *session, const uint8_t *source, size_t size,
                                         markdown_core_text_unit unit) {
    session->document.unit = unit;
    session->revision.pool = &session->pool;
    session->committed = markdown_core_core_parser(NULL, NULL);
    if (!session->committed) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    markdown_core_parser_start(session->committed, &session->revision);
    return session_take(session, source, size);
}

markdown_core_status markdown_core_session_new(const uint8_t *source, size_t size, markdown_core_text_unit unit,
                                               markdown_core_session **session) {
    markdown_core_session *made = markdown_core_alloc(1, sizeof(*made));
    if (!made) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    markdown_core_status status = session_open(made, source, size, unit);
    if (status == MARKDOWN_CORE_OK) {
        status = session_snapshot(made);
    }
    if (status != MARKDOWN_CORE_OK) {
        session_close(made);
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
    markdown_core_status status = session_take(session, text, size);
    if (status == MARKDOWN_CORE_OK) {
        status = session_snapshot(session);
    }
    if (status == MARKDOWN_CORE_OK) {
        *document = &session->document;
    }
    return status;
}

markdown_core_status markdown_core_session_finalize(markdown_core_session *session,
                                                    const markdown_core_document **document) {
    if (session->finalized) {
        return MARKDOWN_CORE_SESSION_FINALIZED;
    }
    markdown_core_status status = session_finalize(session);
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

/* A fresh parse is a session that reads its source once, is finalized and
 * is then discarded: the document leaves the session with its nodes, which
 * hold their slabs. */
markdown_core_status markdown_core_document_parse_in(const uint8_t *source, size_t length, markdown_core_text_unit unit,
                                                     markdown_core_document **document) {
    markdown_core_document *parsed = markdown_core_alloc(1, sizeof(*parsed));
    if (!parsed) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    markdown_core_session session = {0};
    markdown_core_status status = session_open(&session, source, length, unit);
    if (status == MARKDOWN_CORE_OK) {
        status = session_finalize(&session);
    }
    if (status != MARKDOWN_CORE_OK) {
        session_close(&session);
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
