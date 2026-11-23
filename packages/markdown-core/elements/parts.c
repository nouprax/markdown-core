#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "../include/markdown_core.h"

#include "ast_internal.h"
#include "parts.h"
#include "element.h"
#include "markdown-core-elements.h"
#include "markdown_core_ctype.h"

/* FIELDS AND MARKERS (docs/plans/2026-10-23-lsp-completion-engine.md, 3.3).
 * A node's own source is its runs less the runs of the nodes it holds. The
 * parse recorded where each of its fields is as it read them (node.h,
 * markdown_core_fields); the rest of its own source is its markers. A
 * literal's field is recorded as an anchor and placed here, from the
 * literal the node holds. A query reads one node, as the scope query
 * does; nothing is read again. */

/* THE LITERAL'S PLACES IN A NODE'S TEXT: the text ranges of the bytes its
 * field of no slot kind holds (node.h, LINES and FOUND). */
typedef struct {
    const unsigned char *text;
    bufsize_t length;
    markdown_core_place *items;
    size_t count, capacity;
    bool failed;
} placed;

/* Adds the range from `start` to `end`, unless it is empty. */
static void place_add(placed *parts, bufsize_t start, bufsize_t end) {
    if (start >= end || start < 0 || end > parts->length) {
        return;
    }
    markdown_core_place *grown =
        markdown_core_reserve(parts->items, &parts->capacity, parts->count + 1, sizeof(*parts->items));
    if (!grown) {
        parts->failed = true;
        return;
    }
    parts->items = grown;
    parts->items[parts->count++] = (markdown_core_place){(uint32_t)start, (uint32_t)end};
}

/* Adds the places of `literal`, `size` bytes of a block's literal whose
 * lines end the text's lines from `from` on: the block took each line but
 * for what it stripped from its start, an indentation or a container's
 * prefix. Answers where the last line ends. */
static bufsize_t place_lines(placed *parts, bufsize_t from, const unsigned char *literal, bufsize_t size) {
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
            place_add(parts, end - width, past);
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

/* Adds the places of `literal`, `size` bytes of a node's literal, found
 * line by line in the text from `from` on: each of its lines is the first
 * place after the one before that holds its bytes. Answers where the last
 * line ends. */
static bufsize_t place_find(placed *parts, bufsize_t from, const unsigned char *literal, bufsize_t size) {
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
        place_add(parts, found, end);
        at = end;
        line = next;
    }
    return at;
}

/* A NODE AS ITS RECORD PLACES IT: its runs, the source they hold joined as
 * its text, `size` bytes, and where each run starts in that text, and the
 * source ranges of the nodes it holds. The text reads each NUL as the
 * parse did, as the three bytes of U+FFFD; `nuls` are where the runs hold
 * one, in the source. */
typedef struct {
    markdown_core_place *runs;
    size_t run_count;
    bufsize_t *bases;
    unsigned char *text;
    bufsize_t size;
    uint32_t *nuls;
    size_t nul_count;
    /* The held ranges, in the source, and where the node starts. */
    markdown_core_place *taken;
    size_t taken_count, taken_room;
    uint32_t start;
} view;

static void view_free(view *r) {
    markdown_core_free(r->runs);
    markdown_core_free(r->bases);
    markdown_core_free(r->text);
    markdown_core_free(r->nuls);
    markdown_core_free(r->taken);
}

/* How many of the NULs the runs hold lie before the source byte at `at`. */
static size_t nuls_before(const view *r, uint32_t at) {
    size_t low = 0, high = r->nul_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (r->nuls[middle] < at) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

/* Where the source byte at `at`, in run `i`, stands in the text. */
static bufsize_t text_at(const view *r, size_t i, uint32_t at) {
    const size_t nuls = r->nul_count ? nuls_before(r, at) - nuls_before(r, r->runs[i].start) : 0;
    return r->bases[i] + (bufsize_t)(at - r->runs[i].start) + 2 * (bufsize_t)nuls;
}

/* Where the text at `at`, in run `i`, stands in the source: a place inside
 * a NUL's U+FFFD stands at the NUL, or past it when `end`. */
static uint32_t source_at(const view *r, size_t i, bufsize_t at, bool end) {
    const markdown_core_place run = r->runs[i];
    if (!r->nul_count) {
        return run.start + (uint32_t)(at - r->bases[i]);
    }
    /* The NULs of the run before `at`: the text place of each is past the
     * one before it. */
    const size_t first = nuls_before(r, run.start);
    size_t low = first, high = nuls_before(r, run.end);
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (r->bases[i] + (bufsize_t)(r->nuls[middle] - run.start) + 2 * (bufsize_t)(middle - first) < at) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low > first) {
        const uint32_t nul = r->nuls[low - 1];
        const bufsize_t place = r->bases[i] + (bufsize_t)(nul - run.start) + 2 * (bufsize_t)(low - 1 - first);
        if (at < place + 3) {
            return end ? nul + 1 : nul;
        }
    }
    return run.start + (uint32_t)(at - r->bases[i]) - 2 * (uint32_t)(low - first);
}

