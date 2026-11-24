#include "markdown-core-elements.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "../include/markdown_core.h"

#include "ast_internal.h"

#include <node.h>

/* A slot as the context keeps it: the public value, where its prefix
 * strings begin in the context's prefixes, and what its spelling reads
 * (markdown_core_slot_spell): the offsets in the context's copy of the
 * cursor's line of the field's start, or SIZE_MAX when it began on an
 * earlier line, of the slot's start, the cursor and the slot's end, and of
 * its construct's opener, or SIZE_MAX; how the field goes on past a cursor,
 * from `lead` bytes before its start, or NULL when the line does not hold
 * them. */
typedef struct {
    markdown_core_slot slot;
    size_t prefix;
    size_t field, start, cursor, end, opener;
    /* The delimiter before the slot that its source opens with and does not
     * hold: a destination's `<`, a braced key's `{`, the quote of a value
     * that began on an earlier line, or a properties member's quote or
     * literal's `|`; or 0. */
    uint8_t wrap;
    /* Whether the field goes on to another line, which no insertion on the
     * cursor's line replaces whole. */
    bool split;
} context_slot;

/* THE CONTEXT (docs/plans/2026-10-23-lsp-completion-engine.md, 3.1): its
 * blocks, inlines and slots, and the bytes of every slot's prefixes, which
 * it owns. */
struct markdown_core_context {
    markdown_core_opener *blocks;
    size_t block_count;
    markdown_core_opener *inlines;
    size_t inline_count;
    context_slot *slots;
    size_t slot_count;
    markdown_core_string *prefixes;
    uint8_t *bytes;
    /* The cursor's line, its terminator not included, and whether it is a
     * table's row, whose cells an unescaped `|` splits. */
    uint8_t *line;
    size_t size;
    bool row;
    /* What the inline openers of earlier lines leave open at the line's
     * start, as their elements say (write.h, `carry`). */
    markdown_core_carried carried;
};

/* What the query reads: the whole text and its line starts, the document
 * parsed from it, and the cursor's byte; the part of the document the query
 * reads, or NULL for all of it: its window of the tree, and its lines. */
typedef struct {
    const markdown_core_document *document;
    const uint8_t *source;
    size_t length;
    markdown_core_source_lines lines;
    size_t cursor;
    const markdown_core_locus *locus;
    const markdown_core_window *window;
    struct column_mark *marks;
} context_query;

/* The last byte a UTF-16 column was counted to: its line, its offset and
 * its column. The query asks for the scopes of its blocks, openers and
 * slots mostly in source order, so a count on the same line goes on from
 * the last instead of from the line's start. Scope starts and scope ends
 * keep a mark each, since the blocks nested on one line start in order but
 * all end at the line's end. */
typedef struct column_mark {
    size_t line, offset, units;
    bool set;
} column_mark;

#ifndef NDEBUG
/* Whether the query's lines hold `offset`: a table of some lines only holds
 * the offsets through the end of its last line, and the locus answers for
 * those before its first. */
static bool lines_hold(const context_query *query, size_t offset) {
    const size_t *starts = query->lines.starts;
    size_t last = starts[query->lines.count - 1];
    while (last < query->length && query->source[last] != '\n' && query->source[last] != '\r') {
        last++;
    }
    return offset <= last + 1;
}
#endif

/* The line that holds `offset`, counted from 0 in the text, and where it
 * starts: from the query's lines, or from the locus for a byte before
 * them, as of a block the cursor's is nested in. */
