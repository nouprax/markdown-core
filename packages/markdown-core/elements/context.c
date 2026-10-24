#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "../include/markdown_core.h"

#include "ast_internal.h"
#include "sink.h"
#include "attributes.h"
#include "../core/attributes.h"

#include <node.h>

/* A slot as the context keeps it: the public value, and where its prefix
 * strings begin in the context's prefixes. */
typedef struct {
    markdown_core_slot slot;
    size_t prefix;
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
};

/* A source range of a node of the document, the range of the holding that
 * encloses it, by index, or -1, and its place in the document's walk. */
typedef struct {
    const markdown_core_node *node;
    uint32_t start, end;
    size_t order;
    ptrdiff_t enclosing;
} holding;

/* What the query reads: the whole text and its line starts, the document
 * parsed from it, and the cursor's byte. `holdings` are the document's
 * source ranges, read by one walk, in source order, outer before inner; when
 * they nest, `nested`, the nodes that hold a byte are found by climbing
 * from the last that starts at or before it, not by another walk. */
typedef struct {
    const markdown_core_document *document;
    const uint8_t *source;
    size_t length;
    markdown_core_source_lines lines;
    size_t cursor;
    holding *holdings;
    size_t holding_count;
    bool nested;
    struct column_mark *marks;
    struct tail_memo *tail;
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

/* The attribute parser of the line whose tails are being confirmed, from
 * `from` to `to`: the candidate braces of one line share its memo. */
typedef struct tail_memo {
    size_t from, to;
    markdown_core_attribute_parser parser;
} tail_memo;

static size_t query_line(const context_query *query, size_t offset) {
    size_t lo = 0, hi = query->lines.count;
    while (hi - lo > 1) {
        const size_t mid = lo + (hi - lo) / 2;
        if (query->lines.starts[mid] <= offset) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* The line holding `offset`, as a 1-based number, and the UTF-16 units from
 * its start to `offset`, counted on from the nearer mark before it; the
 * count is left in mark `which`. */
static size_t query_units(const context_query *query, size_t offset, int32_t *line, size_t which) {
    const size_t at = query_line(query, offset);
    size_t from = query->lines.starts[at], units = 0;
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
    if (query->document->unit == MARKDOWN_CORE_TEXT_UNIT_UTF8) {
        return markdown_core_source_scope(&query->lines, query->source,
                                          (markdown_core_place){(uint32_t)start, (uint32_t)end}, query->document->unit);
    }
    markdown_core_scope scope;
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

static int holding_order(const void *left, const void *right) {
    const holding *a = left, *b = right;
    if (a->start != b->start) {
        return a->start < b->start ? -1 : 1;
    }
    if (a->end != b->end) {
        return a->end > b->end ? -1 : 1;
    }
    return a->order < b->order ? -1 : a->order > b->order ? 1 : 0;
}

/* Reads the document's source ranges into the query's holdings with one
 * walk, sorts them and links each to the range that encloses it; false when
 * the walk or the list could not be allocated. */
static bool read_holdings(context_query *query) {
    size_t capacity = 0;
    bool grown = true;
    markdown_core_walk walk;
    markdown_core_walk_item item;
    markdown_core_walk_begin(&walk, query->document->root);
    for (size_t order = 0; grown && markdown_core_walk_next(&walk, &item); order++) {
        const markdown_core_place *ranges;
        size_t count;
        if (!item.node || !markdown_core_walk_ranges(&walk, &item, &ranges, &count)) {
            continue;
        }
        for (size_t i = 0; grown && i < count; i++) {
            if (ranges[i].start >= ranges[i].end) {
                continue;
            }
            holding *grew =
                markdown_core_reserve(query->holdings, &capacity, query->holding_count + 1, sizeof(*query->holdings));
            grown = grew != NULL;
            if (grew) {
                query->holdings = grew;
                query->holdings[query->holding_count++] =
                    (holding){item.node, ranges[i].start, ranges[i].end, order, -1};
            }
        }
    }
    const bool failed = walk.failed || !grown;
    markdown_core_walk_end(&walk);
    if (failed) {
        return false;
    }
    if (query->holding_count) {
        qsort(query->holdings, query->holding_count, sizeof(*query->holdings), holding_order);
    }
    /* Each range's enclosing range is the innermost one still open where it
     * starts; a range that crosses one's end leaves the ranges unnested. */
    query->nested = true;
    ptrdiff_t open = -1;
    for (size_t i = 0; i < query->holding_count; i++) {
        holding *at = &query->holdings[i];
        while (open >= 0 && query->holdings[open].end <= at->start) {
            open = query->holdings[open].enclosing;
        }
        if (open >= 0 && query->holdings[open].end < at->end) {
            query->nested = false;
        }
        at->enclosing = open;
        open = (ptrdiff_t)i;
    }
    return true;
}

/* The nodes of the document that hold the byte at `offset`, outermost
 * first, as markdown_core_tree_holders lists them; false when the list
 * could not be allocated. */
static bool query_holders(const context_query *query, size_t offset, const markdown_core_node ***nodes, size_t *count) {
    const markdown_core_node *last;
    if (!query->nested) {
        return markdown_core_tree_holders(query->document->root, offset, nodes, count, &last);
    }
    size_t low = 0, high = query->holding_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (query->holdings[middle].start <= offset) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    ptrdiff_t at = (ptrdiff_t)low - 1;
    while (at >= 0 && query->holdings[at].end <= offset) {
        at = query->holdings[at].enclosing;
    }
    size_t depth = 0;
    for (ptrdiff_t up = at; up >= 0; up = query->holdings[up].enclosing) {
        depth++;
    }
    *nodes = NULL;
    *count = depth;
    if (!depth) {
        return true;
    }
    const markdown_core_node **held = markdown_core_alloc(depth, sizeof(*held));
    size_t *orders = markdown_core_alloc(depth, sizeof(*orders));
    if (!held || !orders) {
        markdown_core_free((void *)held);
        markdown_core_free(orders);
        return false;
    }
    /* In walk order, as the walk lists them: a node the walk reaches through
     * another, as a note through its citation, may nest otherwise. */
    for (ptrdiff_t up = at; up >= 0; up = query->holdings[up].enclosing) {
        size_t i = --depth;
        for (; i + 1 < *count && query->holdings[up].order > orders[i + 1]; i++) {
            held[i] = held[i + 1];
            orders[i] = orders[i + 1];
        }
        held[i] = query->holdings[up].node;
        orders[i] = query->holdings[up].order;
    }
    markdown_core_free(orders);
    *nodes = held;
    return true;
}

/* The deepest node of the document that holds the byte at `offset`, and its
 * holder before it, or NULL for either; false when the walk failed. */
static bool held_by(const context_query *query, size_t offset, const markdown_core_node **node,
                    const markdown_core_node **holder) {
    const markdown_core_node **path = NULL;
    size_t count = 0;
    *node = *holder = NULL;
    if (offset >= query->length) {
        return true;
    }
    if (!query_holders(query, offset, &path, &count)) {
        return false;
    }
    if (count) {
        *node = path[count - 1];
        *holder = count > 1 ? path[count - 2] : NULL;
    }
    markdown_core_free((void *)path);
    return true;
}

/* Whether `node` is one of the nodes of the document that hold the byte at
 * `offset`, in `*found`; false when the walk failed. */
static bool holds(const context_query *query, const markdown_core_node *node, size_t offset, bool *found) {
    const markdown_core_node **path = NULL;
    size_t count = 0;
    *found = false;
    if (offset >= query->length) {
        return true;
    }
    if (!query_holders(query, offset, &path, &count)) {
        return false;
    }
    for (size_t i = 0; i < count && !*found; i++) {
        *found = path[i] == node;
    }
    markdown_core_free((void *)path);
    return true;
}

/* Whether `node` holds its source as literal text, which no construct
 * reads inside: a code span, a formula, inline HTML or a comment. */
static bool literal(const markdown_core_node *node) {
    switch (markdown_core_node_get_kind(node)) {
    case MARKDOWN_CORE_KIND_CODE:
    case MARKDOWN_CORE_KIND_FORMULA:
    case MARKDOWN_CORE_KIND_HTML:
    case MARKDOWN_CORE_KIND_COMMENT:
        return true;
    default:
        return false;
    }
}

/* Whether `anchor` is the identifier written at `first`, ended by the `#`
 * of a block identifier that ends its line, or by the brace or separator
 * after the `#id` of the attribute block whose brace is at `brace`: the one
 * the field declares, not another the node carries, nor the same name
 * written as text before it. */
static bool names(const context_query *query, const markdown_core_chunk *anchor, size_t first, size_t brace) {
    const size_t length = (size_t)anchor->len, end = first + length;
    const bool block = brace < query->length && query->source[brace] == '{';
    if (!length || end > query->length || memcmp(query->source + first, anchor->data, length)) {
        return false;
    }
    if (block) {
        return end == query->length || (query->source[end] && strchr("} \t\r\n", query->source[end]));
    }
    /* A block identifier's `#` ends its line. */
    size_t at = end + 1;
    while (at < query->length && (query->source[at] == ' ' || query->source[at] == '\t')) {
        at++;
    }
    return end < query->length && query->source[end] == '#' &&
           (at >= query->length || query->source[at] == '\n' || query->source[at] == '\r');
}

/* Whether `chunk` begins with the `length` bytes at `text`. */
static bool leads(const markdown_core_chunk *chunk, const uint8_t *text, size_t length) {
    return (size_t)chunk->len >= length && (!length || !memcmp(chunk->data, text, length));
}

/* Whether `node` carries the class or the value written from `first` to
 * the cursor: a block that ends at the brace of another, a value's, holds
 * none of the members written inside that value. A field that holds an
 * escape, a reference or a quote is passed, since its member holds other
 * bytes than its source. */
static bool carries(const context_query *query, const markdown_core_node *node, markdown_core_slot_kind slot,
                    size_t first) {
    if ((slot != MARKDOWN_CORE_SLOT_CLASS && slot != MARKDOWN_CORE_SLOT_VALUE) || first > query->cursor) {
        return true;
    }
    const uint8_t *text = query->source + first;
    size_t length = query->cursor - first, word = 0;
    /* An unquoted value ends at the spaces before the cursor; a field not
     * yet begun is the word after it. */
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\t')) {
        length--;
    }
    if (first == query->cursor) {
        while (first + length < query->length && !strchr(" \t\r\n}#.=", text[length])) {
            length++;
        }
    }
    if (!length) {
        return true;
    }
    for (size_t i = 0; i < length; i++) {
        if (strchr("\\&\"'\r\n", text[i])) {
            return true;
        }
        if (text[i] == ' ' || text[i] == '\t') {
            word = i + 1;
        }
    }
    const markdown_core_attributes *attributes = &node->attributes;
    /* A `class=` value's last word is a class. */
    for (uint32_t i = 0; i < attributes->class_count; i++) {
        if (leads(&attributes->classes[i], text + word, length - word)) {
            return true;
        }
    }
    if (slot == MARKDOWN_CORE_SLOT_CLASS) {
        return false;
    }
    for (uint32_t i = 0; i < attributes->record_count; i++) {
        if (leads(&attributes->records[i].value, text, length)) {
            return true;
        }
    }
    return leads(&attributes->anchor, text, length);
}

/* Whether `citation` is one a field of `slot` writes: a footnote's label
 * one that names a footnote, a key one that names no footnote. A citation
 * that stands for an inline note holds the note's bytes, and no field of
 * its own among them. */
static bool refers(const markdown_core_node *citation, markdown_core_slot_kind slot) {
    const markdown_core_citation_item *item = citation->as.citation;
    if (item->note) {
        return false;
    }
    switch (slot) {
    case MARKDOWN_CORE_SLOT_FOOTNOTE:
        return item->referent == MARKDOWN_CORE_NODE_REFERENT_FOOTNOTE;
    case MARKDOWN_CORE_SLOT_BIB:
    case MARKDOWN_CORE_SLOT_SPECIMEN:
        return item->referent != MARKDOWN_CORE_NODE_REFERENT_FOOTNOTE;
    default:
        return true;
    }
}

/* Whether the document holds a node of kind `owner` whose source holds the
 * byte at `first` and the last byte before the cursor, or the cursor's byte
 * when the field is empty: the construct the field belongs to, which the
 * document finishes at or after the cursor. At the document's end, where no
 * byte follows, the last byte stands for both. An identifier's `slot` names
 * its node, which then carries the anchor written at `first`; a class's or
 * a value's in an inline block is one that `carries` it, and a citation's
 * one it `refers` to.
 * A construct whose opener the field names starts at it, at `begins`, or
 * else is another of its kind that holds the bytes: a link whose title
 * holds the text of one. SIZE_MAX names no start. A construct that takes
 * the attribute block at `brace` ends with it: it holds the brace and the
 * block's last byte, and not `past`, the byte after the block. A construct
 * that holds `past` holds the block as text, in a title, and one that
 * starts past the brace is one the block holds. No construct takes a block
 * the document leaves open, whose `past` is its brace. SIZE_MAX names no
 * block. */
static bool finished_by(const context_query *query, markdown_core_node_kind owner, size_t first,
                        markdown_core_slot_kind slot, size_t begins, size_t brace, size_t past, bool *found) {
    size_t probe = query->cursor > first || query->cursor >= query->length ? query->cursor - 1 : query->cursor;
    /* At the start of a later line of the field, past only the line's
     * container prefix, which no inline node holds, the cursor's own byte
     * is the field's. */
    if (probe + 1 == query->cursor && query->cursor < query->length &&
        query_line(query, first) < query_line(query, query->cursor)) {
        size_t at = query->lines.starts[query_line(query, query->cursor)];
        while (at < query->cursor &&
               (query->source[at] == ' ' || query->source[at] == '\t' || query->source[at] == '>')) {
            at++;
        }
        if (at == query->cursor) {
            probe = query->cursor;
        }
    }
    const markdown_core_node **at_probe = NULL, **at_first = NULL;
    size_t probe_count = 0, first_count = 0;
    *found = false;
    if ((!query->cursor && !query->length) || owner == MARKDOWN_CORE_KIND_NONE) {
        return true;
    }
    if (first >= query->length) {
        first = probe;
    }
    if (!query_holders(query, probe, &at_probe, &probe_count) ||
        !query_holders(query, first, &at_first, &first_count)) {
        markdown_core_free((void *)at_probe);
        return false;
    }
    /* A node that holds its bytes as literal text, deeper than the one
     * found, holds them instead: a construct inside a code span is text. */
    size_t opaque = 0;
    for (size_t i = 0; i < probe_count; i++) {
        if (literal(at_probe[i])) {
            opaque = i + 1;
        }
    }
    for (size_t i = 0; i < probe_count && !*found; i++) {
        if (markdown_core_node_get_kind(at_probe[i]) != owner ||
            (slot == MARKDOWN_CORE_SLOT_ID && !names(query, &at_probe[i]->attributes.anchor, first, brace)) ||
            (owner == MARKDOWN_CORE_KIND_CITATION && !refers(at_probe[i], slot)) ||
            (past != SIZE_MAX && !carries(query, at_probe[i], slot, first))) {
            continue;
        }
        bool starts = true, before = false;
        if (begins != SIZE_MAX && (!holds(query, at_probe[i], begins, &starts) ||
                                   (begins && !holds(query, at_probe[i], begins - 1, &before)))) {
            markdown_core_free((void *)at_probe);
            markdown_core_free((void *)at_first);
            return false;
        }
        bool over = false, takes = true;
        if (past != SIZE_MAX && (!holds(query, at_probe[i], past, &over) || !holds(query, at_probe[i], brace, &takes) ||
                                 (takes && past > brace && !holds(query, at_probe[i], past - 1, &takes)))) {
            markdown_core_free((void *)at_probe);
            markdown_core_free((void *)at_first);
            return false;
        }
        if (!starts || before || over || !takes) {
            continue;
        }
        /* A block identifier ends the paragraph it is written in. */
        if (slot == MARKDOWN_CORE_SLOT_ID && !(brace < query->length && query->source[brace] == '{')) {
            const size_t next = line_end(query, first) + 1;
            bool later = false;
            for (size_t j = first_count; j-- > 0 && next < query->length;) {
                if (markdown_core_node_get_kind(at_first[j]) == MARKDOWN_CORE_KIND_PARAGRAPH) {
                    if (!holds(query, at_first[j], next, &later)) {
                        markdown_core_free((void *)at_probe);
                        markdown_core_free((void *)at_first);
                        return false;
                    }
                    break;
                }
            }
            if (later) {
                continue;
            }
        }
        for (size_t j = 0; j < first_count && opaque <= i + 1; j++) {
            if (at_first[j] == at_probe[i]) {
                *found = true;
                break;
            }
        }
    }
    markdown_core_free((void *)at_probe);
    markdown_core_free((void *)at_first);
    return true;
}

/* THE CONFIRMATION of what the text before the cursor leaves open (3.1),
 * a field from `first` that a construct of kind `owner` reads, or an
 * opener, whose syntax starts at `opener`: it survives when the document
 * finishes its construct after the cursor, `*closed`, or reads its opener
 * as text in an inline root that holds the cursor's last byte before it;
 * the document absorbed it into another node otherwise, and `*kept` is
 * false. */
static bool confirm(const context_query *query, markdown_core_node_kind owner, size_t first,
                    markdown_core_slot_kind slot, size_t opener, size_t begins, size_t brace, size_t past, bool *kept,
                    bool *closed) {
    *kept = *closed = false;
    if (!finished_by(query, owner, first, slot, begins, brace, past, closed)) {
        return false;
    }
    if (*closed) {
        *kept = true;
        return true;
    }
    const markdown_core_node *text, *root;
    if (!held_by(query, opener, &text, &root)) {
        return false;
    }
    if (!text || markdown_core_node_get_kind(text) != MARKDOWN_CORE_KIND_TEXT || !root) {
        return true;
    }
    const size_t last = query->cursor > opener ? query->cursor - 1 : opener;
    return holds(query, root, last, kept);
}

static int opener_order(const void *left, const void *right) {
    const markdown_core_context_opener *a = left, *b = right;
    if (a->place.start != b->place.start) {
        return a->place.start < b->place.start ? -1 : 1;
    }
    return a->place.end > b->place.end ? -1 : a->place.end < b->place.end ? 1 : 0;
}

/* The context's blocks: each open block's kind, and its opening line from
 * its start to the line's end or the cursor. */
static bool take_blocks(markdown_core_context *context, const context_query *query,
                        const markdown_core_context_sink *sink) {
    if (!sink->block_count) {
        return true;
    }
    context->blocks = markdown_core_alloc(sink->block_count, sizeof(*context->blocks));
    if (!context->blocks) {
        return false;
    }
    /* Blocks nested on one line share its end: a start the last scan
     * passed takes the end it found. */
    size_t from = SIZE_MAX, end = 0;
    for (size_t i = 0; i < sink->block_count; i++) {
        const markdown_core_node *block = sink->blocks[i];
        size_t start = block->place.start < query->cursor ? block->place.start : query->cursor;
        if (start < from || start > end) {
            from = start;
            end = line_end(query, start);
        }
        context->blocks[context->block_count++] = (markdown_core_opener){
            markdown_core_node_get_kind(block), query_scope(query, start, end < query->cursor ? end : query->cursor)};
    }
    return true;
}

/* The context's inlines: the openers of inline runs the document confirms,
 * earliest first, and of two at one start the longer first; a line's
 * openers stand only for the fields they read. The sink's fields name their openers by index,
 * so they are sorted in a copy. */
static bool take_inlines(markdown_core_context *context, const context_query *query,
                         const markdown_core_context_sink *sink) {
    if (!sink->opener_count) {
        return true;
    }
    markdown_core_context_opener *sorted = markdown_core_alloc(sink->opener_count, sizeof(*sorted));
    context->inlines = markdown_core_alloc(sink->opener_count, sizeof(*context->inlines));
    if (!sorted || !context->inlines) {
        markdown_core_free(sorted);
        return false;
    }
    memcpy(sorted, sink->openers, sink->opener_count * sizeof(*sorted));
    qsort(sorted, sink->opener_count, sizeof(*sorted), opener_order);
    for (size_t i = 0; i < sink->opener_count; i++) {
        bool kept, closed;
        if (sorted[i].block) {
            continue;
        }
        if (!confirm(query, sorted[i].kind, sorted[i].place.start, MARKDOWN_CORE_SLOT_NONE, sorted[i].place.start,
                     sorted[i].kind == MARKDOWN_CORE_KIND_NONE || sorted[i].kind == MARKDOWN_CORE_KIND_CITE
                         ? SIZE_MAX
                         : sorted[i].place.start,
                     SIZE_MAX, SIZE_MAX, &kept, &closed)) {
            markdown_core_free(sorted);
            return false;
        }
        if (kept) {
            context->inlines[context->inline_count++] =
                (markdown_core_opener){sorted[i].kind, query_scope(query, sorted[i].place.start, sorted[i].place.end)};
        }
    }
    markdown_core_free(sorted);
    return true;
}

/* A field's place among those that read the same slot from the same start:
 * by slot kind and start, then by its opener's start and its listing order. */
typedef struct reading {
    markdown_core_slot_kind kind;
    uint32_t start;
    uint32_t opener;
    size_t index;
} reading;

static int reading_order(const void *left, const void *right) {
    const reading *a = left, *b = right;
    if (a->kind != b->kind) {
        return a->kind < b->kind ? -1 : 1;
    }
    if (a->start != b->start) {
        return a->start < b->start ? -1 : 1;
    }
    if (a->opener != b->opener) {
        return a->opener < b->opener ? -1 : 1;
    }
    return a->index < b->index ? -1 : a->index > b->index ? 1 : 0;
}

/* Whether a field that ends at `end` ended before the cursor at a space or
 * a tab. An inline run's content drops the spaces and tabs at its end, so
 * its scanner reports the field before them as open, though the cursor is
 * past it. A field may end short of the cursor in other ways: `#foo/` reads
 * its `/` only once a segment follows. */
static bool spaced(const context_query *query, size_t end) {
    for (size_t at = end; at < query->cursor; at++) {
        if (query->source[at] == ' ' || query->source[at] == '\t') {
            return true;
        }
    }
    return false;
}

/* Whether the attribute block whose brace is at `brace` ends the line of a
 * block that takes it as its tail, in `*ends`: past its closing brace, only
 * spaces and tabs, and for a fenced directive, whose closing colons may
 * follow its tail, `colons`, those colons. A heading's closing hashes come
 * before the tail, not after it. A block the line leaves open is still
 * being written when nothing but spaces and tabs follows the cursor. False
 * when recognition ran out of memory. */
static bool tail_ends(const context_query *query, size_t brace, size_t line, bool colons, bool *ends) {
    tail_memo *tail = query->tail;
    const size_t from = query->lines.starts[query_line(query, brace)];
    if (!tail->parser.data || tail->from != from || tail->to != line) {
        markdown_core_attribute_parser_free(&tail->parser);
        tail->from = from;
        tail->to = line;
        tail->parser = (markdown_core_attribute_parser){.data = query->source + tail->from,
                                                        .length = (bufsize_t)(line - tail->from)};
    }
    const bufsize_t start = (bufsize_t)(brace - tail->from);
    bufsize_t at = markdown_core_attributes_end(&tail->parser, start);
    /* A closed block is the tail only when it is the line's first block
     * ending where it ends, as the block's parser selects it: one inside an
     * earlier block's value is text of that value. */
    const bool first = !at || markdown_core_attributes_tail(&tail->parser, 0, at) == start;
    size_t end = at ? tail->from + (size_t)at : query->cursor;
    *ends = false;
    if (tail->parser.oom) {
        return false;
    }
    if (!first) {
        return true;
    }
    while (end < line &&
           (query->source[end] == ' ' || query->source[end] == '\t' || (colons && at && query->source[end] == ':'))) {
        end++;
    }
    *ends = end == line;
    return true;
}

/* Whether the field from `field->start` to `end`, on a line that ends at
 * `last`, is where its construct reads it. A fence's info string and a
 * fenced directive's name follow the fence's run of marks, which goes on
 * through any further mark: ``` before a ` is ````. A destination and a
 * fenced directive's name may follow their opener after spaces, so an
 * empty one with more written past the spaces is not the one the document
 * reads: `[x]:‸ /u` reads `/u`, and `:::‸ {.a}` has no name. */
static bool marked(const context_query *query, const markdown_core_context_field *field, size_t last, size_t end) {
    const unsigned char *source = query->source;
    const bool fenced = field->kind == MARKDOWN_CORE_SLOT_INFO ||
                        (field->kind == MARKDOWN_CORE_SLOT_NAME && field->owner == MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK);
    if (fenced && field->start > 0 && field->start < last) {
        const unsigned char mark = source[field->start - 1];
        if ((mark == '`' || mark == '~' || mark == ':') && source[field->start] == mark) {
            return false;
        }
    }
    if ((field->kind == MARKDOWN_CORE_SLOT_DEST || (fenced && field->kind == MARKDOWN_CORE_SLOT_NAME)) &&
        end == field->start) {
        size_t at = end;
        while (at < last && (source[at] == ' ' || source[at] == '\t')) {
            at++;
        }
        if (at > end && at < last && source[at] != ')') {
            return false;
        }
    }
    /* A destination's path or anchor is the one the document reads only
     * when it keeps the destination's own syntax whole: its parentheses
     * balanced, and the brackets of a `<...>` destination on both sides of
     * it. One the line's end cuts short is still being written. */
    const markdown_core_node_kind owner = field->owner;
    if ((field->kind == MARKDOWN_CORE_SLOT_DEST || field->kind == MARKDOWN_CORE_SLOT_ANCHOR) &&
        (owner == MARKDOWN_CORE_KIND_LINK || owner == MARKDOWN_CORE_KIND_EMBEDDED ||
         owner == MARKDOWN_CORE_KIND_REFERENCE)) {
        const size_t origin = field->start - field->lead;
        const bool angled = source[origin] == '<' || (origin > 0 && source[origin - 1] == '<');
        long depth = 0;
        for (size_t at = field->start; at < end && !angled; at++) {
            if (source[at] == '\\' && at + 1 < end) {
                at++;
            } else if (source[at] == '(') {
                depth++;
            } else if (source[at] == ')' && --depth < 0) {
                return false;
            }
        }
        if (end < last && source[origin] == '<' && (end == origin || source[end - 1] != '>')) {
            return false;
        }
    }
    /* A topic's name is one only when no `#` follows it at once. */
    if (field->kind == MARKDOWN_CORE_SLOT_NAME && owner == MARKDOWN_CORE_KIND_TOPIC && end < last &&
        source[end] == '#') {
        return false;
    }
    /* A nameless container's class word, which a space puts after its
     * colons where a `.` puts a member's class, is one only when nothing
     * but closing colons follows it on the line. */
    if (field->kind == MARKDOWN_CORE_SLOT_CLASS && owner == MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK &&
        (source[field->start - 1] == ' ' || source[field->start - 1] == '\t')) {
        size_t at = end;
        while (at < last && (source[at] == ' ' || source[at] == '\t' || source[at] == ':')) {
            at++;
        }
        if (at < last && source[at] != '\r' && source[at] != '\n') {
            return false;
        }
    }
    /* A callout's variant is one only when its marker ends the word: `]`,
     * a fold sign, then a space or the line's end. A marker the line's end
     * cuts short is still being written. */
    if (field->kind == MARKDOWN_CORE_SLOT_VARIANT) {
        size_t at = end;
        while (at < last && (source[at] == '\r' || source[at] == '\n')) {
            at++;
        }
        if (at == last) {
            return true;
        }
        at = end;
        if (source[at] != ']') {
            return false;
        }
        at++;
        if (at < last && (source[at] == '+' || source[at] == '-')) {
            at++;
        }
        return at == last || source[at] == ' ' || source[at] == '\t' || source[at] == '\r' || source[at] == '\n';
    }
    return true;
}

/* Where the text from the line end `last` stops being one block's: at the
 * next blank line, or the source's end. */
static size_t block_end(const context_query *query, size_t last) {
    size_t at = last;
    while (at < query->length) {
        size_t next =
            at + (query->source[at] == '\r' && at + 1 < query->length && query->source[at + 1] == '\n' ? 2 : 1);
        size_t end = next;
        while (end < query->length && (query->source[end] == ' ' || query->source[end] == '\t')) {
            end++;
        }
        if (end >= query->length || query->source[end] == '\n' || query->source[end] == '\r') {
            return at;
        }
        at = line_end(query, end);
    }
    return at;
}

/* Whether only spaces and tabs, and at most one line ending, come between
 * `from` and the cursor, and something does. */
static bool blank(const context_query *query, size_t from) {
    size_t at = from;
    while (at < query->cursor && (query->source[at] == ' ' || query->source[at] == '\t')) {
        at++;
    }
    if (at < query->cursor && query->source[at] == '\r') {
        at++;
    }
    if (at < query->cursor && query->source[at] == '\n' && (at == from || query->source[at - 1] != '\n')) {
        at++;
    }
    while (at < query->cursor && (query->source[at] == ' ' || query->source[at] == '\t')) {
        at++;
    }
    return at == query->cursor && from < query->cursor;
}

/* Whether the field ending at `end` is a member of an attribute block
 * that only spaces, tabs and a line ending follow up to the cursor. */
static bool member(const context_query *query, const markdown_core_context_field *field,
                   const markdown_core_context_opener *opener, size_t end) {
    const markdown_core_slot_kind kind = field->kind;
    if (opener->kind != MARKDOWN_CORE_KIND_NONE || query->source[opener->place.start] != '{' ||
        (kind != MARKDOWN_CORE_SLOT_ID && kind != MARKDOWN_CORE_SLOT_CLASS && kind != MARKDOWN_CORE_SLOT_RECORD &&
         kind != MARKDOWN_CORE_SLOT_VALUE)) {
        return false;
    }
    return blank(query, end);
}

/* What the query makes of a field: whether the document keeps it, whether
 * its construct is closed, and where it ends on the cursor's line. */
typedef struct verdict {
    bool kept, closed;
    markdown_core_slot_kind kind;
    size_t start, end;
} verdict;

/* The verdict on the field at `index`, whose line runs from `line` to
 * `last`; false when the confirmation ran out of memory. */
static bool weigh(const context_query *query, const markdown_core_context_sink *sink, size_t index, size_t line,
                  size_t last, verdict *out) {
    const markdown_core_context_field *field = &sink->fields[index];
    const markdown_core_context_opener *opener = &sink->openers[field->opener];
    bool kept, closed;
    *out = (verdict){false, false, field->kind, field->start, 0};
    if (field->start > query->cursor) {
        return true;
    }
    /* An inline construct begins at its opener, a fenced directive at its
     * colons, which may instead close one around it, and a footnote or a
     * reference definition at its bracket, which an inline note or a link
     * may instead begin inside. */
    const bool defines = field->declares &&
                         (field->owner == MARKDOWN_CORE_KIND_FOOTNOTE || field->owner == MARKDOWN_CORE_KIND_REFERENCE);
    size_t begins =
        (opener->kind == field->owner && (!opener->block || field->owner == MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK)) ||
                defines
            ? opener->place.start
            : SIZE_MAX;
    if (field->declares && field->owner == MARKDOWN_CORE_KIND_SPECIMEN && field->start) {
        /* A specimen definition begins at the `(` before its number and
         * its `@`, which the content of another may hold as text. */
        begins = field->start - 1;
        while (begins > 0 && query->source[begins - 1] >= '0' && query->source[begins - 1] <= '9') {
            begins--;
        }
        begins -= begins > 0 && query->source[begins - 1] == '(';
    }
    const markdown_core_node_kind owner = field->owner;
    const bool tail = owner == MARKDOWN_CORE_KIND_HEADING || owner == MARKDOWN_CORE_KIND_CODE_BLOCK ||
                      owner == MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK;
    size_t past = SIZE_MAX;
    const size_t brace = opener->place.start;
    if (opener->kind == MARKDOWN_CORE_KIND_NONE && !tail && brace < query->length && query->source[brace] == '{') {
        markdown_core_attribute_parser block = {.data = query->source + brace,
                                                .length = (bufsize_t)(query->length - brace)};
        const bufsize_t at = markdown_core_attributes_end(&block, 0);
        const int oom = block.oom;
        markdown_core_attribute_parser_free(&block);
        if (oom) {
            return false;
        }
        past = brace + (size_t)at;
    }
    if (!confirm(query, owner, field->start, field->kind, opener->place.start, begins, brace, past, &kept, &closed)) {
        return false;
    }
    /* A member the document's node does not carry, an `#id` a later one
     * replaces, may still be done, and the next one the cursor's: the
     * construct is weighed alone. */
    bool done = false;
    if (!kept && query->cursor > field->start && member(query, field, opener, query->cursor - 1) &&
        (query->source[query->cursor - 1] == ' ' || query->source[query->cursor - 1] == '\t')) {
        if (!confirm(query, owner, field->start, MARKDOWN_CORE_SLOT_RECORD, opener->place.start, begins, brace, past,
                     &kept, &closed)) {
            return false;
        }
        done = kept;
    }
    if (!kept || (field->declares && !closed)) {
        return true;
    }
    /* A block's attribute tail is its only when nothing but spaces follows
     * it on the line, and when no literal node, a code span, holds its
     * brace as text. */
    if (opener->kind == MARKDOWN_CORE_KIND_NONE && tail) {
        if (!tail_ends(query, opener->place.start, last, owner == MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK, &kept)) {
            return false;
        }
        if (!kept) {
            return true;
        }
        const markdown_core_node **path = NULL;
        size_t count = 0;
        if (!query_holders(query, opener->place.start, &path, &count)) {
            return false;
        }
        for (size_t i = 0; i < count && kept; i++) {
            kept = !literal(path[i]);
        }
        markdown_core_free((void *)path);
        if (!kept) {
            return true;
        }
    }
    markdown_core_context_field moved = *field;
    size_t origin = field->start - field->lead;
    /* A quoted value may close on a later line of its block, and only what
     * follows tells whether a brace before that closes the block. */
    const bool quoted = field->kind == MARKDOWN_CORE_SLOT_VALUE && origin < query->length &&
                        (query->source[origin] == '"' || query->source[origin] == '\'');
    bufsize_t reach =
        field->end(query->source + origin, (bufsize_t)((quoted ? block_end(query, last) : last) - origin));
    if (reach < 0) {
        return false;
    }
    if (field->kind == MARKDOWN_CORE_SLOT_DEST && !reach && !field->lead && blank(query, field->start)) {
        /* A destination not yet begun begins after the spaces and the line
         * ending the run dropped from its end: at the cursor. */
        moved.start = query->cursor;
        field = &moved;
        origin = query->cursor;
        reach = field->end(query->source + origin, (bufsize_t)(last - origin));
        if (reach < 0) {
            return false;
        }
    }
    const size_t end = origin + (size_t)reach;
    /* An info string's word stops short of the cursor only at an attribute
     * tail the cursor is in, which its own field reads. */
    if (field->kind == MARKDOWN_CORE_SLOT_INFO && end < query->cursor) {
        return true;
    }
    if (!marked(query, field, last, end)) {
        return true;
    }
    if (done && !(end < query->cursor && member(query, field, opener, end))) {
        return true;
    }
    if (end < query->cursor && member(query, field, opener, end)) {
        /* The run dropped the spaces at its end, so it reads the member
         * before them as the one cut short: that member is done, and the
         * next begins at the cursor. */
        const bufsize_t name =
            markdown_core_attributes_name_end(query->source + query->cursor, (bufsize_t)(last - query->cursor));
        *out = (verdict){true, closed, MARKDOWN_CORE_SLOT_RECORD, query->cursor, query->cursor + (size_t)name};
        return true;
    }
    if (end < query->cursor && field->owner != MARKDOWN_CORE_KIND_TOPIC) {
        return true;
    }
    if (!spaced(query, end)) {
        *out = (verdict){true, closed, field->kind, field->start, end};
    }
    return true;
}

/* The verdicts on the fields of `sink`. A field another reads before it is
 * text of that field, not one of its own: one that reads the same slot from
 * the same start as a field the document keeps under an opener that starts
 * no later, as the `[[` of a `![[` reads the target the `![[` reads, and
 * one that starts inside a
 * field of the same slot the document keeps, as an `@{` inside a braced key.
 * A declaration's field neither reads before another nor is read: it stands
 * only when the document holds the declaration. One sort orders them, so
 * the fields one reads are passed over without a scan of their own. */
static bool judge(const context_query *query, const markdown_core_context_sink *sink, size_t line, size_t last,
                  verdict *verdicts) {
    reading *readings = markdown_core_alloc(sink->field_count, sizeof(*readings));
    if (!readings) {
        return false;
    }
    size_t count = 0;
    for (size_t i = 0; i < sink->field_count; i++) {
        const markdown_core_context_field *field = &sink->fields[i];
        if (!field->declares) {
            readings[count++] = (reading){field->kind, field->start, sink->openers[field->opener].place.start, i};
        } else if (!weigh(query, sink, i, line, last, &verdicts[i])) {
            markdown_core_free(readings);
            return false;
        }
    }
    qsort(readings, count, sizeof(*readings), reading_order);
    size_t reach = 0, taken = SIZE_MAX;
    verdict *reader = NULL;
    for (size_t i = 0; i < count; i++) {
        const bool kind = i && readings[i].kind == readings[i - 1].kind;
        if (!kind) {
            reach = 0;
            taken = SIZE_MAX;
            reader = NULL;
        }
        /* A field read inside one the document leaves open is weighed
         * still: one it finishes is the field, and the open one is not. */
        const bool open = reader && !reader->closed;
        if ((readings[i].start == taken || readings[i].start < reach) && !open) {
            continue;
        }
        verdict *v = &verdicts[readings[i].index];
        if (!weigh(query, sink, readings[i].index, line, last, v)) {
            markdown_core_free(readings);
            return false;
        }
        if (v->kept && (readings[i].start == taken || readings[i].start < reach)) {
            if (!v->closed) {
                v->kept = false;
                continue;
            }
            reader->kept = false;
        }
        if (v->kept) {
            taken = readings[i].start;
            reach = v->end > reach ? v->end : reach;
            reader = v;
        }
    }
    markdown_core_free(readings);
    return true;
}

/* The context's slots: each field the document confirms, with its source
 * before and after the cursor, and its prefixes, copied. */
static bool take_slots(markdown_core_context *context, const context_query *query,
                       const markdown_core_context_sink *sink) {
    if (!sink->field_count) {
        return true;
    }
    size_t bytes = 0, prefixes = 0;
    for (size_t i = 0; i < sink->field_count; i++) {
        for (uint32_t j = 0; j < sink->fields[i].prefix_count; j++) {
            bytes += sink->fields[i].prefix[j].end - sink->fields[i].prefix[j].start;
            prefixes++;
        }
    }
    size_t line = query->cursor;
    const size_t last = line_end(query, query->cursor);
    while (line > 0 && query->source[line - 1] != '\n' && query->source[line - 1] != '\r') {
        line--;
    }
    verdict *verdicts = markdown_core_alloc(sink->field_count, sizeof(*verdicts));
    if (!verdicts || !judge(query, sink, line, last, verdicts)) {
        markdown_core_free(verdicts);
        return false;
    }
    context->slots = markdown_core_alloc(sink->field_count, sizeof(*context->slots));
    context->prefixes = prefixes ? markdown_core_alloc(prefixes, sizeof(*context->prefixes)) : NULL;
    context->bytes = bytes ? markdown_core_alloc(bytes, 1) : NULL;
    if (!context->slots || (prefixes && !context->prefixes) || (bytes && !context->bytes)) {
        markdown_core_free(verdicts);
        return false;
    }
    /* A slot keeps to the cursor's line: a field that began on an earlier
     * one, a label or a quoted value, is written from past the line's
     * container prefix, and one that goes on to a later line up to the
     * line's end. */
    size_t from = line;
    while (from < query->cursor &&
           (query->source[from] == ' ' || query->source[from] == '\t' || query->source[from] == '>')) {
        from++;
    }
    size_t prefix = 0, written = 0;
    for (size_t i = 0; i < sink->field_count; i++) {
        const markdown_core_context_field *field = &sink->fields[i];
        if (!verdicts[i].kept) {
            continue;
        }
        const size_t start = verdicts[i].start < line ? from : verdicts[i].start;
        const size_t end = verdicts[i].end < last ? verdicts[i].end : last;
        const size_t after = end > query->cursor ? end : query->cursor;
        const uint32_t count = verdicts[i].kind == field->kind ? field->prefix_count : 0;
        context_slot *slot = &context->slots[context->slot_count++];
        *slot = (context_slot){{verdicts[i].kind, field->owner, query_scope(query, start, query->cursor),
                                query_scope(query, query->cursor, after), verdicts[i].closed, count},
                               prefix};
        for (uint32_t j = 0; j < count; j++) {
            const size_t size = field->prefix[j].end - field->prefix[j].start;
            if (size) {
                memcpy(context->bytes + written, query->source + field->prefix[j].start, size);
            }
            context->prefixes[prefix++] = (markdown_core_string){context->bytes + written, size};
            written += size;
        }
    }
    markdown_core_free(verdicts);
    return true;
}

markdown_core_status markdown_core_document_context(const markdown_core_document *document, const uint8_t *source,
                                                    size_t length, markdown_core_position position,
                                                    markdown_core_context **context) {
    if (position.line < 1 || position.column < 1) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    column_mark marks[2] = {{0}};
    tail_memo tail = {0};
    context_query query = {.document = document, .source = source, .length = length, .marks = marks, .tail = &tail};
    if (!markdown_core_source_lines_read(&query.lines, source, length)) {
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    if (!markdown_core_position_offset(&query.lines, source, length, position, document->unit, true, &query.cursor)) {
        markdown_core_free(query.lines.starts);
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    if (!read_holdings(&query)) {
        markdown_core_free(query.holdings);
        markdown_core_free(query.lines.starts);
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    markdown_core_context_sink sink = {.source = source, .length = query.cursor};
    markdown_core_document *before = NULL;
    markdown_core_context *made = markdown_core_alloc(1, sizeof(*made));
    markdown_core_status status = made ? markdown_core_parse_before_cursor(&sink, document->unit, document, &before)
                                       : MARKDOWN_CORE_ALLOCATION_FAILED;
    if (status == MARKDOWN_CORE_OK && (sink.failed || !take_blocks(made, &query, &sink) ||
                                       !take_inlines(made, &query, &sink) || !take_slots(made, &query, &sink))) {
        status = MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    markdown_core_document_free(before);
    markdown_core_context_sink_clear(&sink);
    markdown_core_attribute_parser_free(&tail.parser);
    markdown_core_free(query.holdings);
    markdown_core_free(query.lines.starts);
    if (status != MARKDOWN_CORE_OK) {
        markdown_core_context_free(made);
        return status;
    }
    *context = made;
    return MARKDOWN_CORE_OK;
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

void markdown_core_context_free(markdown_core_context *context) {
    if (!context) {
        return;
    }
    markdown_core_free(context->blocks);
    markdown_core_free(context->inlines);
    markdown_core_free(context->slots);
    markdown_core_free(context->prefixes);
    markdown_core_free(context->bytes);
    markdown_core_free(context);
}
