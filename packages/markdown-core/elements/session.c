#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "array.h"
#include "../include/markdown_core.h"

#include "ast_internal.h"
#include "sink.h"
#include "markdown-core-elements.h"

#include <parser.h>

/* A SESSION: the text, the parse that has committed its lines, the document
 * published from it, the storage its nodes live in and whether it was
 * flushed. The text is an array of bytes (array.h), which only grows at
 * its end.
 *
 * THE COMMITTED PARSE is one transaction of `committed` across the
 * session's appends (plan 5.3): it has read the lines the text has finished
 * and the steps of its last line that the line's bytes decide, each as a
 * parse of any longer text reads it, and completed the blocks that settled
 * in them (plan 5.4). An append commits what the new bytes finish, completes
 * what settled, and then takes a snapshot of the whole text on the committed
 * parse (blocks.c, THE SNAPSHOT); the flush commits the rest of the text
 * and ends the committed parse at its end.
 * Each new document continues the old one (parser.h, `revision`): the
 * revision lends the session's pool and counts the ids it issued. The
 * document is the committed parse's tree, published in place (ast.c,
 * PUBLISHING IN PLACE), and the session lends it until the next append or
 * the flush; the flush returns the last document.
 *
 * THE CONTEXT is the session's state at its cursor: the context query at the
 * position it was last located at, which every read of the context and every
 * spelling reads in place (context.c, THE SESSION'S CONTEXT). The session
 * lends it for its whole life. It is empty until the first locate, and an
 * append or the flush, which change the document, empty it again. A fresh
 * parse's session has none. */
/* THE WIDTHS: where the root's children are, from their extents alone
 * (docs/plans/relative-positions.md). The children are counted in chunks of
 * SESSION_CHUNK, and each chunk keeps the bytes from where the child before
 * it ends to where its last ends: a child's start is the chunks before its
 * own and the children before it in its own, and an edit, which moves no
 * child it takes back, makes again only the chunks of the children it read
 * and of the first it took back -- all from there on when the count of
 * children changed. They are made at the first edit, and an edit that could
 * not have their storage leaves them to the next to make again. */
#define SESSION_CHUNK 64

typedef struct {
    uint32_t *chunks;
    size_t count, capacity;
    bool made;
} session_widths;

struct markdown_core_session {
    MARKDOWN_CORE_ARRAY(uint8_t) text;
    markdown_core_parser *committed;
    markdown_core_node_pool pool;
    markdown_core_revision revision;
    markdown_core_document document;
    markdown_core_context *context;
    bool located;
    bool flushed;
    /* The bytes of the text the last edit read again. */
    size_t reread_from, reread_to;
    session_widths widths;
};

/* The bytes from where the child before `child` ends to where it ends. */
static inline uint32_t child_width(const markdown_core_node *child) {
    return (uint32_t)((int64_t)child->extent.lead + child->extent.span);
}

/* Makes the chunks of the root's children from `first` to `last` again,
 * and all of them from `first` on when `last` is past the root's children.
 * False, with the widths to be made again, when storage could not be had. */
static bool widths_mend(session_widths *widths, const markdown_core_node *root, size_t first, size_t last) {
    const size_t count = (root->child_count + SESSION_CHUNK - 1) / SESSION_CHUNK;
    if (last >= root->child_count) {
        uint32_t *chunks = markdown_core_reserve(widths->chunks, &widths->capacity, count, sizeof(*chunks));
        if (count && !chunks) {
            widths->made = false;
            return false;
        }
        widths->chunks = chunks;
        widths->count = count;
        last = root->child_count ? root->child_count - 1 : 0;
    }
    for (size_t chunk = first / SESSION_CHUNK; chunk < count && chunk <= last / SESSION_CHUNK; chunk++) {
        const size_t end =
            (chunk + 1) * SESSION_CHUNK < root->child_count ? (chunk + 1) * SESSION_CHUNK : root->child_count;
        uint32_t width = 0;
        for (size_t i = chunk * SESSION_CHUNK; i < end; i++) {
            width += child_width(root->children[i]);
        }
        widths->chunks[chunk] = width;
    }
    return true;
}

