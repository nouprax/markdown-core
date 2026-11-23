#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "array.h"
#include "../include/markdown_core.h"

#include "ast_internal.h"
#include "markdown-core-elements.h"

#include <element.h>
#include <index.h>
#include <parser.h>
#include <utf8.h>

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
/* What only an edit keeps, made at the first: the child index (index.h),
 * and the bytes of the text the last edit read again. A session that is
 * never edited holds none of it. */
typedef struct {
    markdown_core_index index;
    size_t reread_from, reread_to;
    /* The most text the session has held since it last read all of it into
     * storage of its own (THE COMPACTION). */
    size_t peak;
} session_edits;

struct markdown_core_session {
    MARKDOWN_CORE_ARRAY(uint8_t) text;
    markdown_core_parser *committed;
    markdown_core_node_pool pool;
    markdown_core_revision revision;
    markdown_core_document document;
    markdown_core_context *context;
    bool located;
    bool flushed;
    session_edits *edits;
};

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
    if (session->edits) {
        markdown_core_index_free(&session->edits->index);
        markdown_core_free(session->edits);
    }
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

/* The widths of the children of `holder`, a block below the root
 * (markdown_core_index_nest). */
static inline const markdown_core_widths *session_nest(const markdown_core_session *session,
                                                       const markdown_core_node *holder) {
    return markdown_core_index_nest(&session->edits->index, holder);
}

/* Where the root's child `child` starts and ends, and the root's child
 * before the first that ends past `offset`, or its first. */
static inline size_t widths_start(const markdown_core_widths *widths, const markdown_core_node *root, size_t child) {
    return markdown_core_widths_start(widths, root, root->extent.lead, child);
}

static inline size_t widths_end(const markdown_core_widths *widths, const markdown_core_node *root, size_t child) {
    return markdown_core_widths_window(widths, root, (uint32_t)root->extent.lead, child).end;
}

