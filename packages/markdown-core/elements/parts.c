#include <string.h>

#include "alloc.h"
#include "../include/markdown_core.h"

#include "ast_internal.h"
#include "parts.h"
#include "markdown_core_ctype.h"

/* FIELDS AND MARKERS (docs/plans/2026-10-23-lsp-completion-engine.md, 3.3).
 * A node's own source is its runs less the runs of the nodes it holds. Its
 * element's reader reads it again, with the scanners that read it in the
 * parse, and names its parts, the fields; the rest of it is its markers.
 * Nothing of this is kept in the tree: a query reads one node, as the
 * scope query does. */

void markdown_core_parts_add(markdown_core_parts *parts, markdown_core_slot_kind slot, bufsize_t start, bufsize_t end) {
    if (start >= end || start < 0 || end > parts->length) {
        return;
    }
    markdown_core_part *grown =
        markdown_core_reserve(parts->items, &parts->capacity, parts->count + 1, sizeof(*parts->items));
    if (!grown) {
        parts->failed = true;
        return;
    }
    parts->items = grown;
    parts->items[parts->count++] = (markdown_core_part){slot, start, end, false};
}

void markdown_core_parts_lend(markdown_core_parts *parts, markdown_core_slot_kind slot, bufsize_t start,
                              bufsize_t end) {
    const size_t count = parts->count;
    markdown_core_parts_add(parts, slot, start, end);
    if (parts->count > count) {
        parts->items[count].lent = true;
    }
}

bufsize_t markdown_core_parts_skip(const markdown_core_parts *parts, bufsize_t at) {
    for (size_t i = 0; i < parts->held_count; i++) {
        if ((bufsize_t)parts->held[i].start == at && parts->held[i].end > parts->held[i].start) {
            at = (bufsize_t)parts->held[i].end;
        }
    }
    return at;
}

bufsize_t markdown_core_parts_next(const markdown_core_parts *parts, bufsize_t at) {
    for (size_t i = 0; i < parts->held_count; i++) {
        if ((bufsize_t)parts->held[i].start >= at) {
            return (bufsize_t)parts->held[i].start;
        }
    }
    return parts->length;
}

bool markdown_core_parts_within(const markdown_core_parts *parts, bufsize_t at) {
    for (size_t i = 0; i < parts->held_count; i++) {
        if ((bufsize_t)parts->held[i].start <= at && at < (bufsize_t)parts->held[i].end) {
            return true;
        }
    }
    return false;
}

bufsize_t markdown_core_parts_last(const markdown_core_parts *parts, bufsize_t at) {
    for (size_t i = 0; i < parts->held_count; i++) {
        if ((bufsize_t)parts->held[i].end > at) {
            at = (bufsize_t)parts->held[i].end;
        }
    }
    return at;
}

bufsize_t markdown_core_parts_lines(markdown_core_parts *parts, bufsize_t from, const unsigned char *literal,
                                    bufsize_t size) {
    const unsigned char *s = parts->text;
    bufsize_t at = from;
    for (bufsize_t line = 0; line < size && at < parts->length;) {
        bufsize_t next = line;
        while (next < size && literal[next] != '\n' && literal[next] != '\r') {
            next++;
        }
        bufsize_t end = at;
        while (end < parts->length && s[end] != '\n' && s[end] != '\r') {
            end++;
        }
        /* The literal's line is the tail of the text's, and takes the text's
         * line ending, where the text has one. */
        bufsize_t past = end;
        past += past < parts->length && s[past] == '\r';
        past += past < parts->length && s[past] == '\n';
        /* A tab the block took part of stands in the literal as the
         * spaces it left. */
        bufsize_t lead = line;
        while (lead < next && literal[lead] == ' ' &&
               !(next - lead <= end - at && !memcmp(s + end - (next - lead), literal + lead, (size_t)(next - lead)))) {
            lead++;
        }
        const bufsize_t width = next - lead;
        if (width <= end - at && !memcmp(s + end - width, literal + lead, (size_t)width)) {
            markdown_core_parts_add(parts, MARKDOWN_CORE_SLOT_NONE, end - width, past);
        }
        next += next < size && literal[next] == '\r';
        next += next < size && literal[next] == '\n';
        at = past;
        line = next;
    }
    return at;
}