/* Makes the widths when they are not made. */
static bool widths_make(session_widths *widths, const markdown_core_node *root) {
    widths->made = widths->made || widths_mend(widths, root, 0, SIZE_MAX);
    return widths->made;
}

/* Where the root's child `child` starts. */
static size_t widths_start(const session_widths *widths, const markdown_core_node *root, size_t child) {
    int64_t at = root->extent.lead;
    for (size_t chunk = 0; chunk < child / SESSION_CHUNK; chunk++) {
        at += widths->chunks[chunk];
    }
    for (size_t i = child / SESSION_CHUNK * SESSION_CHUNK; i < child; i++) {
        at += child_width(root->children[i]);
    }
    return (size_t)(at + root->children[child]->extent.lead);
}

/* Where the root's child `child` ends. */
static size_t widths_end(const session_widths *widths, const markdown_core_node *root, size_t child) {
    return widths_start(widths, root, child) + root->children[child]->extent.span;
}

/* The child of `root` before the first that ends past `offset`, or its
 * first. */
static uint32_t widths_child(const session_widths *widths, const markdown_core_node *root, size_t offset) {
    int64_t at = root->extent.lead;
    size_t chunk = 0;
    while (chunk < widths->count && at + widths->chunks[chunk] <= (int64_t)offset) {
        at += widths->chunks[chunk++];
    }
    uint32_t child = chunk < widths->count ? (uint32_t)(chunk * SESSION_CHUNK) : root->child_count;
    while (child < root->child_count && at + child_width(root->children[child]) <= (int64_t)offset) {
        at += child_width(root->children[child++]);
    }
    return child ? child - 1 : 0;
}

#ifndef NDEBUG
/* The widths an edit mended are the widths of its root made anew, and
 * each child stands where the walk finds it. */
static void widths_check(const session_widths *widths, const markdown_core_node *root) {
    session_widths fresh = {0};
    if (!widths_mend(&fresh, root, 0, SIZE_MAX)) {
        return;
    }
    assert(fresh.count == widths->count);
    for (size_t i = 0; i < fresh.count; i++) {
        assert(fresh.chunks[i] == widths->chunks[i]);
    }
    markdown_core_free(fresh.chunks);
    /* The walk measures each child from the one before. */
    markdown_core_walk walk;
    markdown_core_walk_item item;
    markdown_core_walk_begin(&walk, root);
    size_t child = 0;
    while (markdown_core_walk_next(&walk, &item)) {
        if (item.node && item.level == 1 && child < root->child_count && item.node == root->children[child]) {
            assert(widths_start(widths, root, child) == item.place.start);
            assert(widths_end(widths, root, child) == item.place.end);
            child++;
        }
    }
    assert(walk.failed || child == root->child_count);
    markdown_core_walk_end(&walk);
}
#endif

/* The text a parser reads: an empty text is the empty buffer. */
static const char *session_source(const markdown_core_session *session) {
    static const uint8_t empty[1] = {0};
    return (const char *)(session->text.count ? session->text.values : empty);
}

/* Publishes `root`, the document a transaction returned, or NULL when the
 * transaction failed. */
static markdown_core_status session_publish(markdown_core_session *session, markdown_core_node *root) {
    if (!root) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    session->document.root = root;
    return MARKDOWN_CORE_OK;
}

/* Commits the text and completes the blocks that settled in it, then
 * publishes a snapshot of the whole text. */
static markdown_core_status session_snapshot(markdown_core_session *session) {
    if (!markdown_core_parser_commit(session->committed, session_source(session), session->text.count, false) ||
        !markdown_core_parser_complete(session->committed)) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    return session_publish(
        session, markdown_core_parser_snapshot(session->committed, session_source(session), session->text.count));
}