static size_t query_line(const context_query *query, size_t offset, size_t *start) {
    assert(lines_hold(query, offset));
    if (offset < query->lines.starts[0]) {
        size_t line;
        query->locus->line(query->locus->owner, offset, &line, start);
        return line;
    }
    size_t lo = 0, hi = query->lines.count;
    while (hi - lo > 1) {
        const size_t mid = lo + (hi - lo) / 2;
        if (query->lines.starts[mid] <= offset) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    *start = query->lines.starts[lo];
    return query->lines.base + lo;
}

/* Where the line that holds `offset` starts. */
static size_t line_start(const context_query *query, size_t offset) {
    size_t start;
    query_line(query, offset, &start);
    return start;
}

/* The line holding `offset`, as a 1-based number, and the UTF-16 units from
 * its start to `offset`, counted on from the nearer mark before it; the
 * count is left in mark `which`. */
static size_t query_units(const context_query *query, size_t offset, int32_t *line, size_t which) {
    size_t from, units = 0;
    const size_t at = query_line(query, offset, &from);
    for (size_t i = 0; i < 2; i++) {
        const column_mark *mark = &query->marks[i];
        if (mark->set && mark->line == at && mark->offset <= offset && mark->offset >= from) {
            from = mark->offset;
            units = mark->units;
        }
    }
    for (; from < offset; from++) {
        const uint8_t byte = query->source[from];
        units += (byte & 0xC0) == 0x80 ? 0 : byte >= 0xF0 ? 2 : 1;
    }
    query->marks[which] = (column_mark){at, offset, units, true};
    *line = (int32_t)(at + 1);
    return units;
}

static markdown_core_scope query_scope(const context_query *query, size_t start, size_t end) {
    markdown_core_scope scope;
    if (query->document->unit == MARKDOWN_CORE_TEXT_UNIT_UTF8) {
        size_t from;
        scope.start.line = (int32_t)(query_line(query, start, &from) + 1);
        scope.start.column = (int32_t)(start - from + 1);
        scope.end.line = (int32_t)(query_line(query, end, &from) + 1);
        scope.end.column = (int32_t)(end - from);
        return scope;
    }
    scope.start.column = (int32_t)query_units(query, start, &scope.start.line, 0) + 1;
    scope.end.column = (int32_t)query_units(query, end, &scope.end.line, 1);
    return scope;
}

/* The end of the line that holds `offset`: its terminator, or the source's
 * end. */
static size_t line_end(const context_query *query, size_t offset) {
    while (offset < query->length && query->source[offset] != '\n' && query->source[offset] != '\r') {
        offset++;
    }
    return offset;
}

/* THE CONTEXT FROM THE DOCUMENT (core-element-decoupling.md 9.2). The
 * parse of the whole text keeps on each node the fields it read, and on an
 * inline root, or on the block that took a line, the constructs it began
 * and gave up with the fields they wrote (node.h, OPENER and OPEN): the
 * context reads them at the cursor, with no grammar of its own. A node
 * the window's walk reaches holds the cursor when its source does, an end
 * included: the node a cursor at its end writes on. */
typedef struct {
    const markdown_core_node *node;
    markdown_core_place place;
    /* Where the node's first child starts, or its end for none. */
    uint32_t child;
    /* Where the run that holds the cursor starts. */
    uint32_t lead;
    /* Whether a run of the node holds the cursor, its last's end included:
     * one on
     * a later line of it whose container's syntax holds the cursor (`> `)
     * writes none of its fields. */
    bool runs;
} context_held;

/* What the context gathers before it copies its slots: a slot as the
 * context keeps it, with where its prefix lies in the source. */
typedef struct {
    context_slot slot;
    markdown_core_place prefix;
    bool prefixed;
    /* Where the run of its node that holds the cursor starts. */
    size_t lead;
} context_found;

typedef struct {
    context_held *held;
    size_t held_count, held_capacity;
    markdown_core_opener *inlines;
    size_t inline_count, inline_capacity;
    markdown_core_place *opens;
    size_t open_capacity;
    context_found *found;
    size_t found_count, found_capacity;
    bool failed;
} context_gather;

/* The slot kind whose field, before a field of slot kind `slot` of a
 * construct of kind `owner` in it, the slot's text depends on, as the
 * slot's writer says (write.h, `prefix`); NONE for none. */
static markdown_core_slot_kind prefix_of(markdown_core_slot_kind slot, markdown_core_node_kind owner) {
    const markdown_core_write_ops *writer = markdown_core_writer(slot, owner);
    return writer && writer->prefix ? writer->prefix(slot) : MARKDOWN_CORE_SLOT_NONE;
}

/* Whether the block whose source is `place` holds the cursor: from its
 * start through its end, but for an end past the terminator of its last
 * line, which the next line's text starts at. */
static bool block_holds(const context_query *query, markdown_core_place place) {
    const size_t cursor = query->cursor;
    if (place.start > cursor || place.end < cursor) {
        return false;
    }
    return cursor < place.end || !place.end || place.end > query->length ||
           (query->source[place.end - 1] != '\n' && query->source[place.end - 1] != '\r');
}

static bool node_inline(const markdown_core_node *node) {
    return (node->kind & MARKDOWN_CORE_NODE_TYPE_INLINE) == MARKDOWN_CORE_NODE_TYPE_INLINE;
}

/* Adds an inline opener of kind `kind` at `place`; the same opener added
 * twice is kept once when the openers are sorted (take_inlines). */
static void gather_inline(context_gather *gather, markdown_core_node_kind kind, markdown_core_place place) {
    markdown_core_opener *grown =
        markdown_core_reserve(gather->inlines, &gather->inline_capacity, gather->inline_count + 1, sizeof(*grown));
    if (!grown) {
        gather->failed = true;
        return;
    }
    gather->inlines = grown;
    /* The scope holds the place's offsets until the openers are sorted. */
    grown[gather->inline_count++] = (markdown_core_opener){kind, {{(int32_t)place.start, 0}, {(int32_t)place.end, 0}}};
}

/* Adds the slot of slot kind `kind` of a construct of kind `owner` whose
 * field is `field`, `closed` when the document finishes it, its construct
 * opening at `opener`, and its prefix `prefix` when `prefixed`; once. */
static void gather_slot(context_gather *gather, markdown_core_slot_kind kind, markdown_core_node_kind owner,
                        markdown_core_place field, bool closed, size_t opener, markdown_core_place prefix,
                        bool prefixed, size_t lead) {
    for (size_t i = 0; i < gather->found_count; i++) {
        const context_found *at = &gather->found[i];
        if (at->slot.slot.kind == kind && at->slot.slot.owner == owner && at->slot.field == field.start &&
            at->slot.end == field.end && at->slot.slot.closed == closed) {
            return;
        }
    }
    context_found *grown =
        markdown_core_reserve(gather->found, &gather->found_capacity, gather->found_count + 1, sizeof(*grown));
    if (!grown) {
        gather->failed = true;
        return;
    }
    gather->found = grown;
    /* The offsets are the source's until the slot is cut to the line. */
    const context_slot slot = {.slot = {.kind = kind, .owner = owner, .closed = closed, .prefix_count = prefixed},
                               .field = field.start,
                               .start = field.start,
                               .end = field.end,
                               .opener = opener};
    grown[gather->found_count++] = (context_found){slot, prefix, prefixed, lead};
}

/* Reads the entries of `node`, whose source starts at `base`: its fields
 * that hold the cursor, closed slots, and the constructs it gave up, each
 * an inline opener when it opens before the cursor, read by the inline
 * phase, and each of their fields that holds the cursor an open slot. */
static void gather_node(context_gather *gather, const context_query *query, const markdown_core_node *node,
                        uint32_t base, uint32_t lead) {
    const markdown_core_fields *fields = node->fields;
    const size_t cursor = query->cursor;
    const markdown_core_node_kind kind = markdown_core_node_get_kind(node);
    /* The entries of the construct in hand: its opener, and where its run
     * of OPEN entries started. */
    size_t opener = SIZE_MAX, group = 0;
    for (uint32_t i = 0; fields && i < fields->count && !gather->failed; i++) {
        const markdown_core_written *at = &fields->items[i];
        const markdown_core_place place = {base + at->start, base + at->end};
        if (at->how == MARKDOWN_CORE_FIELD_OPENER) {
            opener = place.start;
            group = i + 1;
            if (!(at->slot & 1) && place.end <= cursor) {
                gather_inline(gather, at->owner, place);
            }
            continue;
        }
        const bool open = at->how == MARKDOWN_CORE_FIELD_OPEN;
        if ((!open && at->how != MARKDOWN_CORE_FIELD) || at->slot == MARKDOWN_CORE_SLOT_NONE || place.start > cursor ||
            place.end < cursor) {
            continue;
        }
        /* Its prefix: the last field before it in its construct of the
         * slot kind it reads after. */
        const markdown_core_slot_kind before = prefix_of(at->slot, open ? at->owner : kind);
        markdown_core_place prefix = {0, 0};
        bool prefixed = false;
        for (uint32_t k = open ? (uint32_t)group : 0; before && k < i; k++) {
            const markdown_core_written *was = &fields->items[k];
            if (was->slot == before && was->how == at->how && (open || was->end > was->start)) {
                prefix = (markdown_core_place){base + was->start, base + was->end};
                prefixed = true;
            }
        }
        gather_slot(gather, at->slot, open ? at->owner : kind, place, !open, open ? opener : SIZE_MAX, prefix, prefixed,
                    lead);
    }
}

static int opener_order(const void *left, const void *right) {
    const markdown_core_opener *a = left, *b = right;
    if (a->scope.start.line != b->scope.start.line) {
        return a->scope.start.line < b->scope.start.line ? -1 : 1;
    }
    if (a->scope.end.line != b->scope.end.line) {
        return a->scope.end.line > b->scope.end.line ? -1 : 1;
    }
    return a->kind < b->kind ? -1 : a->kind > b->kind ? 1 : 0;
}

/* Notes what `opener`, kept, leaves open at the start of the cursor's line
 * when it opens on an earlier one, as each element of the dialect says of
 * its own (write.h, `carry`). */
static void leave(markdown_core_context *context, const context_query *query, markdown_core_node_kind kind,
                  markdown_core_place place) {
    const size_t line = line_start(query, query->cursor);
    if (place.end > line || !place.end) {
        return;
    }
    bool more = true;
    for (size_t i = 0; more; i++) {
        const markdown_core_write_ops *ops = markdown_core_writes(i, &more);
        if (ops && ops->carry) {
            ops->carry(kind, query->source, place.start, place.end, line, &context->carried);
        }
    }
}

/* Reads the window's nodes that hold the cursor, outermost first; false
 * when the walk could not allocate. */
static bool gather_held(context_gather *gather, const context_query *query) {
    markdown_core_walk walk;
    markdown_core_walk_item item;
    markdown_core_walk_within(&walk, query->document->root, query->window);
    /* The node held last, whose first child is the item after it a level
     * below it. */
    size_t level = SIZE_MAX;
    while (!gather->failed && markdown_core_walk_next(&walk, &item)) {
        if (level != SIZE_MAX && item.node && item.level == level + 1) {
            gather->held[gather->held_count - 1].child = item.place.start;
        }
        level = SIZE_MAX;
        if (!item.node || item.place.start > query->cursor || item.place.end < query->cursor) {
            continue;
        }
        level = item.level;
        context_held *grown =
            markdown_core_reserve(gather->held, &gather->held_capacity, gather->held_count + 1, sizeof(*grown));
        if (!grown) {
            gather->failed = true;
            break;
        }
        gather->held = grown;
        bool runs = true;
        uint32_t lead = item.place.start;
        if (item.node->fields) {
            const markdown_core_place *ranges;
            size_t count;
            if (!markdown_core_walk_ranges(&walk, &item, &ranges, &count)) {
                break;
            }
            runs = false;
            for (size_t i = 0; i < count && !runs; i++) {
                /* An end a gap follows is where the next run's line
                 * starts, not the run's. */
                runs = ranges[i].start <= query->cursor &&
                       (query->cursor < ranges[i].end ||
                        (query->cursor == ranges[i].end && (i + 1 == count || ranges[i + 1].start == ranges[i].end)));
                lead = runs ? ranges[i].start : lead;
            }
        }
        grown[gather->held_count++] = (context_held){item.node, item.place, item.place.end, lead, runs};
    }
    const bool failed = walk.failed;
    markdown_core_walk_end(&walk);
    return !failed && !gather->failed;
}

/* The context's blocks: each block that holds the cursor, outermost first,
 * and its opening line from its start to the line's end or the cursor. */
static bool take_blocks(markdown_core_context *context, const context_query *query, const context_gather *gather) {
    context->blocks = gather->held_count ? markdown_core_alloc(gather->held_count, sizeof(*context->blocks)) : NULL;
    if (gather->held_count && !context->blocks) {
        return false;
    }
    /* Blocks nested on one line share its end: a start the last scan
     * passed takes the end it found. */
    size_t from = SIZE_MAX, end = 0;
    for (size_t i = 0; i < gather->held_count; i++) {
        const context_held *held = &gather->held[i];
        if (node_inline(held->node)) {
            /* A block an inline node holds, an inline note, is inline. */
            break;
        }
        if (held->node == query->document->root || !block_holds(query, held->place)) {
            continue;
        }
        const size_t start = held->place.start < query->cursor ? held->place.start : query->cursor;
        if (start < from || start > end) {
            from = start;
            end = line_end(query, start);
        }
        context->blocks[context->block_count++] =
            (markdown_core_opener){markdown_core_node_get_kind(held->node),
                                   query_scope(query, start, end < query->cursor ? end : query->cursor)};
    }
    return true;
}

/* The context's inlines: the inline nodes the cursor is inside, each at its
 * opening syntax, from its start to its first field or child, when it has
 * one -- a text has none -- and the
 * constructs the inline roots that hold it gave up before it, earliest
 * first, and of two at one start the longer first. */
static bool take_inlines(markdown_core_context *context, const context_query *query, context_gather *gather) {
    for (size_t i = 0; i < gather->held_count && !gather->failed; i++) {
        const context_held *held = &gather->held[i];
        if (!node_inline(held->node) || held->place.start >= query->cursor || held->place.end <= query->cursor) {
            continue;
        }
        uint32_t opening = held->child;
        const markdown_core_fields *fields = held->node->fields;
        for (uint32_t k = 0; fields && k < fields->count; k++) {
            const uint32_t at = held->place.start + fields->items[k].start;
            opening = fields->items[k].how < MARKDOWN_CORE_FIELD_OPENER && at < opening ? at : opening;
        }
        if (opening > held->place.start && opening < held->place.end) {
            gather_inline(gather, markdown_core_node_get_kind(held->node),
                          (markdown_core_place){held->place.start, opening});
        }
    }
    if (gather->failed) {
        return false;
    }
    if (gather->inline_count > 1) {
        qsort(gather->inlines, gather->inline_count, sizeof(*gather->inlines), opener_order);
        /* Sorted, an opener added twice is next to itself. */
        size_t kept = 1;
        for (size_t i = 1; i < gather->inline_count; i++) {
            if (opener_order(&gather->inlines[kept - 1], &gather->inlines[i])) {
                gather->inlines[kept++] = gather->inlines[i];
            }
        }
        gather->inline_count = kept;
    }
    for (size_t i = 0; i < gather->inline_count; i++) {
        markdown_core_opener *at = &gather->inlines[i];
        const markdown_core_place place = {(uint32_t)at->scope.start.line, (uint32_t)at->scope.end.line};
        leave(context, query, at->kind, place);
        at->scope = query_scope(query, place.start, place.end);
    }
    context->inlines = gather->inlines;
    context->inline_count = gather->inline_count;
    gather->inlines = NULL;
    return true;
}

/* The context's slots, each cut to the cursor's line, with its prefixes
 * copied: a field that began on an earlier line, a label or a quoted
 * value, is written from past the line's container prefix, and one that
 * goes on to a later line up to the line's end. */
static bool take_slots(markdown_core_context *context, const context_query *query, const context_gather *gather) {
    if (!gather->found_count) {
        return true;
    }
    size_t line = query->cursor;
    const size_t last = line_end(query, query->cursor);
    while (line > 0 && query->source[line - 1] != '\n' && query->source[line - 1] != '\r') {
        line--;
    }
    size_t bytes = 0, prefixes = 0;
    for (size_t i = 0; i < gather->found_count; i++) {
        if (gather->found[i].prefixed) {
            bytes += gather->found[i].prefix.end - gather->found[i].prefix.start;
            prefixes++;
        }
    }
    context->slots = markdown_core_alloc(gather->found_count, sizeof(*context->slots));
    context->prefixes = prefixes ? markdown_core_alloc(prefixes, sizeof(*context->prefixes)) : NULL;
    context->bytes = bytes ? markdown_core_alloc(bytes, 1) : NULL;
    context->size = last - line;
    context->line = markdown_core_alloc(context->size + 1, 1);
    if (!context->slots || (prefixes && !context->prefixes) || (bytes && !context->bytes) || !context->line) {
        return false;
    }
    memcpy(context->line, query->source + line, context->size);
    size_t prefix = 0, written = 0;
    for (size_t i = 0; i < gather->found_count; i++) {
        const context_found *found = &gather->found[i];
        const size_t field = found->slot.field, stop = found->slot.end;
        /* A field that began on an earlier line is written from where its
         * node's run on the line starts, past the line's container prefix. */
        const size_t lead = found->lead > line ? found->lead : line;
        const size_t start = field >= line ? field : lead < query->cursor ? lead : query->cursor;
        const size_t end = stop < last ? stop : last;
        const size_t after = end > query->cursor ? end : query->cursor;
        const markdown_core_slot_kind kind = found->slot.slot.kind;
        const markdown_core_node_kind owner = found->slot.slot.owner;
        context_slot *slot = &context->slots[context->slot_count++];
        *slot = (context_slot){
            {kind, owner, query_scope(query, start, query->cursor), query_scope(query, query->cursor, after),
             found->slot.slot.closed, found->prefixed ? 1 : 0},
            prefix,
            field < line ? SIZE_MAX : field - line,
            start - line,
            query->cursor - line,
            after - line,
            found->slot.opener >= line && found->slot.opener < last ? found->slot.opener - line : SIZE_MAX,
            0,
            field < line || (found->slot.slot.closed && stop > last)};
        /* The delimiter its source opens with and does not hold, as its
         * writer reads it (write.h, `wrap`). */
        const markdown_core_write_ops *writer = markdown_core_writer(kind, owner);
        if (writer && writer->wrap) {
            const markdown_core_wrap_site at = {query->source,       line,           field, start,
                                                found->prefix.start, found->prefixed};
            slot->wrap = writer->wrap(kind, owner, &at);
        }
        if (found->prefixed) {
            const size_t size = found->prefix.end - found->prefix.start;
            if (size) {
                memcpy(context->bytes + written, query->source + found->prefix.start, size);
            }
            context->prefixes[prefix++] = (markdown_core_string){context->bytes + written, size};
            written += size;
        }
    }
    return true;
}

/* THE ENVELOPE'S CONTEXT: a node the walk of the tree does not reach, the
 * document's properties envelope, whose element reads the context at the
 * cursor itself (element.h, the facet's `envelope`): when it holds the
 * cursor, its block is the envelope's node, on its opening line, and its
 * slot the one the element finds. `*inside` answers whether it holds the
 * cursor; false when an allocation failed. */
static bool take_envelope(markdown_core_context *context, const context_query *query, bool *inside) {
    const markdown_core_node *node = query->document->root->as.document->metadata;
    *inside = false;
    const markdown_core_facet *facet = node ? markdown_core_core_facet(markdown_core_node_get_kind(node)) : NULL;
    markdown_core_envelope found = {0};
    if (!facet || !facet->envelope) {
        return true;
    }
    if (!facet->envelope(node, query->source, query->length, query->cursor, &found, inside)) {
        return false;
    }
    if (!*inside) {
        return true;
    }
    const size_t cursor = query->cursor;
    context->blocks = markdown_core_alloc(1, sizeof(*context->blocks));
    if (!context->blocks) {
        return false;
    }
    context->blocks[context->block_count++] = (markdown_core_opener){
        markdown_core_node_get_kind(node), query_scope(query, 0, cursor < found.opening ? cursor : found.opening)};
    if (!found.slot) {
        return true;
    }
    const size_t line = line_start(query, cursor), last = line_end(query, cursor);
    context->slots = markdown_core_alloc(1, sizeof(*context->slots));
    context->size = last - line;
    context->line = markdown_core_alloc(context->size + 1, 1);
    if (!context->slots || !context->line) {
        return false;
    }
    memcpy(context->line, query->source + line, context->size);
    context->slots[context->slot_count++] =
        (context_slot){{found.kind, markdown_core_node_get_kind(node), query_scope(query, found.from, cursor),
                        query_scope(query, cursor, found.to), found.closed, 0},
                       0,
                       found.field < line ? SIZE_MAX : found.field - line,
                       found.from - line,
                       cursor - line,
                       found.to - line,
                       SIZE_MAX,
                       found.wrap,
                       false};
    return true;
}

/* THE CONTEXT AT A CURSOR, read from the document (THE CONTEXT FROM THE
 * DOCUMENT) in `locus`: its lines and its window of the tree, which a
 * session keeps to the blocks that hold the cursor's line, so that the
 * query's cost keeps to them however long the document is; a locus of no
 * window reads the whole tree. */
static markdown_core_status context_at(const markdown_core_document *document, const uint8_t *source, size_t length,
                                       markdown_core_position position, const markdown_core_locus *locus,
                                       markdown_core_context **context) {
    if (position.line < 1 || position.column < 1) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    column_mark marks[2] = {{0}};
    context_query query = {.document = document,
                           .source = source,
                           .length = length,
                           .marks = marks,
                           .locus = locus,
                           .window = locus->window.depth ? &locus->window : NULL};
    query.lines = locus->lines;
    if (!markdown_core_position_offset(&query.lines, source, length, position, document->unit, true, &query.cursor)) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    markdown_core_context *made = markdown_core_alloc(1, sizeof(*made));
    bool inside = false;
    bool ok = made && take_envelope(made, &query, &inside);
    context_gather gather = {0};
    if (ok && !inside) {
        ok = gather_held(&gather, &query);
        for (size_t i = 0; ok && i < gather.held_count; i++) {
            if (gather.held[i].runs) {
                gather_node(&gather, &query, gather.held[i].node, gather.held[i].place.start, gather.held[i].lead);
            }
            made->row = made->row || markdown_core_node_get_kind(gather.held[i].node) == MARKDOWN_CORE_KIND_TABLE_CELL;
        }
        ok = ok && !gather.failed && take_blocks(made, &query, &gather) && take_inlines(made, &query, &gather) &&
             take_slots(made, &query, &gather);
    }
    markdown_core_free(gather.held);
    markdown_core_free(gather.inlines);
    markdown_core_free(gather.found);
    if (!ok) {
        markdown_core_context_free(made);
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    *context = made;
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_document_context(const markdown_core_document *document, const uint8_t *source,
                                                    size_t length, markdown_core_position position,
                                                    markdown_core_context **context) {
    markdown_core_locus locus = {0};
    if (!markdown_core_source_lines_read(&locus.lines, source, length)) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    const markdown_core_status status = context_at(document, source, length, position, &locus, context);
    markdown_core_free(locus.lines.starts);
    return status;
}

size_t markdown_core_context_block_count(const markdown_core_context *context) { return context->block_count; }

markdown_core_status markdown_core_context_block_at(const markdown_core_context *context, size_t index,
                                                    markdown_core_opener *block) {
    if (index >= context->block_count) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    *block = context->blocks[index];
    return MARKDOWN_CORE_OK;
}

size_t markdown_core_context_inline_count(const markdown_core_context *context) { return context->inline_count; }

markdown_core_status markdown_core_context_inline_at(const markdown_core_context *context, size_t index,
                                                     markdown_core_opener *opener) {
    if (index >= context->inline_count) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    *opener = context->inlines[index];
    return MARKDOWN_CORE_OK;
}

size_t markdown_core_context_slot_count(const markdown_core_context *context) { return context->slot_count; }

markdown_core_status markdown_core_context_slot_at(const markdown_core_context *context, size_t index,
                                                   markdown_core_slot *slot) {
    if (index >= context->slot_count) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    *slot = context->slots[index].slot;
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_context_slot_prefix_at(const markdown_core_context *context, size_t slot,
                                                          size_t index, markdown_core_string *prefix) {
    if (slot >= context->slot_count || index >= context->slots[slot].slot.prefix_count) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    *prefix = context->prefixes[context->slots[slot].prefix + index];
    return MARKDOWN_CORE_OK;
}

void markdown_core_context_clear(markdown_core_context *context) {
    markdown_core_free(context->blocks);
    markdown_core_free(context->inlines);
    markdown_core_free(context->slots);
    markdown_core_free(context->prefixes);
    markdown_core_free(context->bytes);
    markdown_core_free(context->line);
    *context = (markdown_core_context){0};
}

void markdown_core_context_free(markdown_core_context *context) {
    if (!context) {
        return;
    }
    markdown_core_context_clear(context);
    markdown_core_free(context);
}

/* THE SESSION'S CONTEXT: the state a session keeps of the cursor it was
 * last located at. A locate reads a fresh context and moves what it holds
 * into the state, so the state stays where it is and a failed locate leaves
 * it whole. */
markdown_core_context *markdown_core_context_new(void) { return markdown_core_alloc(1, sizeof(markdown_core_context)); }

markdown_core_status markdown_core_context_read(const markdown_core_document *document, const uint8_t *source,
                                                size_t length, markdown_core_position position,
                                                const markdown_core_locus *locus, markdown_core_context **context) {
    return context_at(document, source, length, position, locus, context);
}

void markdown_core_context_take(markdown_core_context *context, markdown_core_context *fresh) {
    markdown_core_context_clear(context);
    *context = *fresh;
    markdown_core_free(fresh);
}

/* SPELLING (docs/plans/2026-10-23-lsp-completion-engine.md, 3.1;
 * core-element-decoupling.md 9.9): the text that writes a value in a slot's
 * field, in the field's own syntax, and the closer that finishes a construct
 * the document leaves open. The element that writes the slot's kind spells
 * it (write.h, markdown_core_writer), and every element of the dialect may
 * refuse a spelling that changes a construct of its own on the line. */

/* Whether `value` is well formed UTF-8 with no line ending: no field holds
 * one, since a slot keeps to its line. */
static bool readable(const uint8_t *value, size_t size) {
    for (size_t i = 0; i < size;) {
        const uint8_t c = value[i];
        if (c == '\n' || c == '\r') {
            return false;
        }
        const size_t width = c < 0x80             ? 1
                             : (c & 0xE0) == 0xC0 ? 2
                             : (c & 0xF0) == 0xE0 ? 3
                             : (c & 0xF8) == 0xF0 ? 4
                                                  : 0;
        if (!width || i + width > size || (width == 2 && c < 0xC2) || (width == 4 && c > 0xF4)) {
            return false;
        }
        for (size_t j = 1; j < width; j++) {
            if ((value[i + j] & 0xC0) != 0x80) {
                return false;
            }
        }
        if ((width == 3 && c == 0xE0 && value[i + 1] < 0xA0) || (width == 3 && c == 0xED && value[i + 1] >= 0xA0) ||
            (width == 4 && c == 0xF0 && value[i + 1] < 0x90) || (width == 4 && c == 0xF4 && value[i + 1] >= 0x90)) {
            return false;
        }
        i += width;
    }
    return true;
}

markdown_core_status
markdown_core_write_reads(const markdown_core_site *site, const markdown_core_spelling *spelled,
                          bufsize_t (*end)(uint8_t before, const unsigned char *data, bufsize_t length), bool *whole) {
    const size_t rest = markdown_core_write_rest(site);
    uint8_t *data = markdown_core_alloc(spelled->size + rest + 1, 1);
    if (!data) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    memcpy(data, spelled->text, spelled->size);
    if (rest) {
        memcpy(data + spelled->size, site->line + site->end, rest);
    }
    const uint8_t before = site->start > 0 ? site->line[site->start - 1] : 0;
    const bufsize_t stop = end(before, data, (bufsize_t)(spelled->size + rest));
    markdown_core_free(data);
    *whole = stop < 0 || (size_t)stop == spelled->size;
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_slot_spell(const markdown_core_context *context, size_t index, const uint8_t *value,
                                              size_t size, markdown_core_insertion **insertion) {
    *insertion = NULL;
    if (index >= context->slot_count) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    const context_slot *slot = &context->slots[index];
    const markdown_core_write_ops *writer = markdown_core_writer(slot->slot.kind, slot->slot.owner);
    if (!readable(value, size) || slot->split || !writer || !writer->spell) {
        return MARKDOWN_CORE_OK;
    }
    const markdown_core_site site = {context->line, context->size, slot->slot.kind, slot->slot.owner, slot->slot.closed,
                                     context->row,  slot->field,   slot->start,     slot->cursor,     slot->end,
                                     slot->opener,  slot->wrap,    context->carried};
    /* An escape doubles a byte, a reference's `&` takes five, and quotes,
     * braces and the closer a few more. */
    markdown_core_insertion *made = markdown_core_alloc(1, sizeof(*made) + 8 * size + 64);
    if (!made) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    markdown_core_spelling out = {(uint8_t *)(made + 1), 0, "", MARKDOWN_CORE_TOKENS_FREE};
    bool held = false;
    markdown_core_status status = writer->spell(&site, value, size, &out, &held);
    /* The spelling must leave the constructs around the slot as they are:
     * every element of the dialect is asked of its own. */
    bool more = !writer->alone;
    for (size_t i = 0; status == MARKDOWN_CORE_OK && held && more; i++) {
        const markdown_core_write_ops *ops = markdown_core_writes(i, &more);
        if (ops && ops->keeps) {
            status = ops->keeps(&site, value, size, &out, &held);
        }
    }
    if (status != MARKDOWN_CORE_OK || !held) {
        markdown_core_free(made);
        return status;
    }
    made->text = out.text;
    made->cursor = out.size;
    for (const char *c = out.closer; c && *c; c++) {
        markdown_core_write_put(&out, (uint8_t)*c);
    }
    made->size = out.size;
    *insertion = made;
    return MARKDOWN_CORE_OK;
}

void markdown_core_insertion_free(markdown_core_insertion *insertion) { markdown_core_free(insertion); }