/* The first of `r`'s runs that ends past the source byte at `at`, or the
 * run count: the runs are in source order. */
static size_t run_from(const view *r, uint32_t at) {
    size_t low = 0, high = r->run_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (r->runs[middle].end <= at) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

static int place_order(const void *left, const void *right) {
    const markdown_core_place *a = left, *b = right;
    return a->start != b->start ? (a->start < b->start ? -1 : 1) : 0;
}

/* Puts `*count` ranges in order, those that meet or overlap merged. */
static void settle(markdown_core_place *places, size_t *count) {
    if (!*count) {
        return;
    }
    qsort(places, *count, sizeof(*places), place_order);
    size_t kept = 1;
    for (size_t i = 1; i < *count; i++) {
        markdown_core_place *last = &places[kept - 1];
        if (places[i].start <= last->end) {
            last->end = places[i].end > last->end ? places[i].end : last->end;
        } else {
            places[kept++] = places[i];
        }
    }
    *count = kept;
}

/* Adds a held node's run, as the text ranges of the node's runs it meets: a
 * held node can reach past its holder (an inline note's footnote takes the
 * `^[` and `]` around its citation). */
static bool hold(view *r, markdown_core_place run) {
    for (size_t i = run_from(r, run.start); i < r->run_count && r->runs[i].start < run.end; i++) {
        const uint32_t start = run.start > r->runs[i].start ? run.start : r->runs[i].start;
        const uint32_t end = run.end < r->runs[i].end ? run.end : r->runs[i].end;
        if (start < end) {
            markdown_core_place *grown =
                markdown_core_reserve(r->taken, &r->taken_room, r->taken_count + 1, sizeof(*r->taken));
            if (!grown) {
                return false;
            }
            r->taken = grown;
            r->taken[r->taken_count++] = (markdown_core_place){start, end};
        }
    }
    return true;
}

/* Joins the source of `r`'s runs as its text, each NUL as U+FFFD: false
 * when the source ends before the runs do, `*covered` false, or the text
 * could not be allocated. */
static bool join(view *r, const uint8_t *source, size_t length, bool *covered) {
    if (r->runs[r->run_count - 1].end > length) {
        *covered = false;
        return false;
    }
    size_t size = 0, room = 0;
    for (size_t i = 0; i < r->run_count; i++) {
        r->bases[i] = (bufsize_t)size;
        for (uint32_t at = r->runs[i].start; at < r->runs[i].end; at++) {
            if (source[at]) {
                continue;
            }
            uint32_t *grown = markdown_core_reserve(r->nuls, &room, r->nul_count + 1, sizeof(*r->nuls));
            if (!grown) {
                return false;
            }
            r->nuls = grown;
            r->nuls[r->nul_count++] = at;
            size += 2;
        }
        size += r->runs[i].end - r->runs[i].start;
    }
    if (!(r->text = markdown_core_alloc(size + 1, 1))) {
        return false;
    }
    for (size_t i = 0; i < r->run_count; i++) {
        unsigned char *out = r->text + r->bases[i];
        for (uint32_t at = r->runs[i].start; at < r->runs[i].end; at++) {
            if (source[at]) {
                *out++ = source[at];
            } else {
                memcpy(out, "\xef\xbf\xbd", 3);
                out += 3;
            }
        }
    }
    r->text[size] = '\n';
    r->size = (bufsize_t)size;
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
static bool read_node(const markdown_core_node *root, const markdown_core_window *window,
                      const markdown_core_node *node, const uint8_t *source, size_t length, view *r, bool *covered) {
    markdown_core_walk walk;
    markdown_core_walk_item item;
    markdown_core_place *before = NULL, *cell = NULL;
    size_t level = 0, capacity = 0, cell_count = 0, cell_capacity = 0, cell_level = 0;
    bool in = false, grouped = false;
    *r = (view){0};
    *covered = true;
    markdown_core_walk_within(&walk, root, window);
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
            r->start = item.place.start;
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
        settle(r->taken, &r->taken_count);
    }
    return read;
}

/* THE RANGES A QUERY ANSWERS, as source ranges: each text range split where
 * the node's runs split it. */
typedef struct {
    markdown_core_place *items;
    size_t count, capacity;
} ranges;

static bool add_place(ranges *out, const view *r, bufsize_t start, bufsize_t end) {
    /* The first run whose text ends past `start`: each run's text ends where
     * the next one's starts. */
    size_t low = 0, high = r->run_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if ((middle + 1 < r->run_count ? r->bases[middle + 1] : r->size) <= start) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    for (size_t i = low; i < r->run_count && r->bases[i] < end && start < end; i++) {
        const bufsize_t base = r->bases[i], top = i + 1 < r->run_count ? r->bases[i + 1] : r->size;
        if (start >= top) {
            continue;
        }
        const bufsize_t from = start > base ? start : base, to = end < top ? end : top;
        markdown_core_place *grown =
            markdown_core_reserve(out->items, &out->capacity, out->count + 1, sizeof(*out->items));
        if (!grown) {
            return false;
        }
        out->items = grown;
        out->items[out->count++] = (markdown_core_place){source_at(r, i, from, false), source_at(r, i, to, true)};
    }
    return true;
}

static bool add_source(ranges *out, uint32_t start, uint32_t end) {
    markdown_core_place *grown = markdown_core_reserve(out->items, &out->capacity, out->count + 1, sizeof(*out->items));
    if (!grown) {
        return false;
    }
    out->items = grown;
    out->items[out->count++] = (markdown_core_place){start, end};
    return true;
}

/* Adds the bytes of the node's runs from `start` to `end`. */
static bool add_runs(ranges *out, const view *r, uint32_t start, uint32_t end) {
    for (size_t i = run_from(r, start); i < r->run_count && r->runs[i].start < end; i++) {
        const uint32_t from = start > r->runs[i].start ? start : r->runs[i].start;
        const uint32_t to = end < r->runs[i].end ? end : r->runs[i].end;
        if (from < to && !add_source(out, from, to)) {
            return false;
        }
    }
    return true;
}

/* THE LITERAL A NODE'S FIELD OF NO SLOT KIND IS (node.h, LINES and FOUND),
 * placed in its text from the source byte `at`, as text ranges into
 * `parts`. */
static bool place_literal(const markdown_core_node *node, const view *r, uint32_t at, bool lines, placed *parts) {
    const markdown_core_facet *facet = markdown_core_core_facet(markdown_core_node_get_kind(node));
    if (!facet || !facet->literal) {
        return true;
    }
    bufsize_t start;
    const markdown_core_chunk *literal = facet->literal(node, &start);
    const unsigned char *front, *back;
    bufsize_t front_size, back_size;
    markdown_core_literal_halves(node, literal, start, &front, &front_size, &back, &back_size);
    const size_t i = run_from(r, at);
    bufsize_t from = i < r->run_count && r->runs[i].start <= at ? text_at(r, i, at)
                     : i < r->run_count                         ? r->bases[i]
                                                                : r->size;
    if (lines) {
        /* A literal that starts at a line's end starts on the next line. */
        if (from < r->size && markdown_core_is_line_end(r->text[from])) {
            from += r->text[from] == '\r' && from + 1 < r->size && r->text[from + 1] == '\n' ? 2 : 1;
        }
        from = place_lines(parts, from, front, front_size);
        place_lines(parts, from, back, back_size);
    } else {
        from = place_find(parts, from, front, front_size);
        place_find(parts, from, back, back_size);
    }
    return !parts->failed;
}

/* THE PLACES THE PARSE RECORDED (markdown_core_fields): `node`'s fields of
 * slot kind `slot`, or, when `markers`, its markers, the bytes of its runs
 * neither a held node nor a slot takes. */
static bool recorded_places(const markdown_core_node *node, const view *r, markdown_core_slot_kind slot, bool markers,
                            ranges *out) {
    const markdown_core_fields *slots = node->fields;
    const uint32_t count = slots ? slots->count : 0;
    const markdown_core_facet *facet = markdown_core_core_facet(markdown_core_node_get_kind(node));
    if (facet && facet->whole) {
        return markers || slot != MARKDOWN_CORE_SLOT_NONE || !r->run_count ||
               add_runs(out, r, r->runs[0].start, r->runs[r->run_count - 1].end);
    }
    /* The literal's ranges, in the source. */
    placed spots = {.text = r->text, .length = r->size};
    ranges literal = {0};
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t how = slots->items[i].how;
        if ((how == MARKDOWN_CORE_FIELD_LINES || how == MARKDOWN_CORE_FIELD_FOUND) &&
            !place_literal(node, r, r->start + slots->items[i].start, how == MARKDOWN_CORE_FIELD_LINES, &spots)) {
            markdown_core_free(spots.items);
            return false;
        }
    }
    bool placing = true;
    for (size_t i = 0; placing && i < spots.count; i++) {
        placing = add_place(&literal, r, (bufsize_t)spots.items[i].start, (bufsize_t)spots.items[i].end);
    }
    markdown_core_free(spots.items);
    if (!placing) {
        markdown_core_free(literal.items);
        return false;
    }
    if (!markers) {
        const size_t room = count + (slot == MARKDOWN_CORE_SLOT_NONE ? literal.count : 0);
        markdown_core_place *own = room ? markdown_core_alloc(room, sizeof(*own)) : NULL;
        size_t found = 0;
        if (room && !own) {
            markdown_core_free(literal.items);
            return false;
        }
        for (uint32_t i = 0; i < count; i++) {
            if (slots->items[i].slot == slot &&
                (slots->items[i].how == MARKDOWN_CORE_FIELD || slots->items[i].how == MARKDOWN_CORE_FIELD_BARE) &&
                slots->items[i].end > slots->items[i].start) {
                own[found++] = (markdown_core_place){r->start + slots->items[i].start, r->start + slots->items[i].end};
            }
        }
        for (size_t i = 0; slot == MARKDOWN_CORE_SLOT_NONE && i < literal.count; i++) {
            own[found++] = literal.items[i];
        }
        markdown_core_free(literal.items);
        if (found > 1) {
            qsort(own, found, sizeof(*own), place_order);
        }
        bool ok = true;
        for (size_t i = 0; ok && i < found; i++) {
            ok = add_runs(out, r, own[i].start, own[i].end);
        }
        markdown_core_free(own);
        return ok;
    }
    size_t taken = r->taken_count + count + literal.count;
    markdown_core_place *all = taken ? markdown_core_alloc(taken, sizeof(*all)) : NULL;
    if (taken && !all) {
        markdown_core_free(literal.items);
        return false;
    }
    if (r->taken_count) {
        memcpy(all, r->taken, r->taken_count * sizeof(*all));
    }
    size_t kept = r->taken_count;
    for (uint32_t i = 0; i < count; i++) {
        if (slots->items[i].end > slots->items[i].start && slots->items[i].how < MARKDOWN_CORE_FIELD_OPENER) {
            all[kept++] = (markdown_core_place){r->start + slots->items[i].start, r->start + slots->items[i].end};
        }
    }
    if (literal.count) {
        memcpy(all + kept, literal.items, literal.count * sizeof(*all));
    }
    taken = kept + literal.count;
    markdown_core_free(literal.items);
    settle(all, &taken);
    bool ok = true;
    size_t next = 0;
    for (size_t i = 0; ok && i < r->run_count; i++) {
        uint32_t at = r->runs[i].start;
        const uint32_t end = r->runs[i].end;
        while (next < taken && all[next].end <= at) {
            next++;
        }
        for (size_t k = next; ok && at < end; k++) {
            const uint32_t stop = k < taken && all[k].start < end ? all[k].start : end;
            if (at < stop) {
                ok = add_source(out, at, stop);
            }
            if (k >= taken || all[k].start >= end) {
                break;
            }
            at = all[k].end > at ? all[k].end : at;
        }
    }
    markdown_core_free(all);
    return ok;
}