/* Where the `size` bytes at `literal` end, matched in the text from `at`,
 * or -1: an escape the literal decoded, or a table cell's `\|`, stands for
 * the byte it escapes. */
static bufsize_t S_match(const unsigned char *s, bufsize_t n, bufsize_t at, const unsigned char *literal,
                         bufsize_t size) {
    for (bufsize_t i = 0; i < size; i++, at++) {
        if (at + 1 < n && s[at] == '\\' && s[at] != literal[i] && s[at + 1] == literal[i] &&
            markdown_core_ispunct(literal[i])) {
            at++;
        }
        if (at >= n || s[at] != literal[i]) {
            return -1;
        }
    }
    return at;
}

bufsize_t markdown_core_parts_find(markdown_core_parts *parts, bufsize_t from, const unsigned char *literal,
                                   bufsize_t size) {
    const unsigned char *s = parts->text;
    bufsize_t at = from;
    for (bufsize_t line = 0; line < size;) {
        bufsize_t next = line;
        while (next < size && literal[next] != '\n' && literal[next] != '\r') {
            next++;
        }
        bufsize_t found = at, end = -1;
        while (found < parts->length && (end = S_match(s, parts->length, found, literal + line, next - line)) < 0) {
            found++;
        }
        if (end < 0) {
            break;
        }
        const bufsize_t width = next - line;
        next += next < size && literal[next] == '\r';
        next += next < size && literal[next] == '\n';
        /* A line ending the literal holds is the text's. */
        if (next > line + width) {
            end += end < parts->length && s[end] == '\r';
            end += end < parts->length && s[end] == '\n';
        }
        markdown_core_parts_add(parts, MARKDOWN_CORE_SLOT_NONE, found, end);
        at = end;
        line = next;
    }
    return at;
}

void markdown_core_literal_parts(markdown_core_parts *parts) {
    markdown_core_parts_add(parts, MARKDOWN_CORE_SLOT_NONE, 0, parts->length);
}

/* The reader of a node's kind, or NULL for a kind whose own source is all
 * markers. */
static void (*reader(markdown_core_node_kind kind))(markdown_core_parts *) {
    switch (kind) {
    case MARKDOWN_CORE_KIND_TEXT:
    case MARKDOWN_CORE_KIND_HTML:
    case MARKDOWN_CORE_KIND_HTML_BLOCK:
        return markdown_core_literal_parts;
    case MARKDOWN_CORE_KIND_PARAGRAPH:
    case MARKDOWN_CORE_KIND_LIST:
    case MARKDOWN_CORE_KIND_TABLE:
        return markdown_core_block_identifier_parts;
    case MARKDOWN_CORE_KIND_LIST_ITEM:
        return markdown_core_list_item_parts;
    case MARKDOWN_CORE_KIND_LINK:
    case MARKDOWN_CORE_KIND_EMBEDDED:
        return markdown_core_link_parts;
    case MARKDOWN_CORE_KIND_REFERENCE:
        return markdown_core_reference_parts;
    case MARKDOWN_CORE_KIND_CROSS_LINK:
    case MARKDOWN_CORE_KIND_CROSS_EMBEDDED:
        return markdown_core_cross_link_parts;
    case MARKDOWN_CORE_KIND_CODE:
        return markdown_core_code_parts;
    case MARKDOWN_CORE_KIND_CODE_BLOCK:
        return markdown_core_code_block_parts;
    case MARKDOWN_CORE_KIND_FORMULA:
    case MARKDOWN_CORE_KIND_FORMULA_BLOCK:
        return markdown_core_formula_parts;
    case MARKDOWN_CORE_KIND_COMMENT:
        return markdown_core_comment_parts;
    case MARKDOWN_CORE_KIND_HEADING:
        return markdown_core_heading_parts;
    case MARKDOWN_CORE_KIND_CALLOUT:
        return markdown_core_callout_parts;
    case MARKDOWN_CORE_KIND_DIRECTIVE:
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
        return markdown_core_directive_parts;
    case MARKDOWN_CORE_KIND_TOPIC:
        return markdown_core_topic_parts;
    case MARKDOWN_CORE_KIND_CITATION:
        return markdown_core_citation_parts;
    case MARKDOWN_CORE_KIND_FOOTNOTE:
        return markdown_core_footnote_parts;
    case MARKDOWN_CORE_KIND_SPECIMEN:
        return markdown_core_specimen_parts;
    case MARKDOWN_CORE_KIND_SPAN:
        return markdown_core_span_parts;
    case MARKDOWN_CORE_KIND_TABLE_ROW:
        return markdown_core_table_row_parts;
    case MARKDOWN_CORE_KIND_METADATA:
        return markdown_core_metadata_parts;
    default:
        return NULL;
    }
}

