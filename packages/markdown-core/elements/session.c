#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "../include/markdown_core.h"

#include "ast_internal.h"
#include "markdown-core-elements.h"

#include <parser.h>
#include <text_tree.h>

/* A SESSION: the text, the parser instance that reads it, the document
 * parsed from it, the storage its nodes live in, the last id it issued and
 * the document's node count. Each edit parses the whole text again as a
 * revision of the document (parser.h), so the new document continues the old
 * one and the old one's nodes go back to the session's pool. */
struct markdown_core_session {
    markdown_core_text_tree text;
    markdown_core_parser *parser;
    markdown_core_node_pool pool;
    markdown_core_document document;
    uint64_t last_id;
    size_t node_count;
};

/* The session's text as the parser reads it, a piece at a time. */
static const unsigned char *session_text_read(const markdown_core_text *text, size_t offset, size_t *start,
                                              size_t *end) {
    return markdown_core_text_tree_read(text->bytes, offset, start, end);
}

/* The one parse of a session's text. */
static markdown_core_status session_parse(markdown_core_session *session, const markdown_core_text *text,
                                          const markdown_core_byte_edit *edits, size_t count) {
    markdown_core_revision revision = {
        .pool = &session->pool,
        .previous = session->document.root,
        .edits = edits,
        .edit_count = count,
        .last_id = session->last_id,
        .node_count = session->node_count,
    };
    markdown_core_node *root = markdown_core_parser_parse(session->parser, text, &revision);
    if (!root) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    session->document.root = root;
    session->last_id = revision.last_id;
    session->node_count = revision.node_count;
    return MARKDOWN_CORE_OK;
}

/* Gives back what a session holds, but not the session itself. */
static void session_close(markdown_core_session *session) {
    if (session->document.root) {
        markdown_core_node_pool_release(&session->pool, session->document.root);
    }
    markdown_core_node_pool_dispose(&session->pool);
    markdown_core_text_tree_dispose(&session->text);
    markdown_core_parser_destroy(session->parser);
}

/* Opens a zeroed session in place: makes its parser, parses the source as
 * one piece, then takes it as the text. This is where a source enters the library, so
 * the capacity is checked here and at each edit, and nowhere below: offsets
 * are int32 and every buffer derived from the source stays under half of
 * that. */