/* Commits every line of the text and publishes the committed parse ended
 * there, which ends the session's appends. The parse keeps its state, which
 * the edits read and the close releases. A commit that fails fails the
 * parse, which the flush then publishes as nothing. */
static markdown_core_status session_flush(markdown_core_session *session) {
    markdown_core_parser_commit(session->committed, session_source(session), session->text.count, true);
    session->flushed = true;
    markdown_core_node *root = markdown_core_parser_flush(session->committed);
    session->document.root = root;
    return root ? MARKDOWN_CORE_OK : MARKDOWN_CORE_ALLOCATION_FAILED;
}

/* Adds `size` bytes to the end of the text. This is where text enters the
 * library, so the capacity is checked here and nowhere below: offsets are
 * int32 and every buffer derived from the text stays under half of that.
 * The buffer moves ahead of its growth, so what an append copies besides
 * its own bytes is at most a few times their number. */
static markdown_core_status session_take(markdown_core_session *session, const uint8_t *text, size_t size) {
    if (size > MARKDOWN_CORE_SOURCE_CAPACITY - session->text.count) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    if (size && !markdown_core_array_append(&session->text, text, size)) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    return MARKDOWN_CORE_OK;
}

/* Gives back what a session holds, but not the session itself. The
 * committed parse ends with nothing and releases what it keeps: before a
 * flush, that is the document it published last; after one, its state,
 * which the flush kept for the edits. The document a flush returned is the
 * session's. Both go back to the pool before the pool goes. */
static void session_close(markdown_core_session *session) {
    if (session->committed) {
        markdown_core_parser_discard(session->committed);
        markdown_core_parser_destroy(session->committed);
    }
    if (session->flushed && session->document.root) {
        markdown_core_node_pool_release(&session->pool, session->document.root);
    }
    markdown_core_node_pool_dispose(&session->pool);
    markdown_core_array_free(&session->text);
    markdown_core_free(session->widths.chunks);
    markdown_core_context_free(session->context);
}

/* Opens a zeroed session in place: makes its parser, takes the source and,
 * once it holds it, starts its committed parse. */
static markdown_core_status session_open(markdown_core_session *session, const uint8_t *source, size_t size,
                                         markdown_core_text_unit unit) {
    session->document.unit = unit;
    session->revision.pool = &session->pool;
    session->committed = markdown_core_core_parser(NULL, NULL);
    if (!session->committed) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    markdown_core_status status = session_take(session, source, size);
    if (status == MARKDOWN_CORE_OK) {
        markdown_core_parser_start(session->committed, &session->revision);
    }
    return status;
}

/* Empties the context a locate left, so a step that never follows one
 * costs no more than it did. */
static void session_forget(markdown_core_session *session) {
    if (session->located) {
        markdown_core_context_clear(session->context);
        session->located = false;
    }
}