/* A NODE AS ITS READER READS IT: its runs, the source they hold joined as
 * its text, and where each run starts in that text, and the ranges of the
 * nodes it holds, in the text. */
typedef struct {
    markdown_core_place *runs;
    size_t run_count;
    bufsize_t *bases;
    unsigned char *text;
    markdown_core_place *held;
    size_t held_count, held_room;
} view;

static void view_free(view *r) {
    markdown_core_free(r->runs);
    markdown_core_free(r->bases);
    markdown_core_free(r->text);
    markdown_core_free(r->held);
}

/* Adds the text range of `start` to `end` to the held ranges. */
static bool hold_place(view *r, bufsize_t start, bufsize_t end) {
    markdown_core_place *grown = markdown_core_reserve(r->held, &r->held_room, r->held_count + 1, sizeof(*r->held));
    if (!grown) {
        return false;
    }
    r->held = grown;
    r->held[r->held_count++] = (markdown_core_place){(uint32_t)start, (uint32_t)end};
    return true;
}

static int place_order(const void *left, const void *right) {
    const markdown_core_place *a = left, *b = right;
    return a->start != b->start ? (a->start < b->start ? -1 : 1) : 0;
}

/* Puts the held ranges in source order, those that meet or overlap merged. */
static void settle_held(view *r) {
    if (!r->held_count) {
        return;
    }
    qsort(r->held, r->held_count, sizeof(*r->held), place_order);
    size_t count = 1;
    for (size_t i = 1; i < r->held_count; i++) {
        markdown_core_place *last = &r->held[count - 1];
        if (r->held[i].start <= last->end) {
            last->end = r->held[i].end > last->end ? r->held[i].end : last->end;
        } else {
            r->held[count++] = r->held[i];
        }
    }
    r->held_count = count;
}

/* Adds a held node's run, as the text ranges of the node's runs it meets: a
 * held node can reach past its holder (an inline note's footnote takes the
 * `^[` and `]` around its citation). */
static bool hold(view *r, markdown_core_place run) {
    for (size_t i = 0; i < r->run_count; i++) {
        const uint32_t start = run.start > r->runs[i].start ? run.start : r->runs[i].start;
        const uint32_t end = run.end < r->runs[i].end ? run.end : r->runs[i].end;
        if (start < end && !hold_place(r, r->bases[i] + (bufsize_t)(start - r->runs[i].start),
                                       r->bases[i] + (bufsize_t)(end - r->runs[i].start))) {
            return false;
        }
    }
    return true;
}

/* Joins the source of `r`'s runs as its text: false when the source ends
 * before the runs do, `*covered` false, or the text could not be allocated. */
static bool join(view *r, const uint8_t *source, size_t length, bool *covered) {
    size_t size = 0;
    for (size_t i = 0; i < r->run_count; i++) {
        r->bases[i] = (bufsize_t)size;
        size += r->runs[i].end - r->runs[i].start;
    }
    if (r->runs[r->run_count - 1].end > length) {
        *covered = false;
        return false;
    }
    if (!(r->text = markdown_core_alloc(size + 1, 1))) {
        return false;
    }
    for (size_t i = 0; i < r->run_count; i++) {
        memcpy(r->text + r->bases[i], source + r->runs[i].start, r->runs[i].end - r->runs[i].start);
    }
    r->text[size] = '\n';
    return true;
}

/* Where `run` begins once the sibling before it, at `before`, has taken its
 * bytes: a node the parse makes rather than reads sits on a byte its
 * sibling holds (a table row's autocompleted cell, at the row's last byte),
 * and owns none of it. */
static markdown_core_place after(markdown_core_place run, markdown_core_place before) {
    if (before.start <= run.start && run.start < before.end) {
        run.start = before.end < run.end ? before.end : run.end;
    }
    return run;
}

/* Appends to the `count` runs of `runs` the bytes of `run` that the cell
 * around the node holds, all of them when there is none: a block in a cell
 * of a table drawn in columns can reach across the cell beside it. Answers
 * the new count. */
