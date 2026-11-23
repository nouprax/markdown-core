#include "heading.h"
#include "markdown-core-elements.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "../include/markdown_core.h"

#include "ast_internal.h"
#include "parts.h"
#include "properties.h"
#include "attributes.h"
#include "../core/attributes.h"

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
     * start: a directive's label, whose scan stops at any `]`, and the
     * quote of an attribute block's value. */
    bool label;
    uint8_t quote;
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

static bool among(uint8_t c, const char *set) { return c && strchr(set, c); }

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

/* The slot kind a field of slot kind `slot` reads after it in the same
 * construct, whose text the slot depends on: an anchor its destination's
 * path, a value its member's key; NONE for none. */
static markdown_core_slot_kind prefix_of(markdown_core_slot_kind slot) {
    switch (slot) {
    case MARKDOWN_CORE_SLOT_ANCHOR:
        return MARKDOWN_CORE_SLOT_DEST;
    case MARKDOWN_CORE_SLOT_VALUE:
        return MARKDOWN_CORE_SLOT_RECORD;
    default:
        return MARKDOWN_CORE_SLOT_NONE;
    }
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
                        bool prefixed) {
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
    grown[gather->found_count++] = (context_found){slot, prefix, prefixed};
}

/* Reads the entries of `node`, whose source starts at `base`: its fields
 * that hold the cursor, closed slots, and the constructs it gave up, each
 * an inline opener when it opens before the cursor, read by the inline
 * phase, and each of their fields that holds the cursor an open slot. */
