#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "../include/markdown_core.h"

#include "ast_internal.h"
#include "sink.h"
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
    markdown_core_field_end scan;
    size_t lead;
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

/* Whether the source byte `c` is one of `set`: a NUL, which the parse
 * reads as U+FFFD, is none. */
static bool among(uint8_t c, const char *set) { return c && strchr(set, c); }

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
        return end == query->length || among(query->source[end], "} \t\r\n");
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
 * escape, a reference, a quote or a NUL is passed, since its member holds
 * other bytes than its source. */
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
        while (first + length < query->length && !among(text[length], " \t\r\n}#.=")) {
            length++;
        }
    }
    if (!length) {
        return true;
    }
    for (size_t i = 0; i < length; i++) {
        if (!text[i] || among(text[i], "\\&\"'\r\n")) {
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

/* Whether `node` misses the field of the slot of kind `slot` written from
 * `first` to the cursor (3.3): bytes of it are written, and no field of the
 * node of that kind holds the first of them, so the node does not finish
 * its construct (a callout whose `[!` no `]` closes holds no variant). The
 * written bytes start past the spaces and line endings after `first`, a
 * quoted value's quote and a block anchor's caret, and end at the spaces
 * before the cursor. A citation's key is a bibliography entry's or a
 * specimen's, which only the definitions decide. A quote-led value
 * written from an earlier line also misses unless a value reaches the
 * cursor's line. A slot no field of the node's kind reads, an opener's or a
 * member's of the metadata, is never missed. */
static bool field_misses(const context_query *query, const markdown_core_node *node, markdown_core_slot_kind slot,
                         size_t first, bool *misses) {
    const markdown_core_node_kind kind = markdown_core_node_get_kind(node);
    size_t from = first, end = query->cursor;
    *misses = false;
    if (slot == MARKDOWN_CORE_SLOT_NONE || kind == MARKDOWN_CORE_KIND_METADATA || first > query->cursor) {
        return true;
    }
    while (end > from && (query->source[end - 1] == ' ' || query->source[end - 1] == '\t')) {
        end--;
    }
    while (from < end && among(query->source[from], " \t\r\n>")) {
        from++;
    }
    const bool quoted =
        slot == MARKDOWN_CORE_SLOT_VALUE && from < end && (query->source[from] == '"' || query->source[from] == '\'');
    from += from < end && (query->source[from] == '"' || query->source[from] == '\'' ||
                           (slot == MARKDOWN_CORE_SLOT_ANCHOR && query->source[from] == '^'));
    if (from >= end) {
        return true;
    }
    const bool key =
        kind == MARKDOWN_CORE_KIND_CITATION && (slot == MARKDOWN_CORE_SLOT_BIB || slot == MARKDOWN_CORE_SLOT_SPECIMEN);
    const markdown_core_slot_kind kinds[2] = {slot, slot == MARKDOWN_CORE_SLOT_BIB ? MARKDOWN_CORE_SLOT_SPECIMEN
                                                                                   : MARKDOWN_CORE_SLOT_BIB};
    /* Written from an earlier line, a quote-led value must reach the
     * cursor's: one that no quote closes ends with its own line. */
    const size_t line = query_line(query, query->cursor);
    const size_t reach = quoted && query_line(query, from) < line ? query->lines.starts[line] : 0;
    bool held = false, reached = !reach;
    for (size_t k = 0; k < (key ? 2u : 1u) && !(held && reached); k++) {
        markdown_core_place *places;
        size_t count;
        if (markdown_core_parts_fields(query->document->root, node, kinds[k], query->source, query->length, &places,
                                       &count) == MARKDOWN_CORE_ALLOCATION_FAILED) {
            return false;
        }
        for (size_t i = 0; i < count; i++) {
            held = held || (places[i].start <= from && from < places[i].end);
            reached = reached || (places[i].start < query->cursor && places[i].end >= reach);
        }
        markdown_core_free(places);
    }
    *misses = !held || !reached;
    return true;
}

/* Whether the document holds a node of kind `owner` whose source holds the
 * byte at `first` and the last byte before the cursor, or the cursor's byte
 * when the field is empty: the construct the field belongs to, which the
 * document finishes at or after the cursor. At the document's end, where no
 * byte follows, the last byte stands for both. An identifier's `slot` names
 * its node, which then carries the anchor written at `first`; a class's or
 * a value's in an inline block is one that `carries` it, and a citation's
 * one it `refers` to. A node that `field_misses` the field written does not
 * finish its construct: a block's, which holds the whole opening line, is
 * exact so.
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
    const size_t written = first;
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
        bool misses = false;
        if (markdown_core_node_get_kind(at_probe[i]) != owner) {
            continue;
        }
        if (!field_misses(query, at_probe[i], slot, written, &misses)) {
            markdown_core_free((void *)at_probe);
            markdown_core_free((void *)at_first);
            return false;
        }
        if (misses) {
            continue;
        }
        if ((slot == MARKDOWN_CORE_SLOT_ID && !names(query, &at_probe[i]->attributes.anchor, first, brace)) ||
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
    /* A block's attribute tail the document leaves open is text of its
     * line, which the block holds itself: a fence's info string. */
    if (text && query->source[opener] == '{' && markdown_core_node_get_kind(text) == owner &&
        (owner == MARKDOWN_CORE_KIND_CODE_BLOCK || owner == MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK)) {
        *kept = true;
        return true;
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

/* Notes what `opener`, kept, leaves open at the start of the cursor's line
 * when it opens on an earlier one: a directive's label, or a quote of its
 * attribute block that no quote of its kind closes before the line. */
static void leave(markdown_core_context *context, const context_query *query,
                  const markdown_core_context_opener *opener) {
    const size_t line = query->lines.starts[query_line(query, query->cursor)];
    if (opener->place.end > line || !opener->place.end) {
        return;
    }
    const uint8_t c = query->source[opener->place.end - 1];
    /* A `[` still open to more than one kind is a directive's label after
     * a `:` and a name. */
    size_t name = opener->place.end - 1;
    while (name > 0 && !among(query->source[name - 1], " \t\r\n[{:")) {
        name--;
    }
    if (c == '[' && (opener->kind == MARKDOWN_CORE_KIND_DIRECTIVE ||
                     (opener->kind == MARKDOWN_CORE_KIND_NONE && name + 1 < opener->place.end && name > 0 &&
                      query->source[name - 1] == ':'))) {
        context->label = true;
    } else if (c == '{' && opener->kind == MARKDOWN_CORE_KIND_NONE) {
        uint8_t quote = 0;
        for (size_t at = opener->place.end; at < line; at++) {
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
            leave(context, query, &sorted[i]);
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

/* Cuts `*end` to where the innermost block that holds the byte at `offset`
 * ends, when that is sooner: a paragraph a fence interrupts ends before the
 * fence. False when the walk failed. */
static bool held_end(const context_query *query, size_t offset, size_t *end) {
    const markdown_core_node **path = NULL;
    size_t count = 0;
    if (!query_holders(query, offset, &path, &count)) {
        return false;
    }
    for (size_t i = count; i > 0; i--) {
        if (MARKDOWN_CORE_NODE_BLOCK_P((markdown_core_node *)path[i - 1])) {
            const size_t close = path[i - 1]->place.end;
            *end = close > offset && close < *end ? close : *end;
            break;
        }
    }
    markdown_core_free((void *)path);
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
        /* The attribute block ends with the block that holds its brace. */
        size_t limit = query->length;
        if (!held_end(query, brace, &limit)) {
            return false;
        }
        markdown_core_attribute_parser block = {.data = query->source + brace, .length = (bufsize_t)(limit - brace)};
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
        if (!confirm(query, owner, field->start, MARKDOWN_CORE_SLOT_NONE, opener->place.start, begins, brace, past,
                     &kept, &closed)) {
            return false;
        }
        done = kept;
    }
    /* A quote-led value begun on an earlier line that the document's node
     * does not carry to the cursor's ended with its own line, as no quote
     * closes it: a name on the cursor's line begins the next member. */
    const size_t row = query_line(query, query->cursor);
    if (!kept && field->kind == MARKDOWN_CORE_SLOT_VALUE && query_line(query, field->start) < row &&
        among(query->source[field->start - field->lead], "\"'")) {
        size_t at = query->lines.starts[row];
        while (at < query->cursor && among(query->source[at], " \t>")) {
            at++;
        }
        if (at + (size_t)markdown_core_attributes_name_end(query->source + at, (bufsize_t)(query->cursor - at)) ==
            query->cursor) {
            if (!confirm(query, owner, at, MARKDOWN_CORE_SLOT_NONE, opener->place.start, begins, brace, past, &kept,
                         &closed)) {
                return false;
            }
            if (kept) {
                const bufsize_t name = markdown_core_attributes_name_end(query->source + at, (bufsize_t)(last - at));
                *out = (verdict){true, closed, MARKDOWN_CORE_SLOT_RECORD, at, at + (size_t)name};
            }
            return true;
        }
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
        /* A container's braces follow its name or label at once; only a
         * nameless one's follow spaces after the colons. */
        if (owner == MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK && brace > 0 &&
            (query->source[brace - 1] == ' ' || query->source[brace - 1] == '\t')) {
            size_t before = brace - 1;
            while (before > line && (query->source[before - 1] == ' ' || query->source[before - 1] == '\t')) {
                before--;
            }
            kept = kept && before > line && query->source[before - 1] == ':';
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
    size_t stop = quoted ? block_end(query, last) : last;
    if (quoted && !held_end(query, origin, &stop)) {
        return false;
    }
    bufsize_t reach = field->end(query->source + origin, (bufsize_t)((stop > last ? stop : last) - origin));
    if (reach < 0) {
        return false;
    }
    if (field->kind == MARKDOWN_CORE_SLOT_DEST && !reach && !field->lead && blank(query, field->start)) {
        /* A destination not yet begun begins after the spaces and the line
         * ending the run dropped from its end: at the cursor. */
        moved.start = (uint32_t)query->cursor;
        field = &moved;
        origin = query->cursor;
        reach = field->end(query->source + origin, (bufsize_t)(last - origin));
        if (reach < 0) {
            return false;
        }
    }
    const size_t end = origin + (size_t)reach;
    if (field->owner == MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK && field->kind == MARKDOWN_CORE_SLOT_NAME) {
        /* After a container's name only its label and braces follow at
         * once, and spaces to the line's end. */
        size_t after = end;
        while (after < last && (query->source[after] == ' ' || query->source[after] == '\t')) {
            after++;
        }
        if (after > end && after < last) {
            return true;
        }
    }
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
    /* A field that begins inside a closed field of raw text, as an
     * attribute's value or name, is text of it, and one whose opener is
     * inside a reference definition before its destination, the `:` after
     * its label, is the definition's. A label is not raw: a link's text
     * may be its label, and a fence's attribute tail is read from its
     * info string. */
    for (size_t i = 0; i < sink->field_count; i++) {
        const verdict *v = &verdicts[i];
        const markdown_core_context_field *field = &sink->fields[i];
        const bool raw = v->kind != MARKDOWN_CORE_SLOT_LABEL && v->kind != MARKDOWN_CORE_SLOT_INFO && v->closed;
        const bool defines =
            field->owner == MARKDOWN_CORE_KIND_REFERENCE && v->kind == MARKDOWN_CORE_SLOT_DEST && field->opener >= 0;
        if (!v->kept || (!raw && !defines)) {
            continue;
        }
        const size_t from = defines ? sink->openers[field->opener].place.start : v->start;
        for (size_t other = 0; other < sink->field_count; other++) {
            if (defines && sink->fields[other].opener < 0) {
                continue;
            }
            const size_t at = defines ? sink->openers[sink->fields[other].opener].place.start : verdicts[other].start;
            /* A bare value reads on to the brace a field inside it may
             * begin before. */
            const size_t end = defines ? v->start : v->end + (v->kind == MARKDOWN_CORE_SLOT_VALUE);
            if (other != i && verdicts[other].kept && !sink->fields[other].declares && at > from && at < end) {
                verdicts[other].kept = false;
            }
        }
    }
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
    context->size = last - line;
    context->line = markdown_core_alloc(context->size + 1, 1);
    if (!context->slots || (prefixes && !context->prefixes) || (bytes && !context->bytes) || !context->line) {
        markdown_core_free(verdicts);
        return false;
    }
    memcpy(context->line, query->source + line, context->size);
    const markdown_core_node **path = NULL;
    size_t depth = 0;
    const size_t held = query->cursor > line ? query->cursor - 1 : query->cursor;
    if (held < query->length && !query_holders(query, held, &path, &depth)) {
        markdown_core_free(verdicts);
        return false;
    }
    for (size_t i = 0; i < depth && !context->row; i++) {
        context->row = markdown_core_node_get_kind(path[i]) == MARKDOWN_CORE_KIND_TABLE_CELL;
    }
    markdown_core_free((void *)path);
    /* An autolink holds its text as written: what the run read as fields
     * inside it is text, though the autolink is a link. */
    bool autolinked = false;
    for (size_t i = 0; i < query->holding_count && !autolinked; i++) {
        const holding *at = &query->holdings[i];
        if (at->start < held && held < at->end && query->source[at->start] == '<' &&
            markdown_core_node_get_kind(at->node) == MARKDOWN_CORE_KIND_LINK) {
            /* Its attributes follow its `>`. */
            const uint8_t *close = memchr(query->source + at->start, '>', at->end - at->start);
            autolinked = !close || query->source + held < close;
        }
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
        if (!verdicts[i].kept || autolinked) {
            continue;
        }
        const size_t start = verdicts[i].start < line ? from : verdicts[i].start;
        const size_t end = verdicts[i].end < last ? verdicts[i].end : last;
        const size_t after = end > query->cursor ? end : query->cursor;
        const uint32_t count = verdicts[i].kind == field->kind ? field->prefix_count : 0;
        context_slot *slot = &context->slots[context->slot_count++];
        *slot = (context_slot){{verdicts[i].kind, field->owner, query_scope(query, start, query->cursor),
                                query_scope(query, query->cursor, after), verdicts[i].closed, count},
                               prefix,
                               verdicts[i].start < line ? SIZE_MAX : verdicts[i].start - line,
                               start - line,
                               query->cursor - line,
                               after - line,
                               SIZE_MAX,
                               NULL,
                               0,
                               0,
                               verdicts[i].start < line || verdicts[i].end > last};
        const size_t opener = field->opener >= 0 ? sink->openers[field->opener].place.start : SIZE_MAX;
        slot->opener = opener >= line && opener < last ? opener - line : SIZE_MAX;
        const markdown_core_slot_kind kind = verdicts[i].kind;
        const size_t origin = verdicts[i].start - (kind == MARKDOWN_CORE_SLOT_ANCHOR ? field->lead : 0);
        if ((kind == MARKDOWN_CORE_SLOT_DEST || kind == MARKDOWN_CORE_SLOT_ANCHOR) && origin > line &&
            query->source[origin - 1] == '<' && field->owner != MARKDOWN_CORE_KIND_CROSS_LINK &&
            field->owner != MARKDOWN_CORE_KIND_CROSS_EMBEDDED) {
            slot->wrap = '<';
        } else if ((kind == MARKDOWN_CORE_SLOT_BIB || kind == MARKDOWN_CORE_SLOT_SPECIMEN) && start > line + 1 &&
                   query->source[start - 1] == '{' && query->source[start - 2] == '@') {
            slot->wrap = '{';
        } else if (kind == MARKDOWN_CORE_SLOT_VALUE && verdicts[i].start < line &&
                   (query->source[verdicts[i].start] == '"' || query->source[verdicts[i].start] == '\'')) {
            slot->wrap = query->source[verdicts[i].start];
        }
        /* The field's own scanner reads a spelling, from where it reads the
         * field: a member the run cut short is read by another. */
        if (verdicts[i].kind == field->kind && verdicts[i].start >= line + field->lead &&
            (verdicts[i].start == field->start || !field->lead)) {
            slot->scan = field->end;
            slot->lead = field->lead;
        }
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
 * no field yet. Answers whether it found a slot, as `*kind`, `*from`,
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
            const size_t first = query->lines.starts[query_line(query, at->from)];
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
    if (start != cursor || !begins || (end > line && query->source[line] == '-')) {
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
    markdown_core_metadata_places places = {0};
    if (!markdown_core_metadata_places_read(query->source, metadata->place.end, &places)) {
        markdown_core_free(places.items);
        return false;
    }
    const size_t line = query->lines.starts[query_line(query, cursor)], last = line_end(query, cursor);
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
                       NULL,
                       0,
                       wrap,
                       false};
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
    bool inside = false;
    markdown_core_status status =
        made && take_envelope(made, &query, &inside) ? MARKDOWN_CORE_OK : MARKDOWN_CORE_ALLOCATION_FAILED;
    if (status == MARKDOWN_CORE_OK && !inside) {
        status = markdown_core_parse_before_cursor(&sink, document->unit, document, &before);
    }
    if (status == MARKDOWN_CORE_OK && !inside &&
        (sink.failed || !take_blocks(made, &query, &sink) || !take_inlines(made, &query, &sink) ||
         !take_slots(made, &query, &sink))) {
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
    markdown_core_free(context->line);
    markdown_core_free(context);
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
        const char marked[] = {'\\', (char)slot->wrap, '`', '$', '<', '>', '[', ']', '(', ')', '{', '}', 0};
        escaped(out, value, size, marked, row);
        put(out, slot->wrap);
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

/* Whether the field's own scanner, reading `text` with `tail` after it,
 * ends exactly where `text` does. */
static bool scans(const markdown_core_context *context, const context_slot *slot, const spelled *text,
                  const uint8_t *tail, size_t rest, bool *ok) {
    *ok = true;
    if (!slot->scan) {
        return true;
    }
    const size_t total = slot->lead + text->size + rest;
    uint8_t *data = markdown_core_alloc(total + 1, 1);
    if (!data) {
        return false;
    }
    memcpy(data, context->line + slot->field - slot->lead, slot->lead);
    memcpy(data + slot->lead, text->text, text->size);
    if (rest) {
        memcpy(data + slot->lead + text->size, tail, rest);
    }
    const bufsize_t end = slot->scan(data, (bufsize_t)total);
    markdown_core_free(data);
    if (end < 0) {
        return false;
    }
    *ok = (size_t)end == slot->lead + text->size;
    return true;
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
            /* A key the bare scanner does not read whole is written in
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
        break;
    case MARKDOWN_CORE_SLOT_VALUE:
        held = !closes(value, size) && spell_attribute(&out, slot, context->line, row, value, size);
        closer = unfinished(context, slot, tail, rest, '}') ? "}" : "";
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
    if (held && !scans(context, slot, &out, tail, rest, &whole)) {
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