static size_t within(markdown_core_place *runs, size_t count, markdown_core_place run, const markdown_core_place *cell,
                     size_t cell_count) {
    if (!cell_count) {
        runs[count++] = run;
        return count;
    }
    for (size_t i = 0; i < cell_count; i++) {
        const uint32_t start = run.start > cell[i].start ? run.start : cell[i].start;
        const uint32_t end = run.end < cell[i].end ? run.end : cell[i].end;
        if (start < end) {
            runs[count++] = (markdown_core_place){start, end};
        }
    }
    return count;
}

/* Notes the item a walk returned as the last at its level: the sibling
 * before the next. False when the notes could not grow. */
static bool note(markdown_core_place **before, size_t *capacity, const markdown_core_walk_item *item) {
    markdown_core_place *grown = markdown_core_reserve(*before, capacity, item->level + 2, sizeof(**before));
    if (!grown) {
        return false;
    }
    *before = grown;
    grown[item->level] = item->node ? item->place : (markdown_core_place){0, 0};
    grown[item->level + 1] = (markdown_core_place){0, 0};
    return true;
}

/* Holds, for a table row at `level` - 1, every cell of the table at
 * `level`: a cell that spans rows is its first row's, and takes bytes of
 * the rows it spans. */
static bool hold_cells(const markdown_core_node *root, view *r, size_t level) {
    markdown_core_walk walk;
    markdown_core_walk_item item;
    markdown_core_walk_begin(&walk, root);
    while (!walk.failed && markdown_core_walk_next(&walk, &item)) {
        const markdown_core_place *ranges;
        size_t count;
        if (item.level != level || !item.node ||
            markdown_core_node_get_kind(item.node) != MARKDOWN_CORE_KIND_TABLE_CELL ||
            item.place.end <= r->runs[0].start || item.place.start >= r->runs[r->run_count - 1].end ||
            !markdown_core_walk_ranges(&walk, &item, &ranges, &count)) {
            continue;
        }
        for (size_t i = 0; i < count && !walk.failed; i++) {
            walk.failed = !hold(r, ranges[i]);
        }
    }
    const bool held = !walk.failed;
    markdown_core_walk_end(&walk);
    return held;
}

/* Reads `node` of the tree `root` from `length` bytes of `source`: its runs,
 * its text and the runs of the nodes it holds, by one walk. False when the
 * source ends before the node does, `*covered` false, or an allocation
 * failed. */
static bool read_node(const markdown_core_node *root, const markdown_core_node *node, const uint8_t *source,
                      size_t length, view *r, bool *covered) {
    markdown_core_walk walk;
    markdown_core_walk_item item;
    markdown_core_place *before = NULL, *cell = NULL;
    size_t level = 0, capacity = 0, cell_count = 0, cell_capacity = 0, cell_level = 0;
    bool in = false, grouped = false;
    *r = (view){0};
    *covered = true;
    markdown_core_walk_begin(&walk, root);
    while (markdown_core_walk_next(&walk, &item)) {
        const markdown_core_place *ranges;
        size_t count;
        if (!in) {
            if (cell_count && item.level <= cell_level) {
                cell_count = 0;
            }
            if (item.node != node) {
                walk.failed = !note(&before, &capacity, &item);
                if (!walk.failed && item.node &&
                    markdown_core_node_get_kind(item.node) == MARKDOWN_CORE_KIND_TABLE_CELL) {
                    walk.failed = !markdown_core_walk_ranges(&walk, &item, &ranges, &count) ||
                                  !(cell = markdown_core_reserve(cell, &cell_capacity, count, sizeof(*cell)));
                    if (!walk.failed) {
                        memcpy(cell, ranges, count * sizeof(*cell));
                        cell_count = count;
                        cell_level = item.level;
                    }
                }
                if (walk.failed) {
                    break;
                }
                continue;
            }
            if (!markdown_core_walk_ranges(&walk, &item, &ranges, &count) ||
                !(r->runs = markdown_core_alloc(count + cell_count, sizeof(*r->runs))) ||
                !(r->bases = markdown_core_alloc(count + cell_count, sizeof(*r->bases)))) {
                walk.failed = true;
                break;
            }
            for (size_t i = 0; i < count; i++) {
                const markdown_core_place run =
                    before && item.level < capacity ? after(ranges[i], before[item.level]) : ranges[i];
                r->run_count = within(r->runs, r->run_count, run, cell, cell_count);
            }
            if (!r->run_count) {
                memcpy(r->runs, ranges, count * sizeof(*ranges));
                r->run_count = count;
            }
            if (!join(r, source, length, covered) || !note(&before, &capacity, &item)) {
                walk.failed = true;
                break;
            }
            level = item.level;
            in = true;
            continue;
        }
        if (item.level <= level) {
            break;
        }
        const markdown_core_place previous =
            before && item.level < capacity ? before[item.level] : (markdown_core_place){0, 0};
        if (!note(&before, &capacity, &item)) {
            walk.failed = true;
            break;
        }
        /* A node of a group of the node's own is drawn under the group's
         * line, one level further down. */
        if (!item.node) {
            grouped = item.level == level + 1 || (grouped && item.level > level + 1);
            continue;
        }
        if (item.level == level + 1) {
            grouped = false;
        } else if (!(grouped && item.level == level + 2)) {
            continue;
        }
        if (!markdown_core_walk_ranges(&walk, &item, &ranges, &count)) {
            break;
        }
        for (size_t i = 0; i < count && !walk.failed; i++) {
            walk.failed = !hold(r, after(ranges[i], previous));
        }
        if (walk.failed) {
            break;
        }
    }
    bool read = in && !walk.failed;
    markdown_core_walk_end(&walk);
    markdown_core_free(before);
    markdown_core_free(cell);
    if (read && markdown_core_node_get_kind(node) == MARKDOWN_CORE_KIND_TABLE_ROW) {
        read = hold_cells(root, r, level + 1);
    }
    if (read) {
        settle_held(r);
    }
    return read;
}