static markdown_core_status session_open(markdown_core_session *session, const uint8_t *source, size_t size,
                                         markdown_core_text_unit unit) {
    if (size > MARKDOWN_CORE_SOURCE_CAPACITY) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    session->document.unit = unit;
    session->parser = markdown_core_core_parser(NULL, NULL);
    markdown_core_text text = markdown_core_text_buffer(source, size);
    if (!session->parser || session_parse(session, &text, NULL, 0) != MARKDOWN_CORE_OK ||
        !markdown_core_text_tree_init(&session->text, source, size)) {
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

/* An edit in bytes, with where the batch listed it. */
typedef struct {
    markdown_core_byte_edit edit;
    const uint8_t *text;
    size_t index;
} session_edit;

static int session_edit_compare(const void *left, const void *right) {
    const session_edit *a = left, *b = right;
    if (a->edit.start != b->edit.start) {
        return a->edit.start < b->edit.start ? -1 : 1;
    }
    if (a->edit.end != b->edit.end) {
        return a->edit.end < b->edit.end ? -1 : 1;
    }
    return (a->index > b->index) - (a->index < b->index);
}

/* The byte offset of `offset` in the session's unit: OUT_OF_BOUNDS past the
 * text, and INSIDE_SCALAR where no scalar begins. */
static markdown_core_status session_offset(const markdown_core_session *session, size_t offset, size_t *byte) {
    if (session->document.unit == MARKDOWN_CORE_TEXT_UNIT_UTF16) {
        if (offset > markdown_core_text_tree_units(&session->text)) {
            return MARKDOWN_CORE_OUT_OF_BOUNDS;
        }
        return markdown_core_text_tree_offset(&session->text, offset, byte) ? MARKDOWN_CORE_OK
                                                                            : MARKDOWN_CORE_INSIDE_SCALAR;
    }
    if (offset > markdown_core_text_tree_size(&session->text)) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    *byte = offset;
    return markdown_core_text_tree_boundary(&session->text, offset) ? MARKDOWN_CORE_OK : MARKDOWN_CORE_INSIDE_SCALAR;
}

markdown_core_status markdown_core_session_edit(markdown_core_session *session, const markdown_core_text_edit *edits,
                                                size_t count, const markdown_core_document **document) {
    size_t size = markdown_core_text_tree_size(&session->text);
    session_edit *sorted = markdown_core_alloc(count + 1, sizeof(*sorted));
    if (!sorted) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    markdown_core_status status = MARKDOWN_CORE_OK;
    for (size_t i = 0; i < count && status == MARKDOWN_CORE_OK; i++) {
        session_edit *edit = &sorted[i];
        if (edits[i].start > edits[i].end) {
            status = MARKDOWN_CORE_OUT_OF_BOUNDS;
        } else if ((status = session_offset(session, edits[i].start, &edit->edit.start)) == MARKDOWN_CORE_OK) {
            status = session_offset(session, edits[i].end, &edit->edit.end);
        }
        edit->edit.size = edits[i].size;
        edit->text = edits[i].text;
        edit->index = i;
    }
    if (status == MARKDOWN_CORE_OK) {
        qsort(sorted, count, sizeof(*sorted), session_edit_compare);
    }
    /* The batch in source order, in bytes: the edits the text applies and
     * the revision describes, and the bytes each writes. */
    markdown_core_byte_edit *revision = NULL;
    const uint8_t **texts = NULL;
    if (status == MARKDOWN_CORE_OK) {
        revision = markdown_core_alloc(count + 1, sizeof(*revision));
        texts = markdown_core_alloc(count + 1, sizeof(*texts));
        if (!revision || !texts) {
            status = MARKDOWN_CORE_ALLOCATION_FAILED;
        }
    }
    /* The size after the batch: the text less every replaced range, which
     * disjoint ranges keep within the text, and then each edit's bytes,
     * checked against the capacity as they are added. */
    size_t after = size;
    for (size_t i = 0; i < count && status == MARKDOWN_CORE_OK; i++) {
        const markdown_core_byte_edit *edit = &sorted[i].edit;
        if (i + 1 < count && edit->end > sorted[i + 1].edit.start) {
            status = MARKDOWN_CORE_OUT_OF_BOUNDS;
        } else {
            after -= edit->end - edit->start;
            revision[i] = *edit;
            texts[i] = sorted[i].text;
        }
    }
    for (size_t i = 0; i < count && status == MARKDOWN_CORE_OK; i++) {
        if (revision[i].size > MARKDOWN_CORE_SOURCE_CAPACITY - after) {
            status = MARKDOWN_CORE_ALLOCATION_FAILED;
        } else {
            after += revision[i].size;
        }
    }
    if (status == MARKDOWN_CORE_OK && !markdown_core_text_tree_replace(&session->text, revision, texts, count)) {
        status = MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    if (status == MARKDOWN_CORE_OK) {
        markdown_core_text text = {session_text_read, &session->text, after};
        status = session_parse(session, &text, revision, count);
    }
    markdown_core_free(texts);
    markdown_core_free(revision);
    markdown_core_free(sorted);
    if (status == MARKDOWN_CORE_OK) {
        *document = &session->document;
    }
    return status;
}

markdown_core_status markdown_core_session_append(markdown_core_session *session, const uint8_t *text, size_t size,
                                                  const markdown_core_document **document) {
    size_t end = session->document.unit == MARKDOWN_CORE_TEXT_UNIT_UTF16 ? markdown_core_text_tree_units(&session->text)
                                                                         : markdown_core_text_tree_size(&session->text);
    markdown_core_text_edit edit = {end, end, text, size};
    return markdown_core_session_edit(session, &edit, 1, document);
}

const markdown_core_document *markdown_core_session_document(const markdown_core_session *session) {
    return &session->document;
}

markdown_core_text_unit markdown_core_session_unit(const markdown_core_session *session) {
    return session->document.unit;
}

size_t markdown_core_session_text_size(const markdown_core_session *session) {
    return markdown_core_text_tree_size(&session->text);
}

void markdown_core_session_text(const markdown_core_session *session, uint8_t *bytes) {
    markdown_core_text_tree_copy(&session->text, bytes);
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