static void gather_node(context_gather *gather, const context_query *query, const markdown_core_node *node,
                        uint32_t base) {
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
        const markdown_core_slot_kind before = prefix_of(at->slot);
        markdown_core_place prefix = {0, 0};
        bool prefixed = false;
        for (uint32_t k = open ? (uint32_t)group : 0; before && k < i; k++) {
            const markdown_core_written *was = &fields->items[k];
            if (was->slot == before && was->how == at->how && (open || was->end > was->start)) {
                prefix = (markdown_core_place){base + was->start, base + was->end};
                prefixed = true;
            }
        }
        gather_slot(gather, at->slot, open ? at->owner : kind, place, !open, open ? opener : SIZE_MAX, prefix,
                    prefixed);
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
 * when it opens on an earlier one: a directive's label, or a quote of its
 * attribute block that no quote of its kind closes before the line. */
static void leave(markdown_core_context *context, const context_query *query, markdown_core_node_kind kind,
                  markdown_core_place place) {
    const size_t line = line_start(query, query->cursor);
    if (place.end > line || !place.end) {
        return;
    }
    const uint8_t c = query->source[place.end - 1];
    /* A `[` still open to more than one kind is a directive's label after
     * a `:` and a name. */
    size_t name = place.end - 1;
    while (name > 0 && !among(query->source[name - 1], " \t\r\n[{:")) {
        name--;
    }
    if (c == '[' && (kind == MARKDOWN_CORE_KIND_DIRECTIVE || (kind == MARKDOWN_CORE_KIND_NONE && name + 1 < place.end &&
                                                              name > 0 && query->source[name - 1] == ':'))) {
        context->label = true;
    } else if (c == '{' && kind == MARKDOWN_CORE_KIND_NONE) {
        uint8_t quote = 0;
        for (size_t at = place.end; at < line; at++) {
            const uint8_t b = query->source[at];
            if (b == '\\') {
                at++;
            } else if (quote ? b == quote : b == '"' || b == '\'') {
                quote = quote ? 0 : b;
            }
        }
        context->quote = quote ? quote : context->quote;
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
            }
        }
        grown[gather->held_count++] = (context_held){item.node, item.place, item.place.end, runs};
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
    size_t from = line;
    while (from < query->cursor &&
           (query->source[from] == ' ' || query->source[from] == '\t' || query->source[from] == '>')) {
        from++;
    }
    size_t prefix = 0, written = 0;
    for (size_t i = 0; i < gather->found_count; i++) {
        const context_found *found = &gather->found[i];
        const size_t field = found->slot.field, stop = found->slot.end;
        const size_t start = field < line ? from : field;
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
        /* The delimiter its source opens with and does not hold. */
        const size_t origin = kind == MARKDOWN_CORE_SLOT_ANCHOR && found->prefixed ? found->prefix.start : field;
        if ((kind == MARKDOWN_CORE_SLOT_DEST || kind == MARKDOWN_CORE_SLOT_ANCHOR) && origin > line &&
            query->source[origin - 1] == '<' && owner != MARKDOWN_CORE_KIND_CROSS_LINK &&
            owner != MARKDOWN_CORE_KIND_CROSS_EMBEDDED) {
            slot->wrap = '<';
        } else if ((kind == MARKDOWN_CORE_SLOT_BIB || kind == MARKDOWN_CORE_SLOT_SPECIMEN) && start > line + 1 &&
                   query->source[start - 1] == '{' && query->source[start - 2] == '@') {
            slot->wrap = '{';
        } else if (kind == MARKDOWN_CORE_SLOT_VALUE && field > 0 &&
                   (query->source[field - 1] == '"' || query->source[field - 1] == '\'')) {
            /* A quoted value's field is inside its quotes. */
            slot->wrap = query->source[field - 1];
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

/* Whether `c` is a byte of a member's name as the field names spell it. */
static bool name_byte(uint8_t c) {
    return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || c == '_' || c == '-';
}

/* Whether the bytes from `from` to `to` are spaces and tabs. */
static bool blanks(const context_query *query, size_t from, size_t to) {
    for (size_t at = from; at < to; at++) {
        if (query->source[at] != ' ' && query->source[at] != '\t') {
            return false;
        }
    }
    return true;
}

/* The slot of the envelope's member at the cursor, from its places (plan
 * 3.2): `field` on a name, `value` on a value. A scalar on one line holds
 * the cursor from its first byte to its last, inside its quotes; a
 * literal's line from past the literal's indentation to the line's end.
 * After a name's colon and a space, where the member reads an empty value,
 * the empty value's slot holds it when no more than spaces and a comment
 * follow on the line. Each of these the parse confirms, so it is closed.
 * At a line that begins a member and holds no name the parse reads, the
 * name characters before the cursor are an open `field`: the member names
 * no field yet. A line a member holds (properties.h) begins none. Answers whether it found a slot, as `*kind`, `*from`,
 * `*to`, `*field` (where its field starts), `*wrap` and `*closed`. */
static bool member_slot(const context_query *query, const markdown_core_metadata_places *places, size_t line,
                        size_t last, markdown_core_slot_kind *kind, size_t *from, size_t *to, size_t *field,
                        uint8_t *wrap, bool *closed) {
    const size_t cursor = query->cursor;
    *closed = true;
    for (size_t i = 0; i < places->count; i++) {
        const markdown_core_metadata_place *at = &places->items[i];
        if (at->slot == MARKDOWN_CORE_SLOT_NONE) {
            continue;
        }
        *kind = at->slot;
        *field = at->from;
        *wrap = at->style == MARKDOWN_CORE_YAML_QUOTE        ? '"'
                : at->style == MARKDOWN_CORE_YAML_APOSTROPHE ? '\''
                : at->style == MARKDOWN_CORE_YAML_LITERAL    ? '|'
                                                             : 0;
        if (at->style == MARKDOWN_CORE_YAML_LITERAL) {
            /* Its first line's text starts past the indentation every
             * line of it has. */
            const size_t first = line_start(query, at->from);
            const size_t start = line == first ? at->from : line + (at->from - first);
            if (line >= first && line <= at->to && start <= last && start <= cursor && blanks(query, line, start)) {
                *from = start;
                *to = last;
                return true;
            }
            continue;
        }
        if (at->style == MARKDOWN_CORE_YAML_PLAIN && at->from == at->to) {
            /* An empty value, read where the name's colon ends: the value
             * written after a space takes its place. */
            size_t rest = cursor;
            while (rest < last && (query->source[rest] == ' ' || query->source[rest] == '\t')) {
                rest++;
            }
            if (at->from >= line && at->from < cursor && blanks(query, at->from, cursor) &&
                (rest == last || (rest > cursor && query->source[rest] == '#'))) {
                *from = *to = cursor;
                return true;
            }
            continue;
        }
        if (at->from >= line && at->to <= last && at->from <= cursor && cursor <= at->to) {
            *from = at->from;
            *to = at->to;
            return true;
        }
    }
    /* A line that begins a member, the bytes before the cursor and after it
     * a name: a member the parse skipped begins at the line, or the line is
     * empty, and no value goes on through it. */
    size_t start = line;
    while (start < cursor && name_byte(query->source[start])) {
        start++;
    }
    size_t end = cursor;
    while (end < last && name_byte(query->source[end])) {
        end++;
    }
    bool begins = line == last;
    for (size_t i = 0; i < places->count; i++) {
        const markdown_core_metadata_place *at = &places->items[i];
        begins = (begins || (at->slot == MARKDOWN_CORE_SLOT_NONE && at->from == line && at->to > line)) &&
                 !(at->slot == MARKDOWN_CORE_SLOT_VALUE && at->from < line && line <= at->to);
    }
    if (start != cursor || !begins || places->held || (end > line && query->source[line] == '-')) {
        return false;
    }
    *kind = MARKDOWN_CORE_SLOT_FIELD;
    *from = line;
    *to = end;
    *field = line;
    *wrap = 0;
    *closed = false;
    return true;
}

/* THE ENVELOPE'S CONTEXT. A cursor in a properties envelope, from its
 * opening fence through its closing one, is in the document's Metadata, not
 * in the blocks its lines would be without the fences: its block is the
 * Metadata, on the opening fence's line. In the payload, past the opening
 * fence's line and before the closing fence's, its slot is the member's
 * (member_slot), which the places' reader reads. An envelope that no fence
 * closes is no envelope. `*inside` answers whether the cursor is in one;
 * false when an allocation failed. */
static bool take_envelope(markdown_core_context *context, const context_query *query, bool *inside) {
    const markdown_core_node *metadata = query->document->root->as.document->metadata;
    *inside = false;
    if (!metadata || metadata->place.end < 6 || metadata->place.end > query->length) {
        return true;
    }
    const size_t close = metadata->place.end - 3, cursor = query->cursor;
    const size_t payload = query->lines.count > 1 ? query->lines.starts[1] : close;
    if (cursor > metadata->place.end) {
        return true;
    }
    *inside = true;
    context->blocks = markdown_core_alloc(1, sizeof(*context->blocks));
    if (!context->blocks) {
        return false;
    }
    const size_t opening = line_end(query, 0);
    context->blocks[context->block_count++] =
        (markdown_core_opener){MARKDOWN_CORE_KIND_METADATA, query_scope(query, 0, cursor < opening ? cursor : opening)};
    if (cursor < payload || cursor >= close) {
        return true;
    }
    const size_t line = line_start(query, cursor), last = line_end(query, cursor);
    markdown_core_metadata_places places = {.line = line};
    if (!markdown_core_metadata_places_read(query->source, metadata->place.end, &places)) {
        markdown_core_free(places.items);
        return false;
    }
    markdown_core_slot_kind kind;
    size_t from, to, field;
    uint8_t wrap;
    bool closed;
    const bool found = member_slot(query, &places, line, last, &kind, &from, &to, &field, &wrap, &closed);
    markdown_core_free(places.items);
    if (!found) {
        return true;
    }
    context->slots = markdown_core_alloc(1, sizeof(*context->slots));
    context->size = last - line;
    context->line = markdown_core_alloc(context->size + 1, 1);
    if (!context->slots || !context->line) {
        return false;
    }
    memcpy(context->line, query->source + line, context->size);
    context->slots[context->slot_count++] =
        (context_slot){{kind, MARKDOWN_CORE_KIND_METADATA, query_scope(query, from, cursor),
                        query_scope(query, cursor, to), closed, 0},
                       0,
                       field < line ? SIZE_MAX : field - line,
                       from - line,
                       cursor - line,
                       to - line,
                       SIZE_MAX,
                       wrap,
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
                gather_node(&gather, &query, gather.held[i].node, gather.held[i].place.start);
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

/* The bytes after which a slot may begin empty (markdown_core_slot_cues).
 * The context oracle finds each of them, and no other, before a slot that
 * begins at the cursor. */
static const char CUES[] = "\t\n\r !\"#'(,.:;<=@[^`{~";

markdown_core_string markdown_core_slot_cues(void) {
    return (markdown_core_string){(const uint8_t *)CUES, sizeof(CUES) - 1};
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

/* SPELLING (docs/plans/2026-10-23-lsp-completion-engine.md, 3.1): the text
 * that writes a value in a slot's field, in the field's own syntax, and the
 * closer that finishes a construct the document leaves open. */

/* The text being written, in a buffer the value's size bounds. */
typedef struct {
    uint8_t *text;
    size_t size;
} spelled;

static const char PUNCTUATION[] = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";

static void put(spelled *out, uint8_t c) { out->text[out->size++] = c; }

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

/* Whether `&` at `at` begins what reads as a character reference. */
static bool referent(const uint8_t *value, size_t size, size_t at) {
    size_t i = at + 1;
    i += i < size && value[i] == '#';
    const size_t first = i;
    while (i < size &&
           ((value[i] >= '0' && value[i] <= '9') || ((value[i] | 0x20) >= 'a' && (value[i] | 0x20) <= 'z'))) {
        i++;
    }
    return i > first && i < size && value[i] == ';';
}

/* Writes `value` as it is, false when it holds a byte of `barred`, or a
 * `|` on a table's row, which would split its cell. */
static bool plain(spelled *out, const uint8_t *value, size_t size, const char *barred, bool row) {
    for (size_t i = 0; i < size; i++) {
        if (among(value[i], barred) || (row && value[i] == '|')) {
            return false;
        }
        put(out, value[i]);
    }
    return true;
}

/* Writes `value` with a backslash before each byte of `marked` and before
 * a `|` on a table's row, whose own backslash the row removes first, and an
 * `&` that would begin a character reference as one, `&amp;`: the field
 * reads references before escapes. */
static void escaped(spelled *out, const uint8_t *value, size_t size, const char *marked, bool row) {
    for (size_t i = 0; i < size; i++) {
        const uint8_t c = value[i];
        if (c == '&' && referent(value, size, i)) {
            for (const char *e = "&amp;"; *e; e++) {
                put(out, (uint8_t)*e);
            }
            continue;
        }
        if ((row && c == '|') || among(c, marked)) {
            put(out, '\\');
        }
        put(out, c);
    }
}

/* Whether a backslash in `value` ends it or a bracket stands in it that no
 * backslash escapes: a raw label holds neither. */
static bool bracketed(const uint8_t *value, size_t size) {
    bool odd = false;
    for (size_t i = 0; i < size; i++) {
        if (!odd && (value[i] == '[' || value[i] == ']')) {
            return true;
        }
        odd = value[i] == '\\' && !odd;
    }
    return odd;
}

static bool blank_value(const uint8_t *value, size_t size) {
    for (size_t i = 0; i < size; i++) {
        if (value[i] != ' ' && value[i] != '\t') {
            return false;
        }
    }
    return true;
}

/* Whether every byte of `value` is an ASCII letter, a numeral or one of
 * `also`. */
static bool word(const uint8_t *value, size_t size, const char *also) {
    for (size_t i = 0; i < size; i++) {
        const uint8_t c = value[i];
        if (!((c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || among(c, also))) {
            return false;
        }
    }
    return size > 0;
}

/* The character of the fence the line opens, a backtick or a tilde, or 0:
 * the first run of three before `before`. */
static uint8_t fence_of(const markdown_core_context *context, size_t before) {
    for (size_t i = 0; i + 2 < before && i + 2 < context->size; i++) {
        const uint8_t c = context->line[i];
        if ((c == '`' || c == '~') && context->line[i + 1] == c && context->line[i + 2] == c) {
            return c;
        }
    }
    return 0;
}

/* A destination or its fragment: inside `<…>` as written, bare when the
 * value allows it, otherwise in `<…>` when the slot may write them. */
static bool spell_destination(spelled *out, const context_slot *slot, bool row, const uint8_t *value, size_t size,
                              const uint8_t *tail, size_t rest) {
    const bool fragment = slot->slot.kind == MARKDOWN_CORE_SLOT_ANCHOR;
    if (slot->wrap == '<') {
        escaped(out, value, size, fragment ? "\\<>()[]{}" : "\\<>()[]{}#", row);
        return true;
    }
    bool bare = size > 0 && value[0] != '<';
    for (size_t i = 0; i < size && bare; i++) {
        bare = value[i] > ' ' && value[i] != 0x7F;
    }
    if (bare) {
        /* A destination's `#` would begin its fragment. */
        escaped(out, value, size, fragment ? "\\()<>`$%[]{}" : "\\()<>`$%[]{}#", row);
        return true;
    }
    /* `<...>` is the whole destination: it stands only where the field
     * ends with it, before the call's `)` or a space. */
    if (fragment || (rest && tail[0] != ')' && tail[0] != ' ' && tail[0] != '\t')) {
        return false;
    }
    for (size_t i = 0; i < size; i++) {
        if (value[i] < ' ' && value[i] != '\t') {
            return false;
        }
    }
    put(out, '<');
    escaped(out, value, size, "\\<>()[]{}", row);
    put(out, '>');
    return true;
}

/* An attribute value: bare when nothing in it ends or opens a quoted one,
 * otherwise double quoted; inside the quotes of one already begun. */
static bool spell_attribute(spelled *out, const context_slot *slot, const uint8_t *line, bool row, const uint8_t *value,
                            size_t size) {
    if (slot->wrap) {
        /* Inside its quotes: the closing one is written when the document
         * does not close the value. */
        const char marked[] = {'\\', (char)slot->wrap, '`', '$', '<', '>', '[', ']', '(', ')', '{', '}', 0};
        escaped(out, value, size, marked, row);
        if (!slot->slot.closed) {
            put(out, slot->wrap);
        }
        return true;
    }
    /* A value keeps the quotes it has, since what holds the attributes may
     * read them; one without is bare when nothing in it needs quotes. */
    const uint8_t had = line[slot->start] == '"' || line[slot->start] == '\'' ? line[slot->start] : 0;
    bool bare = !had && size > 0 && value[0] != '"' && value[0] != '\'';
    for (size_t i = 0; i < size && bare; i++) {
        bare = value[i] != ' ' && value[i] != '\t' && value[i] != '&';
    }
    if (bare) {
        escaped(out, value, size, "\\`$<>[](){}", row);
        return true;
    }
    const uint8_t quote = had ? had : '"';
    const char marked[] = {'\\', (char)quote, '`', '$', '<', '>', '[', ']', '(', ')', '{', '}', 0};
    put(out, quote);
    escaped(out, value, size, marked, row);
    put(out, quote);
    return true;
}

/* A key as written: braces in it balanced, and no space, which ends it. */
static bool spell_cite(spelled *out, bool row, const uint8_t *value, size_t size) {
    int depth = 0;
    for (size_t i = 0; i < size; i++) {
        depth += value[i] == '{' ? 1 : value[i] == '}' ? -1 : 0;
        if (depth < 0) {
            return false;
        }
    }
    return size > 0 && !depth && plain(out, value, size, " \t", row);
}

/* Whether `value` holds a byte that opens or closes a literal token, which
 * owns its whole extent whatever field it stands in: a code span's
 * backtick, a formula's `$`, an HTML tag's or an autolink's in_markup bracket,
 * or a comment's `%%`. A field that keeps its bytes as written cannot hold
 * one, since the text around it may pair with it. */
static bool opaque(const uint8_t *value, size_t size) {
    for (size_t i = 0; i < size; i++) {
        if (among(value[i], "`$<>") || (value[i] == '%' && i + 1 < size && value[i + 1] == '%')) {
            return true;
        }
    }
    return false;
}

/* Whether `value` holds a literal token's closer no escape keeps from
 * closing one the text before it opens: a code span's backtick, a
 * formula's `$`, a comment's `%%`. */
static bool closes(const uint8_t *value, size_t size) {
    for (size_t i = 0; i < size; i++) {
        if (value[i] == '`' || value[i] == '$' || (value[i] == '%' && i + 1 < size && value[i + 1] == '%')) {
            return true;
        }
    }
    return false;
}

/* Whether the line before the slot, after a directive's label an earlier
 * line leaves open, leaves an `open` that no `close` matches: a construct
 * around the slot that a `close` would end. */
static bool opened(const markdown_core_context *context, const context_slot *slot, uint8_t open, uint8_t close) {
    size_t depth = open == '[' && context->label;
    for (size_t i = 0; i < slot->start && i < context->size; i++) {
        const uint8_t c = context->line[i];
        if (c == '\\') {
            i++;
        } else if (c == open) {
            depth++;
        } else if (c == close && depth) {
            depth--;
        }
    }
    return depth > 0;
}

/* Whether the owner's own reading of the field (markdown_core_facet,
 * field_end), from `text` with the line's `tail` after it, ends where `text`
 * does; `whole` is left set when the owner reads no field of its own. A
 * bracket the slot stands in bounds what is read: the reading stops at the
 * `]` that closes it. */
static bool scans(const markdown_core_context *context, const context_slot *slot, const spelled *text,
                  const uint8_t *tail, size_t rest, bool *whole) {
    *whole = true;
    if (opened(context, slot, '[', ']')) {
        size_t depth = 0, i = 0;
        for (; i < rest && (tail[i] != ']' || depth); i++) {
            if (tail[i] == '\\') {
                i++;
            } else {
                depth += tail[i] == '[';
                depth -= tail[i] == ']';
            }
        }
        rest = i < rest ? i : rest;
    }
    const markdown_core_facet *facet = markdown_core_core_facet(slot->slot.owner);
    if (!facet || !facet->field_end) {
        return true;
    }
    uint8_t *data = markdown_core_alloc(text->size + rest + 1, 1);
    if (!data) {
        return false;
    }
    memcpy(data, text->text, text->size);
    if (rest) {
        memcpy(data + text->size, tail, rest);
    }
    const uint8_t before = slot->start > 0 ? context->line[slot->start - 1] : 0;
    const bufsize_t end = facet->field_end(slot->slot.kind, before, data, (bufsize_t)(text->size + rest));
    markdown_core_free(data);
    *whole = end < 0 || (size_t)end == text->size;
    return true;
}

/* Whether the spelling would end a construct the line before the slot
 * leaves open: a bracket or a parenthesis it does not match itself, or a
 * brace or the `=` that makes a braced name a key, which a name's or a
 * bare value's scan reads escaped or not, or what ends a cross link's
 * part, an autolink or a tag, or the quote of a value an earlier line
 * leaves open. */
static bool ends(const markdown_core_context *context, const context_slot *slot, const spelled *text) {
    const bool in_brackets = opened(context, slot, '[', ']'), in_parens = opened(context, slot, '(', ')'),
               in_braces = opened(context, slot, '{', '}');
    bool in_markup = false;
    const size_t before = slot->start - (slot->wrap == '<' && slot->start > 0);
    for (size_t i = 0; i < before && i < context->size; i++) {
        const uint8_t c = context->line[i], next = i + 1 < context->size ? context->line[i + 1] : 0;
        if (c == '<' && (among(next, "/!?") || ((next | 0x20) >= 'a' && (next | 0x20) <= 'z'))) {
            in_markup = true;
        } else if (c == '>') {
            in_markup = false;
        }
    }
    /* A cross link's part ends at a `|`, and an autolink or a tag at a
     * space, a quote or an in_markup bracket. */
    size_t cross = 0;
    for (size_t i = 0; i + 1 < slot->start && i + 1 < context->size; i++) {
        if (context->line[i] == context->line[i + 1] && (context->line[i] == '[' || context->line[i] == ']')) {
            cross = context->line[i] == '[' ? cross + 1 : cross ? cross - 1 : 0;
            i++;
        }
    }
    size_t brackets = 0, parens = 0;
    for (size_t i = 0; i < text->size; i++) {
        const uint8_t c = text->text[i];
        if ((in_braces && (c == '}' || c == '=')) || (cross && c == '|') || (in_markup && among(c, " \t\"'<>")) ||
            (context->quote && c == context->quote && slot->slot.kind != MARKDOWN_CORE_SLOT_VALUE)) {
            return true;
        }
        if (c == '\\') {
            i += i + 1 < text->size && !among(text->text[i + 1], "}=|");
            continue;
        }
        if ((c == ']' && !brackets && in_brackets) || (c == ')' && !parens && in_parens)) {
            return true;
        }
        brackets += c == '[' ? 1 : c == ']' ? (size_t)-1 : 0;
        parens += c == '(' ? 1 : c == ')' ? (size_t)-1 : 0;
    }
    return (brackets && in_brackets) || (parens && in_parens);
}

/* Whether the spelling would make a directive: a `:` before or in it
 * whose name runs on, through it, the closer and the line after, to a
 * label or braces. */
static bool names_directive(const markdown_core_context *context, const context_slot *slot, const spelled *text,
                            const char *closer, const uint8_t *tail, size_t rest) {
    /* A directive's name slot follows its own `:`. */
    bool run = false;
    const bool own = slot->slot.kind == MARKDOWN_CORE_SLOT_NAME && slot->slot.owner != MARKDOWN_CORE_KIND_TOPIC;
    for (size_t i = own ? 0 : slot->start; i > 0; i--) {
        const uint8_t c = context->line[i - 1];
        if (among(c, " \t[{:")) {
            run = c == ':';
            break;
        }
    }
    for (size_t i = 0; i < text->size; i++) {
        const uint8_t c = text->text[i];
        if (c == '[' || c == '{') {
            if (run) {
                return true;
            }
        } else if (c == ':') {
            run = true;
        } else if (c == ' ' || c == '\t') {
            run = false;
        }
    }
    for (const char *c = closer; *c && run; c++) {
        if (among((uint8_t)*c, " \t[{:")) {
            return *c == '[' || *c == '{';
        }
    }
    for (size_t i = 0; i < rest && run; i++) {
        if (among(tail[i], " \t\r\n[{:")) {
            return tail[i] == '[' || tail[i] == '{';
        }
    }
    return false;
}

/* The class of a byte to a delimiter run beside it: a space, punctuation,
 * or neither. */
static int flank(uint8_t c) { return c == ' ' || c == '\t' || !c ? 0 : among(c, PUNCTUATION) ? 1 : 2; }

/* Whether a block's attribute tail or callout marker the slot is in is
 * still open on the line: the document reads such a line's opening as
 * text, though the block's node holds the slot. */
static bool unfinished(const markdown_core_context *context, const context_slot *slot, const uint8_t *tail, size_t rest,
                       uint8_t closer) {
    if (!slot->slot.closed) {
        return true;
    }
    const markdown_core_node_kind owner = slot->slot.owner;
    if (owner != MARKDOWN_CORE_KIND_HEADING && owner != MARKDOWN_CORE_KIND_CODE_BLOCK &&
        owner != MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK && owner != MARKDOWN_CORE_KIND_CALLOUT) {
        return false;
    }
    /* A nameless container's class word has no braces to close. */
    return closer == ']' ? !(rest && tail[0] == ']') : opened(context, slot, '{', '}') && !memchr(tail, closer, rest);
}

/* Whether the line after a slot of a block it leaves open holds more
 * members, a space or a tab and then more: a closer after the slot would
 * cut them off, and without one what the slot holds lands in no field. */
static bool members(const uint8_t *tail, size_t rest) {
    for (size_t i = 0; i + 1 < rest; i++) {
        if ((tail[i] == ' ' || tail[i] == '\t') && tail[i + 1] != ' ' && tail[i + 1] != '\t' &&
            !markdown_core_is_line_end(tail[i + 1])) {
            return true;
        }
    }
    return false;
}

/* Whether the character of `width` bytes at `value` is one a YAML scalar
 * holds as it is: a tab, or printable, U+0085 and U+FEFF among them, but
 * not the other C1 controls, U+FFFE or U+FFFF (yaml.c). */
static bool printable(const uint8_t *value, size_t width) {
    const uint8_t c = value[0];
    if (width == 1) {
        return c == '\t' || (c >= 0x20 && c != 0x7F);
    }
    if (width == 2 && c == 0xC2) {
        return value[1] == 0x85 || value[1] >= 0xA0;
    }
    return !(width == 3 && c == 0xEF && value[1] == 0xBF && value[2] >= 0xBE);
}

/* Writes `value` between double quotes' bytes: a quote and a backslash
 * escaped, and a character no scalar holds as JSON's escape of it. */
static void quoted(spelled *out, const uint8_t *value, size_t size) {
    for (size_t i = 0; i < size;) {
        const uint8_t c = value[i];
        const size_t width = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (c == '"' || c == '\\') {
            put(out, '\\');
            put(out, c);
        } else if (c == '\t') {
            put(out, '\\');
            put(out, 't');
        } else if (!printable(value + i, width)) {
            const uint32_t code = width == 1   ? c
                                  : width == 2 ? (uint32_t)(c & 0x1F) << 6 | (value[i + 1] & 0x3F)
                                               : 0xFFFFu - (0xBFu - value[i + 2]);
            for (const char *e = "\\u"; *e; e++) {
                put(out, (uint8_t)*e);
            }
            for (int shift = 12; shift >= 0; shift -= 4) {
                put(out, (uint8_t)"0123456789abcdef"[(code >> shift) & 0xF]);
            }
        } else {
            for (size_t j = 0; j < width; j++) {
                put(out, value[i + j]);
            }
        }
        i += width;
    }
}

/* Whether `value` written bare is a plain scalar of its bytes that reads as
 * text in any member: it starts with no indicator, ends with no space, holds
 * no flow indicator, no `: `, `:?` and ` #`, no character a scalar must escape,
 * and is no null, boolean or number. */
static bool bare(const uint8_t *value, size_t size) {
    if (!size || among(value[0], "-?:,[]{}#&*!|>'\"%@`~ \t") || value[size - 1] == ' ' || value[size - 1] == '\t' ||
        !markdown_core_metadata_text(value, size)) {
        return false;
    }
    for (size_t i = 0; i < size;) {
        const uint8_t c = value[i];
        const size_t width = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (among(c, ",[]{}\t") || !printable(value + i, width) ||
            (c == ':' && (i + 1 == size || value[i + 1] == ' ' || value[i + 1] == '?')) ||
            (c == '#' && value[i - 1] == ' ')) {
            return false;
        }
        i += width;
    }
    return true;
}

/* A properties member's name or value (docs/specs/dialect/properties.md).
 * A name is one of the grammar's field names, letters, numerals, `_` and
 * `-`, written as it is. A value is written as the scalar its slot is in
 * holds it: inside double quotes escaped, inside apostrophes with each
 * doubled and nothing a scalar must escape, on a literal's line as it is,
 * with text there and, on its first line, no space to start it, which
 * would deepen its indentation; and bare where the slot is, as a plain
 * scalar when it reads as that text, else between double quotes. */
static bool spell_member(spelled *out, const context_slot *slot, const uint8_t *value, size_t size) {
    if (slot->slot.kind == MARKDOWN_CORE_SLOT_FIELD) {
        return word(value, size, "_-") && value[0] != '-' && plain(out, value, size, "", false);
    }
    switch (slot->wrap) {
    case '"':
        quoted(out, value, size);
        return true;
    case '\'':
        for (size_t i = 0; i < size;) {
            const size_t width = value[i] < 0x80 ? 1 : value[i] < 0xE0 ? 2 : value[i] < 0xF0 ? 3 : 4;
            if (!printable(value + i, width)) {
                return false;
            }
            if (value[i] == '\'') {
                put(out, '\'');
            }
            for (size_t j = 0; j < width; j++) {
                put(out, value[i + j]);
            }
            i += width;
        }
        return true;
    case '|':
        for (size_t i = 0; i < size;) {
            const size_t width = value[i] < 0x80 ? 1 : value[i] < 0xE0 ? 2 : value[i] < 0xF0 ? 3 : 4;
            if (!printable(value + i, width)) {
                return false;
            }
            i += width;
        }
        return !blank_value(value, size) && !(slot->field == slot->start && value[0] == ' ') &&
               plain(out, value, size, "", false);
    default:
        if (bare(value, size)) {
            return plain(out, value, size, "", false);
        }
        put(out, '"');
        quoted(out, value, size);
        put(out, '"');
        return true;
    }
}

markdown_core_status markdown_core_slot_spell(const markdown_core_context *context, size_t index, const uint8_t *value,
                                              size_t size, markdown_core_insertion **insertion) {
    *insertion = NULL;
    if (index >= context->slot_count) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    const context_slot *slot = &context->slots[index];
    if (!readable(value, size) || slot->split) {
        return MARKDOWN_CORE_OK;
    }
    const markdown_core_slot_kind kind = slot->slot.kind;
    const markdown_core_node_kind owner = slot->slot.owner;
    const bool row = context->row, closed = slot->slot.closed;
    const uint8_t *tail = context->line + slot->end;
    const size_t rest = context->size - slot->end;
    /* An escape doubles a byte, a reference's `&` takes five, and quotes,
     * braces and the closer a few more. */
    markdown_core_insertion *made = markdown_core_alloc(1, sizeof(*made) + 8 * size + 64);
    if (!made) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    spelled out = {(uint8_t *)(made + 1), 0};
    if (owner == MARKDOWN_CORE_KIND_METADATA) {
        if (!spell_member(&out, slot, value, size)) {
            markdown_core_free(made);
            return MARKDOWN_CORE_OK;
        }
        made->text = out.text;
        made->cursor = made->size = out.size;
        *insertion = made;
        return MARKDOWN_CORE_OK;
    }
    const char *closer = "";
    bool held = true;
    const bool cross = owner == MARKDOWN_CORE_KIND_CROSS_LINK || owner == MARKDOWN_CORE_KIND_CROSS_EMBEDDED;
    const bool call = owner == MARKDOWN_CORE_KIND_LINK || owner == MARKDOWN_CORE_KIND_EMBEDDED;
    const uint8_t fence = owner == MARKDOWN_CORE_KIND_CODE_BLOCK ? fence_of(context, slot->start) : 0;
    switch (kind) {
    case MARKDOWN_CORE_SLOT_DEST:
    case MARKDOWN_CORE_SLOT_ANCHOR:
        if (cross) {
            /* A cross target keeps its bytes: what ends a part, a bracket
             * or a line ending it cannot hold, and a final backslash would
             * escape the separator after it. */
            held = size > 0 && value[size - 1] != '\\' && !opaque(value, size) && plain(&out, value, size, "|[]", row);
            if (kind == MARKDOWN_CORE_SLOT_DEST) {
                held = held && !memchr(value, '#', size);
            } else {
                /* A heading's parts are not empty, and a leading `^` marks a
                 * block's identifier, which the anchor drops. */
                held = held && value[0] != '#' && value[size - 1] != '#' && value[0] != '^';
                for (size_t i = 0; held && i + 1 < size; i++) {
                    held = !(value[i] == '#' && value[i + 1] == '#');
                }
            }
            closer = closed ? "" : "]]";
        } else {
            held = !closes(value, size) && spell_destination(&out, slot, row, value, size, tail, rest);
            /* An open `<` closes after the field: where a fragment goes on
             * after the slot, the closers cannot stand at it. */
            const bool in_markup = slot->wrap == '<' && !(rest && tail[0] == '>');
            const bool goes = in_markup && rest && tail[0] != ' ' && tail[0] != '\t';
            closer = closed || goes ? "" : call ? (in_markup ? ">)" : ")") : (in_markup ? ">" : "");
        }
        break;
    case MARKDOWN_CORE_SLOT_LABEL:
        /* A label keeps its bytes. One that begins with `^` is a
         * footnote's. A closed one whose `]` is on a later line goes on
         * past the line. */
        if (closed && !memchr(tail, ']', rest)) {
            held = false;
            break;
        }
        held = !blank_value(value, size) && size < 1000 && value[0] != '^' && !bracketed(value, size) &&
               !opaque(value, size) && plain(&out, value, size, "", row);
        closer = closed ? "" : "]";
        break;
    case MARKDOWN_CORE_SLOT_FOOTNOTE:
        /* A footnote's label keeps its bytes, holds no bracket even
         * escaped, and a final backslash would escape its `]`. */
        if (closed && !memchr(tail, ']', rest)) {
            held = false;
            break;
        }
        held = size > 0 && size <= 1000 && value[size - 1] != '\\' && !opaque(value, size) &&
               plain(&out, value, size, " \t[]", row);
        closer = closed ? "" : "]";
        break;
    case MARKDOWN_CORE_SLOT_BIB:
    case MARKDOWN_CORE_SLOT_SPECIMEN: {
        held = !opaque(value, size) && spell_cite(&out, row, value, size);
        if (kind == MARKDOWN_CORE_SLOT_SPECIMEN) {
            /* A label is key characters with single hyphens between, and
             * no braces write one. */
            held = held && slot->wrap != '{' && value[0] != '-' && value[size - 1] != '-';
            for (size_t i = 0; held && i < size; i++) {
                const uint8_t c = value[i];
                held = c >= 0x80 || word(&c, 1, "_") || (c == '-' && value[i + 1] != '-');
            }
        } else if (held && slot->wrap != '{') {
            /* A key the bare reading does not read whole is written in
             * braces. */
            bool whole = true;
            if (!scans(context, slot, &out, tail, rest, &whole)) {
                markdown_core_free(made);
                return MARKDOWN_CORE_ALLOCATION_FAILED;
            }
            if (!whole) {
                memmove(out.text + 1, out.text, out.size);
                out.text[0] = '{';
                out.size++;
                put(&out, '}');
            }
        }
        const uint8_t open = slot->opener != SIZE_MAX ? context->line[slot->opener] : 0;
        closer = slot->wrap == '{' && !closed ? (open == '['   ? "}]"
                                                 : open == '(' ? "})"
                                                               : "}")
                 : closed                     ? ""
                 : open == '['                ? "]"
                 : open == '('                ? ")"
                                              : "";
        break;
    }
    case MARKDOWN_CORE_SLOT_NAME:
        if (owner == MARKDOWN_CORE_KIND_TOPIC) {
            held = size > 0 && plain(&out, value, size, " \t#", row);
        } else {
            /* A final backslash would escape the label or braces after. */
            held =
                size > 0 && value[size - 1] != '\\' && !opaque(value, size) && plain(&out, value, size, " \t[{:", row);
            closer = owner == MARKDOWN_CORE_KIND_DIRECTIVE && !closed ? "[]" : "";
        }
        break;
    case MARKDOWN_CORE_SLOT_ID:
        if (owner == MARKDOWN_CORE_KIND_PARAGRAPH || owner == MARKDOWN_CORE_KIND_LIST_ITEM ||
            owner == MARKDOWN_CORE_KIND_LIST || owner == MARKDOWN_CORE_KIND_CALLOUT ||
            owner == MARKDOWN_CORE_KIND_TABLE) {
            /* A block identifier: ASCII letters, numerals and hyphens. */
            held = word(value, size, "-") && plain(&out, value, size, "", row);
            closer = closed ? "" : "#";
            break;
        }
        /* fall through */
    case MARKDOWN_CORE_SLOT_CLASS:
    case MARKDOWN_CORE_SLOT_RECORD:
        /* A shorthand or a name keeps its bytes; a final backslash would
         * escape the container's closer. */
        held = size > 0 && value[size - 1] != '\\' && !opaque(value, size) &&
               plain(&out, value, size, " \t=}\"'{", row) &&
               !(kind == MARKDOWN_CORE_SLOT_RECORD &&
                 (value[0] == '#' || value[0] == '.' || (size == 1 && value[0] == '-')));
        closer = unfinished(context, slot, tail, rest, '}') ? "}" : "";
        held = held && !(*closer && members(tail, rest));
        break;
    case MARKDOWN_CORE_SLOT_VALUE:
        held = !closes(value, size) && spell_attribute(&out, slot, context->line, row, value, size);
        closer = unfinished(context, slot, tail, rest, '}') ? "}" : "";
        held = held && !(*closer && members(tail, rest));
        break;
    case MARKDOWN_CORE_SLOT_VARIANT:
        held = word(value, size, "_-") && plain(&out, value, size, "", row);
        closer = unfinished(context, slot, tail, rest, ']') ? "]" : "";
        break;
    case MARKDOWN_CORE_SLOT_INFO:
        /* The slot is the info string's first word, the language. */
        held = size > 0 && !memchr(value, ' ', size) && !memchr(value, '\t', size) &&
               !(fence == '`' && memchr(value, '`', size));
        if (held) {
            escaped(&out, value, size, "\\{", row);
        }
        break;
    default:
        held = false;
        break;
    }
    if (held && fence && kind != MARKDOWN_CORE_SLOT_INFO) {
        /* A backtick fence's info string holds no backtick. */
        held = !(fence == '`' && memchr(out.text, '`', out.size));
    }
    /* The spelling must leave the constructs around the slot as they are:
     * it ends none the line opens before it, begins no directive, and
     * leaves a delimiter run or a citation's `@` after it the byte class
     * before it it had. */
    if (held && kind != MARKDOWN_CORE_SLOT_INFO) {
        held = !ends(context, slot, &out) && !names_directive(context, slot, &out, closer, tail, rest);
    }
    if (held && !*closer && rest && (among(tail[0], "*_~^@") || (tail[0] == '=' && rest > 1 && tail[1] == '='))) {
        const uint8_t was = slot->end > 0 ? context->line[slot->end - 1] : 0;
        const uint8_t now = out.size ? out.text[out.size - 1] : slot->start > 0 ? context->line[slot->start - 1] : 0;
        held = flank(was) == flank(now);
    }
    bool whole = true;
    if (held && !(kind == MARKDOWN_CORE_SLOT_BIB && slot->wrap != '{') &&
        !scans(context, slot, &out, tail, rest, &whole)) {
        markdown_core_free(made);
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    if (!held || !whole) {
        markdown_core_free(made);
        return MARKDOWN_CORE_OK;
    }
    made->text = out.text;
    made->cursor = out.size;
    for (const char *c = closer; *c; c++) {
        put(&out, (uint8_t)*c);
    }
    made->size = out.size;
    *insertion = made;
    return MARKDOWN_CORE_OK;
}

void markdown_core_insertion_free(markdown_core_insertion *insertion) { markdown_core_free(insertion); }