static int part_order(const void *left, const void *right) {
    const markdown_core_part *a = left, *b = right;
    return a->start != b->start ? (a->start < b->start ? -1 : 1) : 0;
}

/* The parts of `node`, read from its text, in source order. */
static bool read_parts(const markdown_core_node *node, const view *r, markdown_core_parts *parts) {
    *parts = (markdown_core_parts){
        .node = node,
        .text = r->text,
        .length =
            r->bases[r->run_count - 1] + (bufsize_t)(r->runs[r->run_count - 1].end - r->runs[r->run_count - 1].start),
        .held = r->held,
        .held_count = r->held_count,
    };
    void (*read)(markdown_core_parts *) = reader(markdown_core_node_get_kind(node));
    if (read) {
        read(parts);
    }
    if (parts->failed) {
        markdown_core_free(parts->items);
        return false;
    }
    if (parts->count > 1) {
        qsort(parts->items, parts->count, sizeof(*parts->items), part_order);
    }
    return true;
}

/* THE RANGES A QUERY ANSWERS, as source ranges: each text range split where
 * the node's runs split it. */
typedef struct {
    markdown_core_place *items;
    size_t count, capacity;
} ranges;

static bool add_place(ranges *out, const view *r, bufsize_t start, bufsize_t end) {
    for (size_t i = 0; i < r->run_count && start < end; i++) {
        const bufsize_t base = r->bases[i], top = base + (bufsize_t)(r->runs[i].end - r->runs[i].start);
        if (end <= base || start >= top) {
            continue;
        }
        const bufsize_t from = start > base ? start : base, to = end < top ? end : top;
        markdown_core_place *grown =
            markdown_core_reserve(out->items, &out->capacity, out->count + 1, sizeof(*out->items));
        if (!grown) {
            return false;
        }
        out->items = grown;
        out->items[out->count++] =
            (markdown_core_place){r->runs[i].start + (uint32_t)(from - base), r->runs[i].start + (uint32_t)(to - base)};
    }
    return true;
}

/* The source ranges of `node`'s fields of slot kind `slot`, or, when
 * `markers`, of its markers. */