markdown_core_status markdown_core_session_new(const uint8_t *source, size_t size, markdown_core_text_unit unit,
                                               markdown_core_session **session) {
    markdown_core_session *made = markdown_core_alloc(1, sizeof(*made));
    if (!made) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    made->context = markdown_core_context_new();
    markdown_core_status status =
        made->context ? session_open(made, source, size, unit) : MARKDOWN_CORE_ALLOCATION_FAILED;
    if (status == MARKDOWN_CORE_OK) {
        /* Only a session takes edits; a one-off parse keeps no reach. */
        status = markdown_core_parser_edits(made->committed) ? session_snapshot(made) : MARKDOWN_CORE_ALLOCATION_FAILED;
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
    if (session->flushed) {
        return MARKDOWN_CORE_SESSION_FLUSHED;
    }
    session_forget(session);
    markdown_core_status status = session_take(session, text, size);
    if (status == MARKDOWN_CORE_OK) {
        status = session_snapshot(session);
    }
    if (status == MARKDOWN_CORE_OK) {
        *document = &session->document;
    }
    return status;
}

markdown_core_status markdown_core_session_flush(markdown_core_session *session,
                                                 const markdown_core_document **document) {
    if (session->flushed) {
        return MARKDOWN_CORE_SESSION_FLUSHED;
    }
    session_forget(session);
    markdown_core_status status = session_flush(session);
    if (status == MARKDOWN_CORE_OK) {
        *document = &session->document;
    }
    return status;
}

/* THE EDIT (docs/plans/2026-09-29-incremental-parsing.md, 12.3). */

/* The byte a cursor at `position` of the flushed text stands before, in
 * `*offset`: false when it stands at none. */
static bool session_offset(const markdown_core_session *session, markdown_core_position position, size_t *offset) {
    size_t start, end;
    return position.line >= 1 && position.column >= 1 &&
           markdown_core_parser_line_span(session->committed, (size_t)position.line - 1, &start, &end) &&
           markdown_core_column_offset((const uint8_t *)session_source(session), start, end, position.column,
                                       session->document.unit, true, offset);
}

/* The rest point the edit reads from, before the root's child `*child`, at
 * the start of input line `*line`: the start of the child before the first
 * that ends past the start of `line`, where the edit begins -- every rule of
 * the grammar that writes backwards reaches only the block before -- or the
 * document's start. A child that starts on a line the child before it ends
 * on has no rest point of its own, nor one whose start a reading from
 * before it read past (blocks.c, THE REACH): the rest point moves back to
 * the child before the one that reading began in, as that child may read
 * otherwise. */
static void session_rest(const markdown_core_session *session, size_t edited, uint32_t *child, size_t *line) {
    const markdown_core_node *root = session->document.root;
    const session_widths *widths = &session->widths;
    size_t start, end;
    markdown_core_parser_sort_reach(session->committed);
    markdown_core_parser_line_span(session->committed, edited, &start, &end);
    uint32_t at = widths_child(widths, root, start);
    size_t from = 0;
    while (at) {
        from = markdown_core_parser_line_of(session->committed, widths_start(widths, root, at));
        markdown_core_parser_line_span(session->committed, from, &start, &end);
        size_t reached;
        if (markdown_core_parser_spanned(session->committed, start, &reached)) {
            at = widths_child(widths, root, reached);
        } else if (widths_end(widths, root, at - 1) > start) {
            at--;
        } else {
            break;
        }
        from = 0;
    }
    *child = at;
    *line = from;
}

/* Replaces the bytes from `from` to `to` of the text with `size` bytes of
 * `text`. False, with the text as it was, when storage could not be had. */
static bool session_replace(markdown_core_session *session, size_t from, size_t to, const uint8_t *text, size_t size) {
    const size_t rest = session->text.count - to;
    uint8_t *suffix = rest ? markdown_core_alloc(rest, 1) : NULL;
    if (rest && !suffix) {
        return false;
    }
    if (rest) {
        memcpy(suffix, session->text.values + to, rest);
    }
    const size_t total = from + size + rest;
    bool ok = markdown_core_array_reserve(&session->text, total);
    if (ok) {
        markdown_core_array_truncate(&session->text, from);
        ok = (!size || markdown_core_array_append(&session->text, text, size)) &&
             (!rest || markdown_core_array_append(&session->text, suffix, rest));
        assert(ok);
    }
    markdown_core_free(suffix);
    return ok;
}

/* The flushed parse put back before the root's child `child`, at input line
 * `line`, reads the text, in which an edit replaced the old text from `from`
 * to `to` and moved what follows by `delta`, and is flushed again: to its
 * end, or to where it takes the old children back, unless `whole` says to
 * read all of it. */
static markdown_core_status session_reread(markdown_core_session *session, uint32_t child, size_t line, size_t from,
                                           size_t to, int64_t delta, bool whole) {
    markdown_core_node *root = session->document.root;
    const size_t count = root->child_count;
    session->document.root = NULL;
    size_t start, end;
    markdown_core_parser_line_span(session->committed, line, &start, &end);
    const uint32_t prior =
        child ? (uint32_t)widths_end(&session->widths, root, child - 1) : (uint32_t)root->extent.lead;
    if (!markdown_core_parser_reopen(session->committed, root, child, line, prior, from, to, delta,
                                     session_source(session), whole)) {
        markdown_core_node_pool_release(&session->pool, root);
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    const markdown_core_status status = session_flush(session);
    const size_t resumed = markdown_core_parser_resumed(session->committed);
    session->reread_from = start;
    session->reread_to = resumed ? resumed : session->text.count;
    if (status != MARKDOWN_CORE_OK) {
        session->widths.made = false;
    } else if (session->widths.made) {
        /* The children it read, and the first it took back, whose lead is
         * from the last it read. */
        root = session->document.root;
        const size_t last =
            resumed && root->child_count == count ? markdown_core_parser_tail(session->committed) : SIZE_MAX;
        if (widths_mend(&session->widths, root, child, last)) {
#ifndef NDEBUG
            widths_check(&session->widths, root);
#endif
        }
    }
    return status;
}

markdown_core_status markdown_core_session_edit(markdown_core_session *session, markdown_core_position start,
                                                markdown_core_position end, const uint8_t *text, size_t length,
                                                const markdown_core_document **document) {
    if (!session->flushed) {
        return MARKDOWN_CORE_SESSION_STREAMING;
    }
    if (!session->document.root) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    size_t from, to;
    if (!session_offset(session, start, &from) || !session_offset(session, end, &to) || from > to) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    if (length > MARKDOWN_CORE_SOURCE_CAPACITY - (session->text.count - (to - from)) ||
        !widths_make(&session->widths, session->document.root)) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    session_forget(session);
    uint32_t child;
    size_t line;
    session_rest(session, (size_t)start.line - 1, &child, &line);
    size_t rest_start, rest_end;
    markdown_core_parser_line_span(session->committed, line, &rest_start, &rest_end);
    /* The labels the text from the rest point declares: when the new text
     * declares others, a lookup of the children that stay may answer
     * otherwise, and the whole text is read again. */
    markdown_core_strbuf before = MARKDOWN_CORE_BUF_INIT(), after = MARKDOWN_CORE_BUF_INIT();
    if (!markdown_core_registry_labels_from(&session->committed->registry, (uint32_t)rest_start, &before) ||
        !session_replace(session, from, to, text, length)) {
        markdown_core_strbuf_free(&before);
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    const int64_t delta = (int64_t)length - (int64_t)(to - from);
    markdown_core_status status = session_reread(session, child, line, from, to, delta, false);
    /* The children before the rest point stay, and so do those the reading
     * took back after the edit. */
    if (status == MARKDOWN_CORE_OK && (child || markdown_core_parser_resumed(session->committed))) {
        if (!markdown_core_registry_labels_from(&session->committed->registry, (uint32_t)rest_start, &after)) {
            /* The children that stay may read otherwise, and no edit after
             * this one may take them as they are. */
            markdown_core_node_pool_release(&session->pool, session->document.root);
            session->document.root = NULL;
            status = MARKDOWN_CORE_ALLOCATION_FAILED;
        } else if ((before.size != after.size || (before.size && memcmp(before.ptr, after.ptr, before.size)))) {
            status = session_reread(session, 0, 0, 0, 0, 0, true);
        }
    }
    markdown_core_strbuf_free(&before);
    markdown_core_strbuf_free(&after);
    if (status == MARKDOWN_CORE_OK) {
        *document = &session->document;
    }
    return status;
}

void markdown_core_session_reread(const markdown_core_session *session, size_t *from, size_t *to) {
    *from = session->reread_from;
    *to = session->reread_to;
}

markdown_core_status markdown_core_session_locate(markdown_core_session *session, markdown_core_position position) {
    markdown_core_status status = markdown_core_context_locate(
        session->context, &session->document, (const uint8_t *)session_source(session), session->text.count, position);
    session->located |= status == MARKDOWN_CORE_OK;
    return status;
}

const markdown_core_context *markdown_core_session_context(const markdown_core_session *session) {
    return session->context;
}

const markdown_core_document *markdown_core_session_document(const markdown_core_session *session) {
    return &session->document;
}

markdown_core_text_unit markdown_core_session_unit(const markdown_core_session *session) {
    return session->document.unit;
}

size_t markdown_core_session_text_size(const markdown_core_session *session) { return session->text.count; }

void markdown_core_session_text(const markdown_core_session *session, uint8_t *bytes) {
    if (session->text.count) {
        memcpy(bytes, session->text.values, session->text.count);
    }
}

void markdown_core_session_close(markdown_core_session *session) {
    if (!session) {
        return;
    }
    session_close(session);
    markdown_core_free(session);
}

/* A fresh parse is a session that reads its source once, is flushed and
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
        status = session_flush(&session);
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

/* Declares in `registry` every label `whole` defines: the reference
 * labels its occurrences resolve, its footnotes' and its specimens'. */
static bool seed_labels(markdown_core_registry *registry, const markdown_core_document *whole) {
    bool ok = true;
    for (size_t i = 0; ok && i < markdown_core_document_reference_label_count(whole); i++) {
        markdown_core_string label;
        const markdown_core_node *target;
        if (markdown_core_document_reference_label_at(whole, i, &label, &target) == MARKDOWN_CORE_OK) {
            ok = markdown_core_registry_declare(registry, MARKDOWN_CORE_REGISTRY_REFERENCE, label.data,
                                                (bufsize_t)label.length, 0);
        }
    }
    for (size_t i = 0; ok && i < markdown_core_document_footnote_count(whole); i++) {
        const markdown_core_node *node;
        markdown_core_optional_string label;
        if (markdown_core_document_footnote_at(whole, i, &node) == MARKDOWN_CORE_OK &&
            markdown_core_footnote_label(node, &label) == MARKDOWN_CORE_OK && label.has_value) {
            ok = markdown_core_registry_declare(registry, MARKDOWN_CORE_REGISTRY_FOOTNOTE, label.value.data,
                                                (bufsize_t)label.value.length, 0);
        }
    }
    for (size_t i = 0; ok && i < markdown_core_document_specimen_count(whole); i++) {
        const markdown_core_node *node;
        if (markdown_core_document_specimen_at(whole, i, &node) == MARKDOWN_CORE_OK &&
            node->as.specimen->label.has_value) {
            ok = markdown_core_registry_declare(registry, MARKDOWN_CORE_REGISTRY_SPECIMEN,
                                                node->as.specimen->label.value.data, node->as.specimen->label.value.len,
                                                0);
        }
    }
    return ok;
}

/* The parse of the text before a cursor is a fresh parse whose parser
 * reports what it leaves open to `sink` (sink.h): the sink names the
 * text, and the document is the context query's alone. What the text
 * after the cursor defines, `whole` names: its labels are declared before
 * the parse reads a line, so an occurrence resolves as it does there. */
markdown_core_status markdown_core_parse_before_cursor(markdown_core_context_sink *sink, markdown_core_text_unit unit,
                                                       const markdown_core_document *whole,
                                                       markdown_core_document **document) {
    markdown_core_document *parsed = markdown_core_alloc(1, sizeof(*parsed));
    if (!parsed) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    markdown_core_session session = {0};
    markdown_core_status status = session_open(&session, sink->source, sink->length, unit);
    if (status == MARKDOWN_CORE_OK && whole && !seed_labels(&session.committed->registry, whole)) {
        status = MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    if (status == MARKDOWN_CORE_OK) {
        session.committed->sink = sink;
        status = session_flush(&session);
    }
    if (status == MARKDOWN_CORE_OK && sink->failed) {
        status = MARKDOWN_CORE_ALLOCATION_FAILED;
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
