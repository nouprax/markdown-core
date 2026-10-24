#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "../include/markdown_core.h"

#include "ast_internal.h"
#include "sink.h"
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
} context_query;

static markdown_core_scope query_scope(const context_query *query, size_t start, size_t end) {
    return markdown_core_source_scope(&query->lines, query->source,
                                      (markdown_core_place){(uint32_t)start, (uint32_t)end}, query->document->unit);
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
 * of a block identifier, or the brace or separator after an attribute
 * block's `#id`: the one the field declares, not another the node carries. */
static bool names(const context_query *query, const markdown_core_chunk *anchor, size_t first) {
    const size_t length = (size_t)anchor->len, end = first + length;
    return length && end <= query->length && !memcmp(query->source + first, anchor->data, length) &&
           (end == query->length || (query->source[end] && strchr("#} \t\r\n", query->source[end])));
}

/* Whether the document holds a node of kind `owner` whose source holds the
 * byte at `first` and the last byte before the cursor, or the cursor's byte
 * when the field is empty: the construct the field belongs to, which the
 * document finishes at or after the cursor. At the document's end, where no
 * byte follows, the last byte stands for both. An `anchored` construct
 * names its node, which then carries the anchor written at `first`. */
static bool finished_by(const context_query *query, markdown_core_node_kind owner, size_t first, bool anchored,
                        bool *found) {
    size_t probe = query->cursor > first || query->cursor >= query->length ? query->cursor - 1 : query->cursor;
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
            (anchored && !names(query, &at_probe[i]->attributes.anchor, first))) {
            continue;
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
static bool confirm(const context_query *query, markdown_core_node_kind owner, size_t first, bool anchored,
                    size_t opener, bool *kept, bool *closed) {
    *kept = *closed = false;
    if (!finished_by(query, owner, first, anchored, closed)) {
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
    for (size_t i = 0; i < sink->block_count; i++) {
        const markdown_core_node *block = sink->blocks[i];
        size_t start = block->place.start < query->cursor ? block->place.start : query->cursor;
        size_t end = line_end(query, start);
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
        if (!confirm(query, sorted[i].kind, sorted[i].place.start, false, sorted[i].place.start, &kept, &closed)) {
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

/* Marks each field an earlier field of `sink` shadows: one that reads the
 * same slot from the same start under an opener that starts no later. The
 * `[[` of a `![[` reads the target the `![[` reads, and the earlier opener's
 * reading is the one the text has. A declaration's field neither shadows nor
 * is shadowed: it stands only when the document holds the declaration. One
 * sort finds them all. */
static bool read_before(const markdown_core_context_sink *sink, bool *shadowed) {
    reading *readings = markdown_core_alloc(sink->field_count, sizeof(*readings));
    if (!readings) {
        return false;
    }
    size_t count = 0;
    for (size_t i = 0; i < sink->field_count; i++) {
        const markdown_core_context_field *field = &sink->fields[i];
        if (!field->declares) {
            readings[count++] = (reading){field->kind, field->start, sink->openers[field->opener].place.start, i};
        }
    }
    qsort(readings, count, sizeof(*readings), reading_order);
    for (size_t i = 1; i < count; i++) {
        if (readings[i].kind == readings[i - 1].kind && readings[i].start == readings[i - 1].start) {
            shadowed[readings[i].index] = true;
        }
    }
    markdown_core_free(readings);
    return true;
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

/* The context's slots: each field the document confirms, with its source
 * before and after the cursor, and its prefixes, copied. */
/* Whether the attribute block whose brace is at `brace` ends the line of a
 * block that takes it as its tail, in `*ends`: past its closing brace, only
 * spaces and tabs, then a heading's closing hashes after a space or a
 * directive's colons. A block the line leaves open is still being written
 * when nothing but spaces and tabs follows the cursor. False when
 * recognition ran out of memory. */
static bool tail_ends(const context_query *query, markdown_core_node_kind owner, size_t brace, size_t line,
                      bool *ends) {
    markdown_core_attribute_parser attributes = {.data = query->source + brace, .length = (bufsize_t)(line - brace)};
    bufsize_t at = markdown_core_attributes_end(&attributes, 0);
    const int oom = attributes.oom;
    markdown_core_attribute_parser_free(&attributes);
    size_t end = at ? brace + (size_t)at : query->cursor, spaces = end;
    *ends = false;
    if (oom) {
        return false;
    }
    while (end < line && (query->source[end] == ' ' || query->source[end] == '\t')) {
        end++;
    }
    const unsigned char close = owner == MARKDOWN_CORE_KIND_HEADING           ? '#'
                                : owner == MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK ? ':'
                                                                              : 0;
    if (at && close && end < line && query->source[end] == close && (close == ':' || end > spaces)) {
        while (end < line && query->source[end] == close) {
            end++;
        }
        while (close == '#' && end < line && (query->source[end] == ' ' || query->source[end] == '\t')) {
            end++;
        }
    }
    *ends = end == line;
    return true;
}

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
    bool *shadowed = markdown_core_alloc(sink->field_count, sizeof(*shadowed));
    if (!shadowed || !read_before(sink, shadowed)) {
        markdown_core_free(shadowed);
        return false;
    }
    context->slots = markdown_core_alloc(sink->field_count, sizeof(*context->slots));
    context->prefixes = prefixes ? markdown_core_alloc(prefixes, sizeof(*context->prefixes)) : NULL;
    context->bytes = bytes ? markdown_core_alloc(bytes, 1) : NULL;
    if (!context->slots || (prefixes && !context->prefixes) || (bytes && !context->bytes)) {
        markdown_core_free(shadowed);
        return false;
    }
    size_t prefix = 0, written = 0, line = query->cursor;
    const size_t cursor_line = line_end(query, query->cursor);
    while (line > 0 && query->source[line - 1] != '\n' && query->source[line - 1] != '\r') {
        line--;
    }
    for (size_t i = 0; i < sink->field_count; i++) {
        const markdown_core_context_field *field = &sink->fields[i];
        if (field->start > query->cursor || field->start < line || shadowed[i]) {
            continue;
        }
        bool kept, closed;
        if (!confirm(query, field->owner, field->start, field->kind == MARKDOWN_CORE_SLOT_ID,
                     sink->openers[field->opener].place.start, &kept, &closed)) {
            markdown_core_free(shadowed);
            return false;
        }
        if (!kept || (field->declares && !closed)) {
            continue;
        }
        /* A block's attribute tail is its only when nothing but its closing
         * syntax follows it on the line. */
        const markdown_core_node_kind owner = field->owner;
        if (sink->openers[field->opener].kind == MARKDOWN_CORE_KIND_NONE &&
            (owner == MARKDOWN_CORE_KIND_HEADING || owner == MARKDOWN_CORE_KIND_CODE_BLOCK ||
             owner == MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK)) {
            if (!tail_ends(query, owner, sink->openers[field->opener].place.start, cursor_line, &kept)) {
                markdown_core_free(shadowed);
                return false;
            }
            if (!kept) {
                continue;
            }
        }
        const size_t end =
            field->start + (size_t)field->end(query->source + field->start, (bufsize_t)(cursor_line - field->start));
        if (spaced(query, end)) {
            continue;
        }
        const size_t after = end > query->cursor ? end : query->cursor;
        context_slot *slot = &context->slots[context->slot_count++];
        *slot = (context_slot){{field->kind, field->owner, query_scope(query, field->start, query->cursor),
                                query_scope(query, query->cursor, after), closed, field->prefix_count},
                               prefix};
        for (uint32_t j = 0; j < field->prefix_count; j++) {
            const size_t size = field->prefix[j].end - field->prefix[j].start;
            if (size) {
                memcpy(context->bytes + written, query->source + field->prefix[j].start, size);
            }
            context->prefixes[prefix++] = (markdown_core_string){context->bytes + written, size};
            written += size;
        }
    }
    markdown_core_free(shadowed);
    return true;
}

markdown_core_status markdown_core_document_context(const markdown_core_document *document, const uint8_t *source,
                                                    size_t length, markdown_core_position position,
                                                    markdown_core_context **context) {
    if (position.line < 1 || position.column < 1) {
        return MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    context_query query = {.document = document, .source = source, .length = length};
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
    markdown_core_status status =
        made ? markdown_core_parse_before_cursor(&sink, document->unit, &before) : MARKDOWN_CORE_ALLOCATION_FAILED;
    if (status == MARKDOWN_CORE_OK && (sink.failed || !take_blocks(made, &query, &sink) ||
                                       !take_inlines(made, &query, &sink) || !take_slots(made, &query, &sink))) {
        status = MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    markdown_core_document_free(before);
    markdown_core_context_sink_clear(&sink);
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