static markdown_core_status node_places(const markdown_core_node *root, const markdown_core_node *node,
                                        markdown_core_slot_kind slot, bool markers, const uint8_t *source,
                                        size_t length, ranges *out) {
    view r;
    bool covered;
    *out = (ranges){0};
    if (!read_node(root, node, source, length, &r, &covered)) {
        view_free(&r);
        return covered ? MARKDOWN_CORE_ALLOCATION_FAILED : MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
    markdown_core_parts parts;
    if (!read_parts(node, &r, &parts)) {
        view_free(&r);
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    bool ok = true;
    if (!markers) {
        for (size_t i = 0; ok && i < parts.count; i++) {
            if (parts.items[i].slot == slot && !parts.items[i].lent) {
                ok = add_place(out, &r, parts.items[i].start, parts.items[i].end);
            }
        }
    } else {
        /* Each byte that neither a held node nor a part takes. */
        bool *taken = markdown_core_alloc((size_t)parts.length + 1, sizeof(*taken));
        ok = taken != NULL;
        for (size_t i = 0; ok && i < r.held_count; i++) {
            memset(taken + r.held[i].start, 1, r.held[i].end - r.held[i].start);
        }
        for (size_t i = 0; ok && i < parts.count; i++) {
            memset(taken + parts.items[i].start, 1, (size_t)(parts.items[i].end - parts.items[i].start));
        }
        for (bufsize_t at = 0; ok && at < parts.length;) {
            if (taken[at]) {
                at++;
                continue;
            }
            bufsize_t end = at;
            while (end < parts.length && !taken[end]) {
                end++;
            }
            ok = add_place(out, &r, at, end);
            at = end;
        }
        markdown_core_free(taken);
    }
    markdown_core_free(parts.items);
    view_free(&r);
    if (!ok) {
        markdown_core_free(out->items);
        *out = (ranges){0};
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    return MARKDOWN_CORE_OK;
}

static markdown_core_status answer(const markdown_core_document *document, const uint8_t *source, size_t length,
                                   markdown_core_status status, ranges *found, markdown_core_scope **scopes,
                                   size_t *count) {
    *scopes = NULL;
    *count = 0;
    if (status != MARKDOWN_CORE_OK || !found->count) {
        markdown_core_free(found->items);
        return status;
    }
    markdown_core_source_lines lines;
    if (!markdown_core_source_lines_read(&lines, source, length) ||
        !(*scopes = markdown_core_alloc(found->count, sizeof(**scopes)))) {
        markdown_core_free(found->items);
        markdown_core_free(lines.starts);
        return MARKDOWN_CORE_ALLOCATION_FAILED;
    }
    for (size_t i = 0; i < found->count; i++) {
        (*scopes)[i] = markdown_core_source_scope(&lines, source, found->items[i], document->unit);
    }
    *count = found->count;
    markdown_core_free(lines.starts);
    markdown_core_free(found->items);
    return MARKDOWN_CORE_OK;
}

markdown_core_status markdown_core_document_field(const markdown_core_document *document,
                                                  const markdown_core_node *node, markdown_core_slot_kind slot,
                                                  const uint8_t *source, size_t length, markdown_core_scope **scopes,
                                                  size_t *count) {
    ranges found;
    const markdown_core_status status = (unsigned)slot > MARKDOWN_CORE_SLOT_VALUE
                                            ? MARKDOWN_CORE_OUT_OF_BOUNDS
                                            : node_places(document->root, node, slot, false, source, length, &found);
    if (status != MARKDOWN_CORE_OK) {
        *scopes = NULL;
        *count = 0;
        return status;
    }
    return answer(document, source, length, status, &found, scopes, count);
}

markdown_core_status markdown_core_document_markers(const markdown_core_document *document,
                                                    const markdown_core_node *node, const uint8_t *source,
                                                    size_t length, markdown_core_scope **scopes, size_t *count) {
    ranges found;
    const markdown_core_status status =
        node_places(document->root, node, MARKDOWN_CORE_SLOT_NONE, true, source, length, &found);
    if (status != MARKDOWN_CORE_OK) {
        *scopes = NULL;
        *count = 0;
        return status;
    }
    return answer(document, source, length, status, &found, scopes, count);
}

markdown_core_status markdown_core_parts_fields(const markdown_core_node *root, const markdown_core_node *node,
                                                markdown_core_slot_kind slot, const uint8_t *source, size_t length,
                                                markdown_core_place **places, size_t *count) {
    ranges found;
    const markdown_core_status status = node_places(root, node, slot, false, source, length, &found);
    *places = status == MARKDOWN_CORE_OK ? found.items : NULL;
    *count = status == MARKDOWN_CORE_OK ? found.count : 0;
    return status;
}
