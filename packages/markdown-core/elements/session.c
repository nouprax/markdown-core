#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "array.h"
#include "../include/markdown_core.h"

#include "ast_internal.h"
#include "sink.h"
#include "markdown-core-elements.h"

#include <element.h>
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
/* THE WIDTHS: where the root's children are, from their extents alone
 * (docs/plans/relative-positions.md). The children are counted in chunks of
 * SESSION_CHUNK, and each chunk keeps the bytes from where the child before
 * it ends to where its last ends: a child's start is the chunks before its
 * own and the children before it in its own, and an edit, which moves no
 * child it takes back, makes again only the chunks of the children it read
 * and of the first it took back -- all from there on when the count of
 * children changed. The chunks' widths are summed in a Fenwick tree, so the
 * width of the chunks before one, and the chunk an offset falls in, are a
 * walk of the tree's levels, however many children the root has. They are
 * made at the first edit or locate, and an edit that could not have their
 * storage leaves them to the next to make again. */
#define SESSION_CHUNK 64

typedef struct {
    /* Each chunk's width, and the tree: `sums[i]`, for i from 1, the widths
     * of the chunks from i minus its lowest set bit to i. */
    uint32_t *chunks, *sums;
    size_t count, capacity, sum_capacity;
    /* `bare`: a child the widths measured since they were made from the
     * first is not published (a definition's body), so its holder has no
     * source of its own to measure. */
    bool made, bare;
} session_widths;

/* The widths of the children of `holder`, a block below the root, and the
 * marks of what they fold into it (parser.h, THE FOLD MARKS). */
typedef struct {
    const markdown_core_node *holder;
    session_widths widths;
    markdown_core_folds folds;
} session_nest;

static void nest_free(session_nest *nest) {
    markdown_core_free(nest->widths.chunks);
    markdown_core_free(nest->widths.sums);
    markdown_core_free(nest->folds.flags);
}

/* What only an edit keeps, made at the first: the widths, and the bytes of
 * the text the last edit read again. A session that is never edited holds
 * none of it. */