#ifndef NDEBUG
/* Each of the root's children stands where the walk finds it. */
static void session_walk_check(const markdown_core_widths *widths, const markdown_core_node *root) {
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

static size_t session_line_start(const markdown_core_session *session, size_t line) {
    size_t start, end;
    markdown_core_parser_line_span(session->committed, line, &start, &end);
    return start;
}

/* Replaces the bytes from `from` to `to` of the text with `size` bytes of
 * `text`. False, with the text as it was, when storage could not be had. */
static bool session_replace(markdown_core_session *session, size_t from, size_t to, const uint8_t *text, size_t size) {
    const size_t rest = session->text.count - to;
    const size_t total = from + size + rest;
    if (!markdown_core_array_reserve(&session->text, total)) {
        return false;
    }
    /* In place: the text after the replaced moves, and what replaces it is
     * written where the replaced was. */
    markdown_core_ahead_unsettle(&session->text.ahead, from);
    uint8_t *values = session->text.values;
    if (rest && size != to - from) {
        memmove(values + from + size, values + to, rest);
    }
    if (size) {
        memcpy(values + from, text, size);
    }
    session->text.count = total;
    return true;
}

/* The fold marks of the `depth` blocks of the chain of `rest`, whose
 * nests its search found or made, or NULL for one it did not, as a leaf the
 * chain ends in. The nests are found, never made: a nest made for a later
 * block would move the nests whose marks the earlier ones point into. */
static void session_chain_folds(session_edits *edits, const markdown_core_rest *rest, size_t depth,
                                markdown_core_folds **folds) {
    for (size_t level = 0; level < depth; level++) {
        folds[level] = markdown_core_index_folds(&edits->index, rest->chain[level]);
    }
}

/* The flushed parse put back at the rest `rest` (index.h, THE REST), or
 * before the root's child `child`, at input line `line`, which started at
 * `start` before the edit, reads the text, in which an
 * edit replaced the old text from `from` to `to` and moved what follows by
 * `delta`, and is flushed again: to its end, or to where it takes the old
 * children back, which is not before `past`, unless `whole` says to read
 * all of it. */
static markdown_core_status session_reread(markdown_core_session *session, uint32_t child, size_t line, size_t start,
                                           size_t from, size_t to, int64_t delta, size_t past, bool whole,
                                           const markdown_core_rest *rest) {
    markdown_core_node *root = session->document.root;
    session_edits *edits = session->edits;
    const size_t count = root->child_count;
    session->document.root = NULL;
    const uint32_t prior =
        child ? (uint32_t)widths_end(&edits->index.widths, root, child - 1) : (uint32_t)root->extent.lead;
    /* The chain's blocks stay, and how many children each had. */
    const size_t depth = rest && !whole ? rest->depth : 0;
    uint32_t counts[MARKDOWN_CORE_REST_DEPTH];
    for (size_t level = 0; level < depth; level++) {
        counts[level] = rest->chain[level]->child_count;
    }
    markdown_core_folds *folds[MARKDOWN_CORE_REST_DEPTH];
    session_chain_folds(edits, rest, depth, folds);
    markdown_core_chain_place places[MARKDOWN_CORE_REST_DEPTH];
    for (size_t level = 0; rest && level < rest->depth; level++) {
        places[level] = (markdown_core_chain_place){rest->starts[level], rest->priors[level]};
    }
    if (!markdown_core_parser_reopen(session->committed, root, child, line, prior, from, to, delta, past,
                                     session_source(session), whole, rest ? rest->path : NULL, places,
                                     rest ? rest->depth : 0, depth && rest->inside ? rest->mid : SIZE_MAX,
                                     depth ? folds : NULL, &edits->index)) {
        markdown_core_node_pool_release(&session->pool, root);
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    const markdown_core_status status = session_flush(session);
    const size_t resumed = markdown_core_parser_resumed(session->committed);
    edits->reread_from = start;
    edits->reread_to = resumed ? resumed : session->text.count;
    if (status != MARKDOWN_CORE_OK) {
        markdown_core_index_drop(&edits->index);
    }
    /* The chain's nests first: the root's child on the path reaches as its
     * nest says (THE REACH OF A NODE, index.c). */
    const bool read = status == MARKDOWN_CORE_OK && rest;
    markdown_core_index_mend_nests(&edits->index, read ? rest->chain : NULL, read ? rest->path : NULL, read ? depth : 0,
                                   counts, delta);
    if (status == MARKDOWN_CORE_OK && edits->index.widths.made) {
        /* The children it read, and the first it took back, whose lead is
         * from the last it read. */
        root = session->document.root;
        const uint32_t tail = resumed ? markdown_core_parser_tail(session->committed) : UINT32_MAX;
        if (markdown_core_index_mend(&edits->index, root, child - (rest && rest->depth ? 1 : 0), tail, count, delta)) {
#ifndef NDEBUG
            session_walk_check(&edits->index.widths, root);
#endif
        }
    }
    return status;
}

/* THE DEFINITIONS AN EDIT ALTERED (plan 12.3, step 5; E-D2): a lookup of
 * the children an edit kept, before its rest point and from where it took
 * the old children back, answers otherwise only for a label the edit made
 * declared or left undeclared (markdown_core_parser_altered_labels). Every
 * inline root's lookups are its uses (markdown_core_parser_uses_of), so the
 * blocks that hold a use of one are known, and only they are read again, as
 * an edit of nothing just past where the root starts reads them when it
 * takes no old child back before the last, which completes their content
 * against the definitions the edit left and keeps the identity of every
 * node. */

static int session_size_order(const void *a, const void *b) {
    const size_t left = *(const size_t *)a, other = *(const size_t *)b;
    return (left > other) - (left < other);
}

/* The most readings one refresh makes: places further apart than the
 * others are read in readings of their own, and the rest share them, so
 * that a label many blocks name costs a few readings and not one each. */
#define SESSION_PASSES 8

/* The kept text in `spans`, `count` pairs of offsets, read again where a
 * lookup of one of the `altered` labels may be made. The bytes read again
 * join those the edit read. */
static markdown_core_status session_refresh(markdown_core_session *session, const markdown_core_strbuf *altered,
                                            const size_t *spans, size_t count) {
    session_edits *edits = session->edits;
    size_t *places = NULL, place_count = 0;
    const bool ok = markdown_core_parser_uses_of(session->committed, altered, spans, count, &places, &place_count);
    /* The widest gaps between places part the readings, the earlier of
     * two as wide first: `cuts` holds the places they follow, widest
     * first. */
    size_t cuts[SESSION_PASSES - 1], cut_count = 0;
    for (size_t i = 0; ok && i + 1 < place_count; i++) {
        const size_t width = places[i + 1] - places[i];
        size_t at = cut_count;
        while (at > 0 && places[cuts[at - 1] + 1] - places[cuts[at - 1]] < width) {
            at--;
        }
        if (at == SESSION_PASSES - 1) {
            continue;
        }
        cut_count -= cut_count == SESSION_PASSES - 1;
        memmove(cuts + at + 1, cuts + at, (cut_count - at) * sizeof(*cuts));
        cuts[at] = i;
        cut_count++;
    }
    qsort(cuts, cut_count, sizeof(*cuts), session_size_order);
    markdown_core_status status = ok ? MARKDOWN_CORE_OK : MARKDOWN_CORE_ALLOCATION_FAILED;
    size_t reread_from = edits->reread_from, reread_to = edits->reread_to;
    for (size_t first = 0, cut = 0; status == MARKDOWN_CORE_OK && first < place_count;) {
        while (cut < cut_count && cuts[cut] < first) {
            cut++;
        }
        const size_t last = cut < cut_count ? cuts[cut] : place_count - 1;
        /* An edit just past the opener: one at a block's start would be the
         * end of the block before. */
        const size_t inside = places[first] + 1, past = places[last] + 1;
        markdown_core_rest rest;
        const markdown_core_rest_edit ask = {
            .line = markdown_core_parser_line_of(session->committed, inside), .from = inside, .to = inside};
        if (!markdown_core_index_rest(&edits->index, session_source(session), session->text.count, &ask, &rest)) {
            status = MARKDOWN_CORE_ALLOCATION_FAILED;
            break;
        }
        status = session_reread(session, rest.child, rest.line, session_line_start(session, rest.line), inside, inside,
                                0, past, false, &rest);
        reread_from = edits->reread_from < reread_from ? edits->reread_from : reread_from;
        reread_to = edits->reread_to > reread_to ? edits->reread_to : reread_to;
        /* The places the reading read past need no reading of their own. */
        for (first = last + 1; first < place_count && places[first] < edits->reread_to; first++) {
        }
    }
    markdown_core_free(places);
    edits->reread_from = reread_from;
    edits->reread_to = reread_to;
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
    if (length > MARKDOWN_CORE_SOURCE_CAPACITY - (session->text.count - (to - from))) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    if (!session->edits && !(session->edits = markdown_core_alloc(1, sizeof(*session->edits)))) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    if (!markdown_core_index_make(&session->edits->index, session->committed, session->document.root)) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    session_forget(session);
    markdown_core_rest rest;
    const markdown_core_rest_edit ask = {
        .line = (size_t)start.line - 1, .from = from, .to = to, .text = text, .length = length, .prose = true};
    if (!markdown_core_index_rest(&session->edits->index, session_source(session), session->text.count, &ask, &rest)) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    const uint32_t child = rest.child;
    const size_t line = rest.line;
    /* The text before the rest point stays, past the line's start when the
     * reading stands inside the line (A REST INSIDE A LINE). */
    const size_t line_start = session_line_start(session, line);
    const size_t rest_start = rest.depth && rest.inside ? rest.mid : line_start;
    if (session->text.count > session->edits->peak) {
        session->edits->peak = session->text.count;
    }
    if (!session_replace(session, from, to, text, length)) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    const int64_t delta = (int64_t)length - (int64_t)(to - from);
    session_edits *const edits = session->edits;
    /* THE COMPACTION (plan 12.4, E-D4): the pool keeps what each reading
     * let go for the next, so it holds what the most text held needed.
     * Once the text is less than half of that, the holes exceed what it
     * needs, and the edit reads all of it into new slabs; the old go with
     * the old nodes, which keep them only until the reading lets them go. */
    const bool compact = session->text.count < edits->peak / 2;
    if (compact) {
        markdown_core_node_pool_dispose(&session->pool);
    }
    markdown_core_status status =
        compact ? session_reread(session, 0, 0, 0, from, to, delta, 0, true, NULL)
                : session_reread(session, child, line, line_start, from, to, delta, 0, false, &rest);
    if (compact) {
        markdown_core_node_pool_dispose(&session->pool);
        if (status == MARKDOWN_CORE_OK) {
            markdown_core_parser_compact(session->committed);
        }
        edits->peak = session->text.count;
    }
    /* The children before the rest point stay, and so do those the reading
     * took back after the edit: their lookups of a label the edit altered
     * are made again (THE DEFINITIONS AN EDIT ALTERED). */
    const size_t resumed = markdown_core_parser_resumed(session->committed);
    if (status == MARKDOWN_CORE_OK && !compact && (child || resumed)) {
        markdown_core_strbuf altered = MARKDOWN_CORE_BUF_INIT();
        if (!markdown_core_parser_altered_labels(session->committed, rest_start, &altered)) {
            /* The children that stay may read otherwise, and no edit after
             * this one may take them as they are. */
            markdown_core_node_pool_release(&session->pool, session->document.root);
            session->document.root = NULL;
            status = MARKDOWN_CORE_ALLOCATION_FAILED;
        } else if (altered.size) {
            const size_t spans[] = {0, rest_start, resumed, session->text.count};
            status = session_refresh(session, &altered, spans, resumed ? 2 : 1);
            if (status != MARKDOWN_CORE_OK && session->document.root) {
                /* The children that name an altered label read as they
                 * did, and no edit after this one may take them so. */
                markdown_core_node_pool_release(&session->pool, session->document.root);
                session->document.root = NULL;
            }
        }
        markdown_core_strbuf_free(&altered);
    }
    if (status == MARKDOWN_CORE_OK) {
        *document = &session->document;
    }
    return status;
}

void markdown_core_session_reread(const markdown_core_session *session, size_t *from, size_t *to) {
    const session_edits *edits = session->edits;
    *from = edits ? edits->reread_from : 0;
    *to = edits ? edits->reread_to : 0;
}

/* The line that holds `offset`, counted from 0, and where it starts, for
 * the locus of a locate (ast_internal.h). */
static void session_locus_line(void *owner, size_t offset, size_t *line, size_t *start) {
    const markdown_core_session *session = owner;
    *line = markdown_core_parser_line_of(session->committed, offset);
    *start = session_line_start(session, *line);
}

static markdown_core_status session_locate_flushed(markdown_core_session *session, markdown_core_position position,
                                                   markdown_core_context **context);

/* A text no flushed session holds -- a session's text while it streams --
 * is read whole: its context is the document's that a parse of it makes. */
static markdown_core_status session_locate_whole(const uint8_t *source, size_t size, markdown_core_text_unit unit,
                                                 markdown_core_position position, markdown_core_context **context) {
    markdown_core_document *document = NULL;
    markdown_core_status status = markdown_core_document_parse_in(source, size, unit, &document);
    if (status == MARKDOWN_CORE_OK) {
        status = markdown_core_document_context(document, source, size, position, context);
    }
    markdown_core_document_free(document);
    return status;
}

markdown_core_status markdown_core_session_locate(markdown_core_session *session, markdown_core_position position) {
    markdown_core_context *fresh = NULL;
    const markdown_core_status status =
        session->flushed ? session_locate_flushed(session, position, &fresh)
                         : session_locate_whole((const uint8_t *)session_source(session), session->text.count,
                                                session->document.unit, position, &fresh);
    if (status == MARKDOWN_CORE_OK) {
        markdown_core_context_take(session->context, fresh);
        session->located = true;
    }
    return status;
}

static markdown_core_status session_locate_flushed(markdown_core_session *session, markdown_core_position position,
                                                   markdown_core_context **context) {
    /* THE WINDOW: the query reads the blocks that hold the cursor (context.c,
     * THE CONTEXT FROM THE DOCUMENT), so it keeps to them, down the tree
     * from the root, from the session's widths: at each level the children
     * from the one before the first that ends past the cursor through the
     * last that starts at or before it, and down into the one child that
     * holds the cursor with none of its siblings touching it, a block of
     * blocks. Its lines
     * are those from the deepest level's first child's through the one
     * after the cursor's, from the session's line table. */
    const markdown_core_node *root = session->document.root;
    size_t start, end, cursor;
    if (position.line < 1 || position.column < 1 ||
        !markdown_core_parser_line_span(session->committed, (size_t)position.line - 1, &start, &end) ||
        !session_offset(session, position, &cursor)) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    if (!session->edits && !(session->edits = markdown_core_alloc(1, sizeof(*session->edits)))) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    if (!markdown_core_index_make(&session->edits->index, session->committed, root)) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    markdown_core_locus locus = {.line = session_locus_line, .owner = session};
    const markdown_core_node *holder = root;
    const markdown_core_widths *nest = &session->edits->index.widths;
    int64_t base = root->extent.lead;
    size_t level = 0, first_line = 0;
    while (holder->child_count && level < MARKDOWN_CORE_WINDOW_DEPTH) {
        markdown_core_window_level *at = &locus.window.levels[level];
        at->holder = holder;
        at->first = at->last = markdown_core_widths_child(nest, holder, base, cursor);
        /* Children that touch (a row's padded cells, which stand where its
         * last ends) start at one place, and the one before the first that
         * ends past the cursor may hold it too. */
        while (at->first > 0 &&
               markdown_core_widths_window(nest, holder, (uint32_t)base, at->first - 1).end >= cursor) {
            at->first--;
        }
        while (at->last + 1 < holder->child_count &&
               markdown_core_widths_start(nest, holder, base, at->last + 1) <= cursor) {
            at->last++;
        }
        at->start = (uint32_t)markdown_core_widths_start(nest, holder, base, at->first);
        first_line = markdown_core_parser_line_of(session->committed, at->start);
        level++;
        /* The one child that holds the cursor inside it. */
        uint32_t inside = UINT32_MAX;
        for (uint32_t child = at->first; child <= at->last; child++) {
            const markdown_core_place place = markdown_core_widths_window(nest, holder, (uint32_t)base, child);
            if (place.start < cursor && cursor < place.end) {
                inside = inside == UINT32_MAX ? child : UINT32_MAX - 1;
            } else if (place.start <= cursor && cursor <= place.end) {
                inside = UINT32_MAX - 1;
            }
        }
        /* An inline root's children are read whole. */
        const markdown_core_node *block = inside < UINT32_MAX - 1 ? holder->children[inside] : NULL;
        if (!block || !block->child_count ||
            (block->children[0]->kind & MARKDOWN_CORE_NODE_TYPE_INLINE) == MARKDOWN_CORE_NODE_TYPE_INLINE) {
            break;
        }
        const size_t from = markdown_core_widths_start(nest, holder, base, inside);
        at->first = at->last = inside;
        at->start = (uint32_t)from;
        holder = holder->children[inside];
        base = markdown_core_holder_base(holder, from);
        if (!(nest = session_nest(session, holder))) {
            return MARKDOWN_CORE_ALLOCATION_FAILED;
        }
    }
    locus.window.depth = level;
    /* From the document's start, the lines reach the first child's too,
     * past the envelope, whose members a cursor in it reads. */
    size_t last = (size_t)position.line - 1, next, after;
    if (level == 1 && !locus.window.levels[0].first) {
        first_line = 0;
        const size_t opening = markdown_core_parser_line_of(session->committed, widths_start(nest, root, 0));
        last = opening > last ? opening : last;
    }
    first_line = first_line < (size_t)position.line - 1 ? first_line : (size_t)position.line - 1;
    const bool more = markdown_core_parser_line_span(session->committed, last + 1, &next, &after);
    locus.lines = (markdown_core_source_lines){.count = last + 1 + more - first_line, .base = first_line};
    locus.lines.starts = markdown_core_alloc(locus.lines.count, sizeof(*locus.lines.starts));
    if (!locus.lines.starts) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    for (size_t i = 0; i < locus.lines.count; i++) {
        markdown_core_parser_line_span(session->committed, first_line + i, &locus.lines.starts[i], &after);
    }
    markdown_core_status status = markdown_core_context_read(
        &session->document, (const uint8_t *)session_source(session), session->text.count, position, &locus, context);
    markdown_core_free(locus.lines.starts);
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

markdown_core_string markdown_core_session_source(const markdown_core_session *session) {
    markdown_core_string text = {(const uint8_t *)session_source(session), session->text.count};
    return text;
}

size_t markdown_core_session_lines(const markdown_core_session *session) {
    return session->flushed ? markdown_core_parser_line_count(session->committed) : 0;
}

markdown_core_status markdown_core_session_line(const markdown_core_session *session, size_t line,
                                                markdown_core_string *text) {
    if (!session->flushed) {
        return MARKDOWN_CORE_SESSION_STREAMING;
    }
    size_t start, end;
    if (!line || !markdown_core_parser_line_span(session->committed, line - 1, &start, &end)) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    const uint8_t *bytes = (const uint8_t *)session_source(session);
    if (end > start && bytes[end - 1] == '\n') {
        end--;
    }
    if (end > start && bytes[end - 1] == '\r') {
        end--;
    }
    *text = (markdown_core_string){bytes + start, end - start};
    return MARKDOWN_CORE_OK;
}

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

markdown_core_status markdown_core_document_parse(const uint8_t *source, size_t length,
                                                  markdown_core_document **document) {
    return markdown_core_document_parse_in(source, length, MARKDOWN_CORE_TEXT_UNIT_UTF8, document);
}