/* The source ranges of `node`'s fields of slot kind `slot`, or, when
 * `markers`, of its markers. */
static markdown_core_status node_places(const markdown_core_node *root, const markdown_core_window *window,
                                        const markdown_core_node *node, markdown_core_slot_kind slot, bool markers,
                                        const uint8_t *source, size_t length, ranges *out) {
    view r;
    bool covered;
    *out = (ranges){0};
    if (!read_node(root, window, node, source, length, &r, &covered)) {
        view_free(&r);
        return covered ? MARKDOWN_CORE_ALLOCATION_FAILED : MARKDOWN_CORE_OUT_OF_BOUNDS;
    }
#ifndef NDEBUG
    for (uint32_t i = 0; node->fields && i < node->fields->count; i++) {
        assert(!node->fields->items[i].raw);
    }
#endif
    const bool ok = recorded_places(node, &r, slot, markers, out);
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
    const markdown_core_status status =
        (unsigned)slot > MARKDOWN_CORE_SLOT_VALUE
            ? MARKDOWN_CORE_OUT_OF_BOUNDS
            : node_places(document->root, NULL, node, slot, false, source, length, &found);
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
        node_places(document->root, NULL, node, MARKDOWN_CORE_SLOT_NONE, true, source, length, &found);
    if (status != MARKDOWN_CORE_OK) {
        *scopes = NULL;
        *count = 0;
        return status;
    }
    return answer(document, source, length, status, &found, scopes, count);
}

markdown_core_status markdown_core_parts_fields(const markdown_core_node *root, const markdown_core_window *window,
                                                const markdown_core_node *node, markdown_core_slot_kind slot,
                                                const uint8_t *source, size_t length, markdown_core_place **places,
                                                size_t *count) {
    ranges found;
    const markdown_core_status status = node_places(root, window, node, slot, false, source, length, &found);
    *places = status == MARKDOWN_CORE_OK ? found.items : NULL;
    *count = status == MARKDOWN_CORE_OK ? found.count : 0;
    return status;
}