typedef struct {
    session_widths widths;
    size_t reread_from, reread_to;
    /* The widths of the blocks below the root a rest point was looked for
     * in (session_rest_in): those of the last edit's chain, which it
     * mended, and those a locate since looked in. */
    session_nest *nests;
    size_t nest_count, nest_capacity;
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

/* The bytes from where the child before `child` ends to where it ends. */
static inline uint32_t child_width(const markdown_core_node *child) {
    return (uint32_t)((int64_t)child->extent.lead + child->extent.span);
}

/* Adds `step` to chunk `chunk`'s width in the tree. */
static void widths_add(session_widths *widths, size_t chunk, uint32_t step) {
    for (size_t i = chunk + 1; i <= widths->count; i += i & (0 - i)) {
        widths->sums[i] += step;
    }
}

/* The width of the chunks before chunk `chunk`. */
static uint32_t widths_before(const session_widths *widths, size_t chunk) {
    uint32_t width = 0;
    for (size_t i = chunk; i; i -= i & (0 - i)) {
        width += widths->sums[i];
    }
    return width;
}

/* Makes the chunks of the root's children from `first` to `last` again,
 * and all of them from `first` on when `last` is past the root's children,
 * with the tree. False, with the widths to be made again, when storage
 * could not be had. */
static bool widths_mend(session_widths *widths, const markdown_core_node *root, size_t first, size_t last) {
    const size_t count = (root->child_count + SESSION_CHUNK - 1) / SESSION_CHUNK;
    const bool whole = last >= root->child_count;
    if (whole) {
        uint32_t *chunks = markdown_core_reserve(widths->chunks, &widths->capacity, count, sizeof(*chunks));
        if (count && !chunks) {
            widths->made = false;
            return false;
        }
        widths->chunks = chunks;
        uint32_t *sums = markdown_core_reserve(widths->sums, &widths->sum_capacity, count + 1, sizeof(*sums));
        if (!sums) {
            widths->made = false;
            return false;
        }
        widths->sums = sums;
        widths->count = count;
        widths->bare = widths->bare && first;
        last = root->child_count ? root->child_count - 1 : 0;
    }
    for (size_t chunk = first / SESSION_CHUNK; chunk < count && chunk <= last / SESSION_CHUNK; chunk++) {
        const size_t end =
            (chunk + 1) * SESSION_CHUNK < root->child_count ? (chunk + 1) * SESSION_CHUNK : root->child_count;
        uint32_t width = 0;
        for (size_t i = chunk * SESSION_CHUNK; i < end; i++) {
            width += child_width(root->children[i]);
            widths->bare |= !root->children[i]->source.runs;
        }
        if (!whole) {
            widths_add(widths, chunk, width - widths->chunks[chunk]);
        }
        widths->chunks[chunk] = width;
    }
    /* Made again from `first` on, the tree is built again whole, in one
     * pass: each sum passes itself on to the one that holds it. */
    if (whole) {
        widths->sums[0] = 0;
        for (size_t i = 1; i <= count; i++) {
            widths->sums[i] = widths->chunks[i - 1];
        }
        for (size_t i = 1; i <= count; i++) {
            const size_t up = i + (i & (0 - i));
            if (up <= count) {
                widths->sums[up] += widths->sums[i];
            }
        }
    }
    return true;
}

/* Makes the widths when they are not made. */
static bool widths_make(session_widths *widths, const markdown_core_node *root) {
    widths->made = widths->made || widths_mend(widths, root, 0, SIZE_MAX);
    return widths->made;
}

/* Where the child `child` of `holder`, whose first child is measured from
 * `base`, starts. */
static size_t widths_start_at(const session_widths *widths, const markdown_core_node *holder, int64_t base,
                              size_t child) {
    int64_t at = base + widths_before(widths, child / SESSION_CHUNK);
    for (size_t i = child / SESSION_CHUNK * SESSION_CHUNK; i < child; i++) {
        at += child_width(holder->children[i]);
    }
    return (size_t)(at + holder->children[child]->extent.lead);
}

/* Where the root's child `child` starts. */
static size_t widths_start(const session_widths *widths, const markdown_core_node *root, size_t child) {
    return widths_start_at(widths, root, root->extent.lead, child);
}

/* Where the root's child `child` ends. */
static size_t widths_end(const session_widths *widths, const markdown_core_node *root, size_t child) {
    return widths_start(widths, root, child) + root->children[child]->extent.span;
}

/* The child of `holder`, whose first child is measured from `base`, before
 * the first that ends past `offset`, or its first. The chunks that end at
 * or before `offset` are found by going down the tree's levels. */
static uint32_t widths_child_at(const session_widths *widths, const markdown_core_node *root, int64_t base,
                                size_t offset) {
    int64_t at = base;
    size_t chunk = 0;
    size_t step = 1;
    while (step * 2 <= widths->count) {
        step *= 2;
    }
    for (; step && widths->count; step /= 2) {
        if (chunk + step <= widths->count && at + widths->sums[chunk + step] <= (int64_t)offset) {
            chunk += step;
            at += widths->sums[chunk];
        }
    }
    uint32_t child = chunk < widths->count ? (uint32_t)(chunk * SESSION_CHUNK) : root->child_count;
    while (child < root->child_count && at + child_width(root->children[child]) <= (int64_t)offset) {
        at += child_width(root->children[child++]);
    }
    return child ? child - 1 : 0;
}

static uint32_t widths_child(const session_widths *widths, const markdown_core_node *root, size_t offset) {
    return widths_child_at(widths, root, root->extent.lead, offset);
}

#ifndef NDEBUG
/* The widths an edit mended are the widths of its root made anew, and
 * each child stands where the walk finds it. */
static void widths_check(const session_widths *widths, const markdown_core_node *root) {
    session_widths fresh = {0};
    if (!widths_mend(&fresh, root, 0, SIZE_MAX)) {
        markdown_core_free(fresh.chunks);
        markdown_core_free(fresh.sums);
        return;
    }
    assert(fresh.count == widths->count);
    for (size_t i = 0; i < fresh.count; i++) {
        assert(fresh.chunks[i] == widths->chunks[i]);
        assert(fresh.sums[i + 1] == widths->sums[i + 1]);
    }
    markdown_core_free(fresh.chunks);
    markdown_core_free(fresh.sums);
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

#ifndef NDEBUG
/* The widths an edit mended are the widths of their block made anew. */
static void nest_check(const session_widths *widths, const markdown_core_node *holder) {
    session_widths fresh = {0};
    if (widths_mend(&fresh, holder, 0, SIZE_MAX)) {
        assert(fresh.count == widths->count);
        for (size_t i = 0; i < fresh.count; i++) {
            assert(fresh.chunks[i] == widths->chunks[i]);
            assert(fresh.sums[i + 1] == widths->sums[i + 1]);
        }
    }
    markdown_core_free(fresh.chunks);
    markdown_core_free(fresh.sums);
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
    if (session->edits) {
        markdown_core_free(session->edits->widths.chunks);
        markdown_core_free(session->edits->widths.sums);
        for (size_t i = 0; i < session->edits->nest_count; i++) {
            nest_free(&session->edits->nests[i]);
        }
        markdown_core_free(session->edits->nests);
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

/* THE REST POINT. The edit reads from the start of a line where a block
 * starts: a child of the root, or one below it (blocks.c, THE NESTED
 * REOPEN), `depth` blocks down the `chain` the reading reopens, at their
 * `path` (parser.h, markdown_core_edit), at the start of input line `line`.
 * `close` is where the chain's child of the root starts and `ends` where
 * the reach notes its close as reading to. */
#define SESSION_DEPTH 32

typedef struct {
    markdown_core_node *chain[SESSION_DEPTH];
    uint32_t path[SESSION_DEPTH + 1];
    /* Where each block of the chain starts. */
    uint32_t starts[SESSION_DEPTH];
    size_t depth, line, close, ends[3];
} session_rest_point;

static size_t session_line_start(const markdown_core_session *session, size_t line) {
    size_t start, end;
    markdown_core_parser_line_span(session->committed, line, &start, &end);
    return start;
}

/* Whether the edited line `edited` is a later line of `block`, whose
 * source is `window`, than its first, and the block one that holds blocks
 * and may hold a rest point: then the block the line is in is one of its
 * children, or below them. */
static bool session_inside(const markdown_core_session *session, const markdown_core_node *block,
                           markdown_core_place window, size_t edited) {
    const markdown_core_parser *parser = session->committed;
    const markdown_core_element_instance *structure = markdown_core_parser_block_kind(parser, block)->structure;
    return structure && structure->element->reopens && block->child_count &&
           markdown_core_parser_line_of(parser, window.start) < edited &&
           markdown_core_parser_line_of(parser, window.end - 1) >= edited;
}

/* The nest of `holder`, a block below the root, made when there is none;
 * NULL when storage could not be had. */
static session_nest *nest_find(session_edits *edits, const markdown_core_node *holder) {
    for (size_t i = 0; i < edits->nest_count; i++) {
        if (edits->nests[i].holder == holder) {
            return &edits->nests[i];
        }
    }
    session_nest *grew =
        markdown_core_reserve(edits->nests, &edits->nest_capacity, edits->nest_count + 1, sizeof(*edits->nests));
    if (!grew) {
        return NULL;
    }
    edits->nests = grew;
    session_nest *nest = &grew[edits->nest_count];
    *nest = (session_nest){.holder = holder};
    if (!widths_make(&nest->widths, holder)) {
        nest_free(nest);
        return NULL;
    }
    edits->nest_count++;
    return nest;
}

/* The widths of the children of `holder`, as nest_find makes them. */
static const session_widths *nest_of(session_edits *edits, const markdown_core_node *holder) {
    const session_nest *nest = nest_find(edits, holder);
    return nest ? &nest->widths : NULL;
}

/* The source of the child `child` of `holder`, a block below the root that
 * starts at `base`, from its extent. */
static markdown_core_place nest_window(const session_widths *widths, const markdown_core_node *holder, uint32_t base,
                                       size_t child) {
    const size_t start = widths_start_at(widths, holder, base, child);
    const markdown_core_place place = {(uint32_t)start, (uint32_t)(start + holder->children[child]->extent.span)};
#ifndef NDEBUG
    const markdown_core_node *before = child ? holder->children[child - 1] : NULL;
    const size_t after = before ? widths_start_at(widths, holder, base, child - 1) + before->extent.span : base;
    const markdown_core_place runs = markdown_core_node_window(holder->children[child], (uint32_t)after);
    assert(runs.start == place.start && runs.end == place.end);
#endif
    return place;
}

/* The rest point inside `block`, the chain's block `level`, whose source is
 * `window`, for the edit that begins on line `edited`: the start of a child
 * of the block on a later line of it than its first, the deepest there is,
 * found by the widths of its children (nest_of). It is the child
 * before the one that holds the edited line, as the root's is: a child that
 * starts on the edited line, or on a line the child before it ends on, has
 * no rest point
 * of its own, nor one whose start a reading from before it read past
 * (blocks.c, THE REACH), and the rest point moves back to the child before;
 * the line before it must read as the reopen can tell (blocks.c,
 * markdown_core_parser_rest_blank). The first child of a block whose
 * children abut is a rest point too, when the block starts on its line and
 * on a later line than the block above it, from line `above` on. The closes
 * of the children below the root are not noted: a child's ends as the next
 * line is read, which is no later than the rest point, and before the
 * edited line. False when none of its children, or storage, can be had. */
static bool session_rest_in(markdown_core_session *session, markdown_core_node *block, markdown_core_place window,
                            size_t level, size_t edited, size_t above, session_rest_point *rest) {
    const markdown_core_parser *parser = session->committed;
    const size_t opens = markdown_core_parser_line_of(parser, window.start);
    const markdown_core_element_instance *structure = markdown_core_parser_block_kind(parser, block)->structure;
    if (level == SESSION_DEPTH || !structure || !structure->element->reopens || !block->child_count ||
        opens >= edited) {
        return false;
    }
    session_edits *edits = session->edits;
    const size_t count = block->child_count;
    /* A child that is not published (a definition's body) has no source of
     * its own to measure: no rest point is found below. */
    const session_widths *widths = nest_of(edits, block);
    if (!widths || widths->bare) {
        return false;
    }
    rest->chain[level] = block;
    rest->starts[level] = window.start;
    size_t start, end;
    markdown_core_parser_line_span(parser, edited, &start, &end);
    /* The child that holds the edited line: the last that starts before the
     * line ends. */
    size_t held = widths_child_at(widths, block, window.start, end);
    while (held + 1 < count && widths_start_at(widths, block, window.start, held + 1) < end) {
        held++;
    }
    while (held && widths_start_at(widths, block, window.start, held) >= end) {
        held--;
    }
    if (widths_start_at(widths, block, window.start, held) >= end) {
        return false;
    }
    const markdown_core_place holder = nest_window(widths, block, window.start, held);
    const bool below = holder.end > start &&
                       session_rest_in(session, block->children[held], holder, level + 1, edited, opens + 1, rest);
    /* A rest point looked for below may have made a nest, which moves the
     * nests: the block's is found again. */
    widths = nest_of(edits, block);
    if (below) {
        rest->path[level + 1] = (uint32_t)held;
        return true;
    }
    /* As at the root, the block before the one that holds the line is read
     * again: every rule that writes backwards reaches only the block
     * before. That block is inside the child when the child is one that
     * may hold the rest point and the line is a later one of it. */
    size_t child = held;
    if (!session_inside(session, block->children[held], holder, edited)) {
        child = held ? held - 1 : 0;
    }
    /* The first child of a block whose children abut starts the block's
     * own first line: the block, reopened with none, reads it as a later
     * child's (blocks.c, markdown_core_parser_abuts). */
    const bool first = markdown_core_parser_abuts(parser, block) && opens >= above &&
                       markdown_core_parser_line_of(parser, widths_start_at(widths, block, window.start, 0)) == opens;
    while (child || first) {
        const size_t line = markdown_core_parser_line_of(parser, widths_start_at(widths, block, window.start, child));
        const size_t line_start = session_line_start(session, line);
        size_t reached;
        if (child && (line >= edited || nest_window(widths, block, window.start, child - 1).end > line_start)) {
            child--;
            continue;
        }
        if (markdown_core_parser_spanned(parser, line_start, &reached) && reached < rest->close) {
            return false;
        }
        /* What the block's first line opens its reading decided, which
         * must not have read the edited line: the line after its own, which
         * the reach does not note, or one past it, from anywhere on its
         * line -- a block identifier after the block names it from where it
         * starts, which is past the line's indentation. */
        if (!child && (edited <= line + 1 || (markdown_core_parser_spanned_inside(parser, start, line_start,
                                                                                  rest->close, rest->ends, &reached) &&
                                              markdown_core_parser_line_of(parser, reached) == line))) {
            return false;
        }
        if (markdown_core_parser_spanned_inside(parser, line_start, rest->close, rest->close, rest->ends, &reached)) {
            /* The child before the one the reading began in. */
            if (!child) {
                return false;
            }
            while (child && widths_start_at(widths, block, window.start, child) > reached) {
                child--;
            }
            child = child ? child - 1 : 0;
            continue;
        }
        if (child && markdown_core_parser_rest_blank(
                         parser, (const unsigned char *)session_source(session), rest->chain, level + 1, line,
                         nest_window(widths, block, window.start, child - 1).end) == SIZE_MAX) {
            child--;
            continue;
        }
        rest->path[level + 1] = (uint32_t)child;
        rest->depth = level + 1;
        rest->line = line;
        return true;
    }
    return false;
}

/* The rest point in the root before the line that starts at `start`, as
 * session_rest finds it: before the root's child `*child`, at the start of
 * input line `*line`. */
static void session_rest_root(const markdown_core_session *session, size_t start, uint32_t *child, size_t *line) {
    const markdown_core_node *root = session->document.root;
    const session_widths *widths = &session->edits->widths;
    size_t end;
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

/* The rest point the edit reads from, before the root's child `*child`, at
 * the start of input line `*line`: the start of the child before the first
 * that ends past the start of `line`, where the edit begins -- every rule of
 * the grammar that writes backwards reaches only the block before -- or the
 * document's start. A child that starts on a line the child before it ends
 * on has no rest point of its own, nor one whose start a reading from
 * before it read past (blocks.c, THE REACH): the rest point moves back to
 * the child before the one that reading began in, as that child may read
 * otherwise. Below the child that holds the line, the rest point may be
 * deeper (session_rest_in): then the child is the first after it, and
 * `rest` holds the chain. False when storage could not be had. */
static bool session_rest(markdown_core_session *session, size_t edited, uint32_t *child, size_t *line,
                         session_rest_point *rest) {
    const markdown_core_node *root = session->document.root;
    const session_widths *widths = &session->edits->widths;
    size_t start, end;
    if (!markdown_core_parser_sort_reach(session->committed)) {
        return false;
    }
    markdown_core_parser_line_span(session->committed, edited, &start, &end);
    const size_t edited_start = start;
    /* The child that holds the edited line. */
    uint32_t held = widths_child(widths, root, edited_start);
    if (held < root->child_count && widths_end(widths, root, held) <= edited_start) {
        held++;
    }
    const markdown_core_place window = held < root->child_count
                                           ? (markdown_core_place){(uint32_t)widths_start(widths, root, held),
                                                                   (uint32_t)widths_end(widths, root, held)}
                                           : (markdown_core_place){0, 0};
    const bool inside = held < root->child_count && session_inside(session, root->children[held], window, edited);
    session_rest_root(session, start, child, line);
    rest->depth = 0;
    if (!inside) {
        return true;
    }
    /* Its close, noted as a line after it was read, from the first after
     * its last to the next child's first, or as its last line, when its own
     * end condition or the input's end closes it (blocks.c, S_reach_close). */
    rest->close = window.start;
    rest->ends[0] = window.end + 1;
    rest->ends[1] = (held + 1 < root->child_count ? widths_start(widths, root, held + 1) : session->text.count) + 1;
    rest->ends[2] = session_line_start(session, markdown_core_parser_line_of(session->committed, window.end - 1)) + 1;
    if (session_rest_in(session, root->children[held], window, 0, edited, 0, rest)) {
        rest->path[0] = held;
        *child = held + 1;
        *line = rest->line;
    }
    return true;
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

/* The fold marks of the `depth` blocks of the chain of `rest`, which
 * session_rest_in found nests for, or NULL for one it did not. */
static void session_chain_folds(session_edits *edits, const session_rest_point *rest, size_t depth,
                                markdown_core_folds **folds) {
    for (size_t level = 0; level < depth; level++) {
        session_nest *nest = nest_find(edits, rest->chain[level]);
        folds[level] = nest ? &nest->folds : NULL;
    }
}

/* After a reading from `rest`, through `depth` blocks of its chain, which
 * had `counts` children each: the widths of each block of the chain are
 * made again from the child on its path, through the first it took back
 * when its count of children is as it was, and those of every other block
 * are let go, as the reading may have let the block go. */
static void session_mend_nests(markdown_core_session *session, const session_rest_point *rest, size_t depth,
                               const uint32_t *counts) {
    session_edits *edits = session->edits;
    size_t kept = 0;
    for (size_t i = 0; i < edits->nest_count; i++) {
        session_nest nest = edits->nests[i];
        size_t level = 0;
        while (rest && level < depth && rest->chain[level] != nest.holder) {
            level++;
        }
        if (rest && level < depth) {
            const markdown_core_node *holder = nest.holder;
            const uint32_t tail = markdown_core_parser_tail_in(session->committed, level);
            const size_t last = tail != UINT32_MAX && holder->child_count == counts[level] ? tail : SIZE_MAX;
            const size_t marked = rest->path[level + 1] / MARKDOWN_CORE_FOLD_CHUNK;
            nest.folds.count = nest.folds.count < marked ? nest.folds.count : marked;
            if (widths_mend(&nest.widths, holder, rest->path[level + 1], last)) {
#ifndef NDEBUG
                nest_check(&nest.widths, holder);
#endif
                edits->nests[kept++] = nest;
                continue;
            }
        }
        nest_free(&nest);
    }
    edits->nest_count = kept;
}

/* The flushed parse put back before the root's child `child`, at input line
 * `line`, or below the root as `rest` says, reads the text, in which an
 * edit replaced the old text from `from` to `to` and moved what follows by
 * `delta`, and is flushed again: to its end, or to where it takes the old
 * children back, which is not before `past`, unless `whole` says to read
 * all of it. */
static markdown_core_status session_reread(markdown_core_session *session, uint32_t child, size_t line, size_t from,
                                           size_t to, int64_t delta, size_t past, bool whole,
                                           const session_rest_point *rest) {
    markdown_core_node *root = session->document.root;
    session_edits *edits = session->edits;
    const size_t count = root->child_count;
    session->document.root = NULL;
    size_t start, end;
    markdown_core_parser_line_span(session->committed, line, &start, &end);
    const uint32_t prior = child ? (uint32_t)widths_end(&edits->widths, root, child - 1) : (uint32_t)root->extent.lead;
    /* The chain's blocks stay, and how many children each had. */
    const size_t depth = rest && !whole ? rest->depth : 0;
    uint32_t counts[SESSION_DEPTH];
    for (size_t level = 0; level < depth; level++) {
        counts[level] = rest->chain[level]->child_count;
    }
    markdown_core_folds *folds[SESSION_DEPTH];
    session_chain_folds(edits, rest, depth, folds);
    if (!markdown_core_parser_reopen(session->committed, root, child, line, prior, from, to, delta, past,
                                     session_source(session), whole, rest ? rest->path : NULL, rest ? rest->depth : 0,
                                     depth ? folds : NULL)) {
        markdown_core_node_pool_release(&session->pool, root);
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    const markdown_core_status status = session_flush(session);
    const size_t resumed = markdown_core_parser_resumed(session->committed);
    edits->reread_from = start;
    edits->reread_to = resumed ? resumed : session->text.count;
    if (status != MARKDOWN_CORE_OK) {
        edits->widths.made = false;
    } else if (edits->widths.made) {
        /* The children it read, and the first it took back, whose lead is
         * from the last it read. */
        root = session->document.root;
        const size_t last =
            resumed && root->child_count == count ? markdown_core_parser_tail(session->committed) : SIZE_MAX;
        if (widths_mend(&edits->widths, root, child - (rest && rest->depth ? 1 : 0), last)) {
#ifndef NDEBUG
            widths_check(&edits->widths, root);
#endif
        }
    }
    session_mend_nests(session, status == MARKDOWN_CORE_OK ? rest : NULL, depth, counts);
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
        uint32_t child;
        size_t line;
        session_rest_point rest;
        if (!session_rest(session, markdown_core_parser_line_of(session->committed, inside), &child, &line, &rest)) {
            status = MARKDOWN_CORE_ALLOCATION_FAILED;
            break;
        }
        status = session_reread(session, child, line, inside, inside, 0, past, false, &rest);
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
    if (!widths_make(&session->edits->widths, session->document.root)) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    session_forget(session);
    uint32_t child;
    size_t line;
    session_rest_point rest;
    if (!session_rest(session, (size_t)start.line - 1, &child, &line, &rest)) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    size_t rest_start, rest_end;
    markdown_core_parser_line_span(session->committed, line, &rest_start, &rest_end);
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
    markdown_core_status status = compact ? session_reread(session, 0, 0, from, to, delta, 0, true, NULL)
                                          : session_reread(session, child, line, from, to, delta, 0, false, &rest);
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

/* THE LOOK (blocks.c): what the locate reads the text before the cursor
 * with, from the rest point it found. */
typedef struct session_look {
    markdown_core_session *session;
    uint32_t child, span;
    size_t line, before;
    const session_rest_point *rest;
    uint32_t starts[SESSION_DEPTH];
    markdown_core_folds *folds[SESSION_DEPTH];
} session_look;

static markdown_core_status session_look_read(void *owner, markdown_core_context_sink *sink) {
    const session_look *look = owner;
    markdown_core_parser *parser = look->session->committed;
    parser->sink = sink;
    const bool ok = markdown_core_parser_look(parser, look->session->document.root, look->child, look->line,
                                              session_source(look->session), sink->length, look->rest->path,
                                              look->rest->depth, look->starts, look->before, look->span, look->folds);
    parser->sink = NULL;
    return ok && !sink->failed ? MARKDOWN_CORE_OK : MARKDOWN_CORE_ALLOCATION_FAILED;
}

static void session_look_line(void *owner, size_t offset, size_t *line, size_t *start) {
    const session_look *look = owner;
    *line = markdown_core_parser_line_of(look->session->committed, offset);
    *start = session_line_start(look->session, *line);
}

markdown_core_status markdown_core_session_locate(markdown_core_session *session, markdown_core_position position) {
    /* THE PREFIX FROM A REST POINT (context.c): a flushed session reads the
     * text before the cursor from the rest point an edit of the cursor's
     * line reads from (THE LOOK), and the query reads the lines from that
     * rest point through the one after the cursor's, from the session's
     * line table, and the blocks that hold them: down the rest point's
     * chain, and at its level the children from the one before the rest
     * point's, which some readers measure the first from, through the one
     * that holds the cursor's line. */
    const markdown_core_node *root = session->document.root;
    size_t start, end;
    if (!session->flushed || !root || !root->child_count || position.line < 1 ||
        !markdown_core_parser_line_span(session->committed, (size_t)position.line - 1, &start, &end)) {
        markdown_core_status status =
            markdown_core_context_locate(session->context, &session->document, (const uint8_t *)session_source(session),
                                         session->text.count, position, NULL);
        session->located |= status == MARKDOWN_CORE_OK;
        return status;
    }
    if (!session->edits && !(session->edits = markdown_core_alloc(1, sizeof(*session->edits)))) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    const session_widths *widths = &session->edits->widths;
    if (!widths_make(&session->edits->widths, root)) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    session_rest_point rest;
    session_look look = {.session = session, .rest = &rest};
    if (!session_rest(session, (size_t)position.line - 1, &look.child, &look.line, &rest)) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    markdown_core_locus locus = {.look = session_look_read, .line = session_look_line, .owner = &look};
    /* Down the chain, each block's one child that holds the next, where
     * it starts, as session_rest_in measured them, and at the rest point's
     * level the children through the one that holds the cursor's line. */
    const markdown_core_node *holder = root;
    for (size_t level = 0; level <= rest.depth; level++) {
        markdown_core_window_level *at = &locus.window.levels[level];
        const bool deepest = level == rest.depth;
        const uint32_t first = !deepest ? rest.path[level] : !level ? look.child : rest.path[level];
        at->holder = holder;
        at->first = at->last = deepest && first ? first - 1 : first;
        if (!level) {
            at->start = (uint32_t)widths_start(widths, root, at->first);
            if (deepest) {
                at->last = widths_child(widths, root, end);
                while (at->last + 1 < root->child_count && widths_start(widths, root, at->last + 1) <= end) {
                    at->last++;
                }
            }
        } else {
            const session_widths *nest = nest_of(session->edits, holder);
            const uint32_t base = rest.starts[level - 1];
            if (!nest) {
                return MARKDOWN_CORE_ALLOCATION_FAILED;
            }
            at->start = (uint32_t)widths_start_at(nest, holder, base, at->first);
            if (deepest) {
                at->last = first;
                while (at->last + 1 < holder->child_count && widths_start_at(nest, holder, base, at->last + 1) <= end) {
                    at->last++;
                }
                look.before = first ? nest_window(nest, holder, base, first - 1).end : 0;
            }
        }
        if (!deepest) {
            look.starts[level] = at->start;
            holder = rest.chain[level];
        }
    }
    locus.window.depth = rest.depth + 1;
    session_chain_folds(session->edits, &rest, rest.depth, look.folds);
    /* From the document's start, the lines reach the first child's too,
     * past the envelope, whose members a cursor in it reads. */
    size_t cursor = (size_t)position.line - 1, next, after;
    if (!rest.depth && !look.child) {
        const size_t opening = markdown_core_parser_line_of(session->committed, widths_start(widths, root, 0));
        cursor = opening > cursor ? opening : cursor;
    }
    const bool more = markdown_core_parser_line_span(session->committed, cursor + 1, &next, &after);
    locus.lines = (markdown_core_source_lines){.count = cursor + 1 + more - look.line, .base = look.line};
    locus.lines.starts = markdown_core_alloc(locus.lines.count, sizeof(*locus.lines.starts));
    if (!locus.lines.starts) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    for (size_t i = 0; i < locus.lines.count; i++) {
        markdown_core_parser_line_span(session->committed, look.line + i, &locus.lines.starts[i], &after);
    }
    look.span = (uint32_t)(position.line - look.line + 2);
    markdown_core_status status =
        markdown_core_context_locate(session->context, &session->document, (const uint8_t *)session_source(session),
                                     session->text.count, position, &locus);
    markdown_core_free(locus.lines.starts);
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

markdown_core_string markdown_core_session_source(const markdown_core_session *session) {
    markdown_core_string text = {(const uint8_t *)session_source(session), session->text.count};
    return text;
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

/* Declares in `roster` every label `whole` defines: the reference
 * labels its occurrences resolve, its footnotes' and its specimens'. */
static bool seed_labels(markdown_core_roster *roster, const markdown_core_document *whole) {
    bool ok = true;
    for (size_t i = 0; ok && i < markdown_core_document_reference_label_count(whole); i++) {
        markdown_core_string label;
        const markdown_core_node *target;
        if (markdown_core_document_reference_label_at(whole, i, &label, &target) == MARKDOWN_CORE_OK) {
            ok = markdown_core_roster_declare(roster, MARKDOWN_CORE_ROSTER_REFERENCE, label.data,
                                              (bufsize_t)label.length, 0);
        }
    }
    for (size_t i = 0; ok && i < markdown_core_document_footnote_count(whole); i++) {
        const markdown_core_node *node;
        markdown_core_optional_string label;
        if (markdown_core_document_footnote_at(whole, i, &node) == MARKDOWN_CORE_OK &&
            markdown_core_footnote_label(node, &label) == MARKDOWN_CORE_OK && label.has_value) {
            ok = markdown_core_roster_declare(roster, MARKDOWN_CORE_ROSTER_FOOTNOTE, label.value.data,
                                              (bufsize_t)label.value.length, 0);
        }
    }
    for (size_t i = 0; ok && i < markdown_core_document_specimen_count(whole); i++) {
        const markdown_core_node *node;
        if (markdown_core_document_specimen_at(whole, i, &node) == MARKDOWN_CORE_OK &&
            node->as.specimen->label.has_value) {
            ok =
                markdown_core_roster_declare(roster, MARKDOWN_CORE_ROSTER_SPECIMEN, node->as.specimen->label.value.data,
                                             node->as.specimen->label.value.len, 0);
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
    if (status == MARKDOWN_CORE_OK && whole && !seed_labels(&session.committed->roster, whole)) {
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
