/* The incremental gates' oracles (incremental_oracles.h). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <markdown_core.h>

#include "incremental_oracles.h"
#include "test_support.h"

void fail(run *state, const char *oracle, const char *format, ...) {
    char message[512];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (!state->failures) {
        state->first_oracle = oracle;
        memcpy(state->first, message, sizeof(message));
    }
    if (state->required && strstr(state->required, oracle) && !strstr(state->caught, oracle) &&
        strlen(state->caught) + strlen(oracle) + 2 < sizeof(state->caught)) {
        strcat(state->caught, state->caught[0] ? " " : "");
        strcat(state->caught, oracle);
    }
    if (!state->stop_at_failure) {
        fprintf(stderr, "FAILED [%s] %s\n", oracle, message);
    }
    state->failures++;
}

/* Whether every oracle the run must see fail has failed. */
bool failed_all(const run *state) {
    const char *cursor = state->required;
    char oracle[8];
    int used;
    while (cursor && sscanf(cursor, "%7s%n", oracle, &used) == 1) {
        if (!strstr(state->caught, oracle)) {
            return false;
        }
        cursor += used;
    }
    return true;
}

bool stopped(const run *state) { return state->stop_at_failure && state->failures && failed_all(state); }

markdown_core_text_unit unit_of(ih_unit unit) {
    return unit == IH_UTF16 ? MARKDOWN_CORE_TEXT_UNIT_UTF16 : MARKDOWN_CORE_TEXT_UNIT_UTF8;
}

/* The canonical dump of a document parsed from `source`; the caller frees it. */
static uint8_t *dump_of(const markdown_core_document *document, const uint8_t *source, size_t source_length,
                        size_t *length) {
    uint8_t *output = NULL;
    if (markdown_core_document_dump(document, markdown_core_document_root(document), source, source_length, &output,
                                    length) != MARKDOWN_CORE_OK) {
        return NULL;
    }
    return output;
}

static bool same(const uint8_t *left, size_t left_length, const uint8_t *right, size_t right_length) {
    return left && right && left_length == right_length && memcmp(left, right, left_length) == 0;
}

/* ------------------------------------------------------------------ views */

/* A document as the oracles read it: its nodes in canonical walk order, each
 * with its owner's walk index and relation, its absolute range and, when the
 * view is taken with the document's dump, its value. The view holds no
 * pointer it follows once taken, so it outlives the document: it is the
 * snapshot of 4.3. */
typedef struct view_node {
    uint64_t id;
    markdown_core_node_kind kind;
    const markdown_core_node *object;
    markdown_core_extent extent;
    /* Its source window, and how many runs it has. */
    ts_ast_range range;
    size_t run_count;
    size_t owner; /* SIZE_MAX for the root */
    size_t relation;
    /* The node's items, in order: every node its relations hold. */
    size_t items, item_count;
    /* Its kind, scalars and extent: its dump line without the tree drawing
     * or the scope, and its extent. */
    size_t value, value_length;
} view_node;

typedef struct {
    const markdown_core_node *object;
    size_t index;
} view_object;

typedef struct view {
    view_node *nodes;
    size_t count, capacity;
    size_t *items;
    char *values;
    size_t values_size;
    /* The nodes by object, for answers given as objects. */
    view_object *objects;
    /* The owner of each node while the view is taken. */
    const markdown_core_node **owners;
    size_t owners_capacity;
} view;

static void view_free(view *taken) {
    free(taken->nodes);
    free(taken->items);
    free(taken->values);
    free(taken->objects);
    free(taken->owners);
    memset(taken, 0, sizeof(*taken));
}

static int view_visit(const markdown_core_node *node, ts_ast_place place, void *context) {
    view *taken = (view *)context;
    if (taken->count == taken->capacity) {
        size_t capacity = taken->capacity ? taken->capacity * 2 : 64;
        view_node *nodes = (view_node *)realloc(taken->nodes, capacity * sizeof(*nodes));
        const markdown_core_node **owners;
        if (!nodes) {
            return 1;
        }
        taken->nodes = nodes;
        owners = (const markdown_core_node **)realloc((void *)taken->owners, capacity * sizeof(*owners));
        if (!owners) {
            return 1;
        }
        taken->owners = owners;
        taken->capacity = capacity;
    }
    size_t run_count = 0;
    (void)markdown_core_node_runs(node, &run_count);
    taken->owners[taken->count] = place.owner;
    taken->nodes[taken->count] = (view_node){markdown_core_node_id(node),
                                             markdown_core_node_get_kind(node),
                                             node,
                                             markdown_core_node_extent(node),
                                             place.range,
                                             run_count,
                                             SIZE_MAX,
                                             place.relation,
                                             0,
                                             0,
                                             0,
                                             0};
    taken->count++;
    return 0;
}

static int object_order(const void *left, const void *right) {
    const view_object *a = (const view_object *)left, *b = (const view_object *)right;
    return a->object < b->object ? -1 : a->object > b->object;
}

/* The walk index of `object`, or SIZE_MAX when it is no node of the view. */
static size_t view_find(const view *taken, const markdown_core_node *object) {
    view_object key = {object, 0};
    const view_object *found;
    if (!object) {
        return SIZE_MAX;
    }
    found = (const view_object *)bsearch(&key, taken->objects, taken->count, sizeof(*taken->objects), object_order);
    return found ? found->index : SIZE_MAX;
}

/* A dump line's node part: the line without its tree drawing, which is
 * everything before its first ASCII capital. */
static const char *line_node(const char *line, const char *end) {
    while (line < end && !(*line >= 'A' && *line <= 'Z')) {
        line++;
    }
    return line;
}

/* The values of the view's nodes from its document's dump: the dump's node
 * lines are its nodes in walk order, and a relation's group line has no
 * scope. */
static bool view_values(view *taken, const uint8_t *dump, size_t dump_length) {
    const char *cursor = (const char *)dump, *end = cursor + dump_length;
    size_t index = 0, capacity = dump_length + taken->count * 64 + 1;
    /* Room for each node's runs. */
    for (index = 0; index < taken->count; index++) {
        capacity += taken->nodes[index].run_count * 24;
    }
    index = 0;
    taken->values = (char *)malloc(capacity);
    if (!taken->values) {
        return false;
    }
    while (cursor < end) {
        const char *line_end = (const char *)memchr(cursor, '\n', (size_t)(end - cursor));
        const char *node = line_node(cursor, line_end ? line_end : end);
        const char *stop = line_end ? line_end : end;
        const char *kind_end = node < stop ? (const char *)memchr(node, ' ', (size_t)(stop - node)) : NULL;
        cursor = line_end ? line_end + 1 : end;
        if (!kind_end || stop - kind_end < 7 || memcmp(kind_end, " scope=", 7) != 0) {
            continue;
        }
        if (index == taken->count) {
            return false;
        }
        {
            view_node *entry = &taken->nodes[index++];
            const char *name;
            const char *after = (const char *)memchr(kind_end + 1, ' ', (size_t)(stop - kind_end - 1));
            size_t at = taken->values_size;
            if (markdown_core_node_kind_name(entry->kind, &name) != MARKDOWN_CORE_OK ||
                strlen(name) != (size_t)(kind_end - node) || memcmp(name, node, (size_t)(kind_end - node)) != 0) {
                return false;
            }
            memcpy(taken->values + at, node, (size_t)(kind_end - node));
            at += (size_t)(kind_end - node);
            if (after) {
                memcpy(taken->values + at, after, (size_t)(stop - after));
                at += (size_t)(stop - after);
            }
            at += (size_t)snprintf(taken->values + at, capacity - at, " extent=%d,%u", (int)entry->extent.lead,
                                   (unsigned)entry->extent.span);
            {
                size_t count, item;
                const markdown_core_run *run = markdown_core_node_runs(entry->object, &count);
                at += (size_t)snprintf(taken->values + at, capacity - at, " runs=");
                for (item = 0; item < count; item++) {
                    at += (size_t)snprintf(taken->values + at, capacity - at, "%d,%u;", (int)run[item].lead,
                                           (unsigned)run[item].span);
                }
            }
            entry->value = taken->values_size;
            entry->value_length = at - taken->values_size;
            taken->values_size = at;
        }
    }
    return index == taken->count;
}

/* Takes a view of `document`; with `dump`, its values too. */
static bool view_take(view *taken, const markdown_core_document *document, const uint8_t *dump, size_t dump_length) {
    size_t index, *next;
    memset(taken, 0, sizeof(*taken));
    if (ts_ast_walk_owned(markdown_core_document_root(document), view_visit, taken) != 0 || !taken->count) {
        return false;
    }
    taken->objects = (view_object *)malloc(taken->count * sizeof(*taken->objects));
    taken->items = (size_t *)malloc(taken->count * sizeof(*taken->items));
    next = (size_t *)calloc(taken->count, sizeof(*next));
    if (!taken->objects || !taken->items || !next) {
        free(next);
        return false;
    }
    for (index = 0; index < taken->count; index++) {
        taken->objects[index] = (view_object){taken->nodes[index].object, index};
    }
    qsort(taken->objects, taken->count, sizeof(*taken->objects), object_order);
    /* Owners by walk index, then each owner's items in walk order, which is
     * relation order and then source order within a relation. */
    for (index = 1; index < taken->count; index++) {
        size_t owner = view_find(taken, taken->owners[index]);
        if (owner == SIZE_MAX) {
            free(next);
            return false;
        }
        taken->nodes[index].owner = owner;
        taken->nodes[owner].item_count++;
    }
    for (index = 0, next[0] = 0; index < taken->count; index++) {
        taken->nodes[index].items = index ? taken->nodes[index - 1].items + taken->nodes[index - 1].item_count : 0;
        next[index] = taken->nodes[index].items;
    }
    for (index = 1; index < taken->count; index++) {
        taken->items[next[taken->nodes[index].owner]++] = index;
    }
    free(next);
    return !dump || view_values(taken, dump, dump_length);
}

/* ------------------------------------------------------------- 4.1 queries */

/* The byte offsets where the text's lines start. */
static size_t *line_starts(const uint8_t *text, size_t length, size_t *count) {
    size_t *starts = (size_t *)malloc((length + 2) * sizeof(*starts)), at;
    *count = 0;
    if (!starts) {
        return NULL;
    }
    starts[(*count)++] = 0;
    for (at = 0; at < length; at++) {
        if (text[at] == '\n' || (text[at] == '\r' && (at + 1 == length || text[at + 1] != '\n'))) {
            starts[(*count)++] = at + 1;
        }
    }
    return starts;
}

/* One scope `L:C..L:C` of a dump in UTF-8 columns, in `unit`; `*next` is
 * past it. */
static bool dump_scope(const char *token, const uint8_t *text, const size_t *starts, size_t lines, ih_unit unit,
                       markdown_core_scope *scope, const char **next) {
    int values[4];
    int index, used = 0;
    if (sscanf(token, "%d:%d..%d:%d%n", &values[0], &values[1], &values[2], &values[3], &used) != 4) {
        return false;
    }
    *next = token + used;
    for (index = 0; index < 4; index += 2) {
        if (values[index] < 1 || (size_t)values[index] > lines) {
            return false;
        }
        if (unit == IH_UTF16) {
            size_t start = starts[values[index] - 1];
            /* A start column names its byte; an end column counts to it. */
            size_t bytes = (size_t)values[index + 1] - (index == 0 ? 1 : 0);
            values[index + 1] = (int)(ih_utf16_units(text + start, bytes) + (index == 0 ? 1 : 0));
        }
    }
    *scope = (markdown_core_scope){{values[0], values[1]}, {values[2], values[3]}};
    return true;
}

static bool same_scope(markdown_core_scope a, markdown_core_scope b) {
    return a.start.line == b.start.line && a.start.column == b.start.column && a.end.line == b.end.line &&
           a.end.column == b.end.column;
}

static bool same_scopes(const markdown_core_scope *a, size_t a_count, const markdown_core_scope *b, size_t b_count) {
    size_t index;
    if (a_count != b_count) {
        return false;
    }
    for (index = 0; index < a_count; index++) {
        if (!same_scope(a[index], b[index])) {
            return false;
        }
    }
    return true;
}

/* Whether a dump's `scope=` token, its scopes joined by commas, is
 * `scopes`. */
static bool dump_scopes(const char *token, const uint8_t *text, const size_t *starts, size_t lines, ih_unit unit,
                        const markdown_core_scope *scopes, size_t count) {
    const char *at = token + strlen("scope=");
    size_t index;
    for (index = 0; index < count; index++) {
        markdown_core_scope printed;
        if ((index && *at++ != ',') || !dump_scope(at, text, starts, lines, unit, &printed, &at) ||
            !same_scope(scopes[index], printed)) {
            return false;
        }
    }
    return *at == ' ';
}

/* WHICH NODES CORRESPOND. An oracle compares the subject's document with an
 * expected one node for node: by walk index when the two must be equal, and
 * through a correspondence when parts of them may differ (4.6): the expected
 * node's subject node, SIZE_MAX for none, and which nodes of each lie where
 * the two may differ. */
typedef struct correspondence {
    size_t *to_actual;
    bool *actual_free, *expected_free;
} correspondence;

static size_t corresponding(const correspondence *pairs, size_t expected) {
    return !pairs || expected == SIZE_MAX ? expected : pairs->to_actual[expected];
}

static bool free_node(const bool *flags, size_t index) { return flags && index != SIZE_MAX && flags[index]; }

static void correspondence_free(correspondence *pairs) {
    free(pairs->to_actual);
    free(pairs->actual_free);
    free(pairs->expected_free);
    memset(pairs, 0, sizeof(*pairs));
}

#define SCOPE_SAMPLE 4

/* The queries of 4.1 on a sample of nodes: scope(of:in:) and node(at:in:) at
 * each end of a node's scope and beside it answer as the expected document's
 * corresponding node does, and the expected document's own scopes are its
 * dump's. Each step takes SCOPE_SAMPLE nodes spread over the walk, and the
 * next step the nodes after them, so consecutive steps cover the whole
 * document: every query costs a walk of the document. */
static void check_queries(run *state, const char *oracle, const char *where, size_t step, ih_unit unit,
                          const markdown_core_document *document, const view *actual,
                          const markdown_core_document *expected_document, const view *expected,
                          const uint8_t *expected_dump, size_t expected_dump_size, const uint8_t *text, size_t length) {
    size_t lines = 0, stride = expected->count / SCOPE_SAMPLE + 1, index, line = 0;
    size_t *starts = line_starts(text, length, &lines);
    const char *cursor = (const char *)expected_dump, *end = cursor + expected_dump_size;
    if (!starts) {
        fail(state, "harness", "%s step %zu: out of memory", where, step);
        return;
    }
    for (index = 0; index < expected->count && !stopped(state); index++) {
        markdown_core_scope *mine = NULL, *theirs = NULL;
        size_t mine_count = 0, theirs_count = 0, range;
        const char *scope = NULL;
        /* The expected dump's line of node `index`. */
        while (cursor < end) {
            const char *line_end = (const char *)memchr(cursor, '\n', (size_t)(end - cursor));
            const char *stop = line_end ? line_end : end;
            const char *node = line_node(cursor, stop);
            const char *space = node < stop ? (const char *)memchr(node, ' ', (size_t)(stop - node)) : NULL;
            cursor = line_end ? line_end + 1 : end;
            if (space && stop - space >= 7 && memcmp(space, " scope=", 7) == 0) {
                scope = space + 1;
                line++;
                break;
            }
        }
        if (index % stride != step % stride) {
            continue;
        }
        if (markdown_core_document_scope(document, actual->nodes[index].object, text, length, &mine, &mine_count) !=
                MARKDOWN_CORE_OK ||
            markdown_core_document_scope(expected_document, expected->nodes[index].object, text, length, &theirs,
                                         &theirs_count) != MARKDOWN_CORE_OK) {
            fail(state, oracle, "%s step %zu: a scope query failed for node %zu", where, step, index + 1);
        } else if (!same_scopes(mine, mine_count, theirs, theirs_count)) {
            fail(state, oracle, "%s step %zu: node %zu's scopes differ from the expected document's", where, step,
                 index + 1);
        } else if (!scope || !dump_scopes(scope, text, starts, lines, unit, theirs, theirs_count)) {
            fail(state, oracle, "%s step %zu: node %zu's expected scopes differ from its dump", where, step, index + 1);
        }
        for (range = 0; range < mine_count && same_scopes(mine, mine_count, theirs, theirs_count); range++) {
            markdown_core_position positions[4] = {mine[range].start, mine[range].end, mine[range].start,
                                                   mine[range].end};
            size_t at;
            positions[2].column--;
            positions[3].column++;
            for (at = 0; at < 4; at++) {
                const markdown_core_node *hit = NULL, *expected_hit = NULL;
                size_t hit_index, expected_index;
                if (positions[at].column < 1) {
                    continue;
                }
                if (markdown_core_document_node_at(document, positions[at], text, length, &hit) != MARKDOWN_CORE_OK ||
                    markdown_core_document_node_at(expected_document, positions[at], text, length, &expected_hit) !=
                        MARKDOWN_CORE_OK) {
                    fail(state, oracle, "%s step %zu: node(at: %d:%d) failed", where, step, positions[at].line,
                         positions[at].column);
                    break;
                }
                hit_index = view_find(actual, hit);
                expected_index = view_find(expected, expected_hit);
                if (hit_index != expected_index) {
                    fail(state, oracle, "%s step %zu: node(at: %d:%d) differs from the expected document's", where,
                         step, positions[at].line, positions[at].column);
                    break;
                }
            }
        }
        markdown_core_scopes_free(mine);
        markdown_core_scopes_free(theirs);
    }
    free(starts);
}

/* The definition tables 4.1 compares: footnotes, specimens and references. */
enum { TABLE_FOOTNOTE, TABLE_SPECIMEN, TABLE_REFERENCE, TABLE_COUNT };

static const char *const table_names[TABLE_COUNT] = {"footnote", "specimen", "reference"};

static size_t table_count(const markdown_core_document *document, int table) {
    return table == TABLE_FOOTNOTE   ? markdown_core_document_footnote_count(document)
           : table == TABLE_SPECIMEN ? markdown_core_document_specimen_count(document)
                                     : markdown_core_document_reference_count(document);
}

static const markdown_core_node *table_entry(const markdown_core_document *document, int table, size_t index) {
    const markdown_core_node *entry = NULL;
    TS_OK(table == TABLE_FOOTNOTE   ? markdown_core_document_footnote_at(document, index, &entry)
          : table == TABLE_SPECIMEN ? markdown_core_document_specimen_at(document, index, &entry)
                                    : markdown_core_document_reference_at(document, index, &entry));
    return entry;
}

/* The answer of `table`'s lookup of the label of `entry`, an entry of the
 * expected document's table, in `document`; `*labeled` is whether the entry
 * has a label. */
static const markdown_core_node *table_lookup(const markdown_core_document *document, int table,
                                              const markdown_core_node *entry, bool *labeled) {
    markdown_core_optional_string label = {0};
    markdown_core_optional_i64 start;
    if (table == TABLE_REFERENCE) {
        TS_OK(markdown_core_reference_label(entry, &label.value));
        label.has_value = true;
    } else {
        TS_OK(table == TABLE_SPECIMEN ? markdown_core_specimen_properties(entry, &label, &start)
                                      : markdown_core_footnote_label(entry, &label));
    }
    *labeled = label.has_value;
    if (!label.has_value) {
        return NULL;
    }
    return table == TABLE_FOOTNOTE   ? markdown_core_document_footnote_for(document, label.value)
           : table == TABLE_SPECIMEN ? markdown_core_document_specimen_for(document, label.value)
                                     : markdown_core_document_reference_for(document, label.value);
}

/* The next entry of `table` at or after `*index` that lies where the two
 * documents agree, as a walk index of `taken`, or SIZE_MAX past the last. */
static size_t next_entry(const markdown_core_document *document, int table, const view *taken, const bool *flags,
                         size_t *index) {
    size_t count = table_count(document, table);
    for (; *index < count; ++*index) {
        size_t at = view_find(taken, table_entry(document, table, *index));
        if (!free_node(flags, at)) {
            ++*index;
            return at;
        }
    }
    return SIZE_MAX;
}

/* 4.1's definition tables: the lists, the lookup of every label they hold,
 * and the label table, each entry the expected document's corresponding
 * node. Entries where the two may differ, inline notes in an inline root a
 * snapshot reads otherwise, are left out of the lists. */
static void check_tables(run *state, const char *oracle, const char *where, size_t step,
                         const markdown_core_document *document, const view *actual,
                         const markdown_core_document *expected_document, const view *expected,
                         const correspondence *pairs) {
    int table;
    size_t index;
    for (table = 0; table < TABLE_COUNT; table++) {
        size_t mine_at = 0, theirs_at = 0, entry = 0;
        for (;; entry++) {
            size_t mine = next_entry(document, table, actual, pairs ? pairs->actual_free : NULL, &mine_at);
            size_t theirs =
                next_entry(expected_document, table, expected, pairs ? pairs->expected_free : NULL, &theirs_at);
            bool labeled;
            const markdown_core_node *answer, *expected_answer;
            if (mine != corresponding(pairs, theirs)) {
                fail(state, oracle, "%s step %zu: entry %zu of the %s table differs from the expected document's",
                     where, step, entry, table_names[table]);
                break;
            }
            if (theirs == SIZE_MAX) {
                break;
            }
            expected_answer = table_lookup(expected_document, table, expected->nodes[theirs].object, &labeled);
            answer = table_lookup(document, table, expected->nodes[theirs].object, &labeled);
            if (labeled && view_find(actual, answer) != corresponding(pairs, view_find(expected, expected_answer))) {
                fail(state, oracle, "%s step %zu: the %s lookup of entry %zu's label differs", where, step,
                     table_names[table], entry);
                break;
            }
        }
    }
    if (markdown_core_document_reference_label_count(document) !=
        markdown_core_document_reference_label_count(expected_document)) {
        fail(state, oracle, "%s step %zu: the reference label table differs from the expected document's", where, step);
        return;
    }
    for (index = 0; index < markdown_core_document_reference_label_count(document); index++) {
        markdown_core_string label, expected_label;
        const markdown_core_node *target = NULL, *expected_target = NULL;
        TS_OK(markdown_core_document_reference_label_at(document, index, &label, &target));
        TS_OK(markdown_core_document_reference_label_at(expected_document, index, &expected_label, &expected_target));
        if (label.length != expected_label.length || memcmp(label.data, expected_label.data, label.length) != 0 ||
            view_find(actual, target) != corresponding(pairs, view_find(expected, expected_target))) {
            fail(state, oracle, "%s step %zu: entry %zu of the reference label table differs", where, step, index);
            break;
        }
    }
}

/* ------------------------------------------------- identity (4.2 to 4.5) */

static bool history_reserve(history *ids, uint64_t id) {
    size_t capacity = ids->capacity ? ids->capacity : 256;
    if (id < ids->capacity) {
        return true;
    }
    while (capacity <= id) {
        capacity *= 2;
    }
    {
        unsigned char *state = (unsigned char *)realloc(ids->state, capacity);
        markdown_core_node_kind *kinds;
        if (!state) {
            return false;
        }
        ids->state = state;
        kinds = (markdown_core_node_kind *)realloc(ids->kinds, capacity * sizeof(*kinds));
        if (!kinds) {
            return false;
        }
        ids->kinds = kinds;
    }
    memset(ids->state + ids->capacity, 0, capacity - ids->capacity);
    ids->capacity = capacity;
    return true;
}

static void history_free(history *ids) {
    free(ids->state);
    free(ids->kinds);
    memset(ids, 0, sizeof(*ids));
}

/* The walk index of each old node's node of the same id in `ids`, a table
 * over the new view's ids; SIZE_MAX for none. */
typedef struct {
    size_t *by_id;
    size_t capacity;
} id_index;

static bool index_ids(id_index *table, const view *taken) {
    size_t index;
    uint64_t most = 0;
    for (index = 0; index < taken->count; index++) {
        most = taken->nodes[index].id > most ? taken->nodes[index].id : most;
    }
    table->capacity = (size_t)most + 1;
    table->by_id = (size_t *)malloc(table->capacity * sizeof(*table->by_id));
    if (!table->by_id) {
        return false;
    }
    memset(table->by_id, 0xff, table->capacity * sizeof(*table->by_id));
    for (index = 0; index < taken->count; index++) {
        table->by_id[taken->nodes[index].id] = index;
    }
    return true;
}

static size_t id_lookup(const id_index *table, uint64_t id) {
    return id < table->capacity ? table->by_id[id] : SIZE_MAX;
}

/* A node's position for 4.5: the first node of the view in walk order of
 * kind `kind` whose source starts at byte `at`. */
static size_t view_at(const view *taken, const char *kind, size_t at) {
    size_t index;
    for (index = 0; index < taken->count; index++) {
        const char *name;
        TS_OK(markdown_core_node_kind_name(taken->nodes[index].kind, &name));
        if (taken->nodes[index].range.start == (int64_t)at && strcmp(name, kind) == 0) {
            return index;
        }
    }
    return SIZE_MAX;
}

/* Where an old node's anchor lies in the new text (4.4): its first byte that
 * survived the step, at the image of that byte. A byte before an edit keeps
 * its offset, a byte after it moves by the edit's change in length, and an
 * append or a flush moves no byte. False for a node with no source or
 * none of whose bytes survived. */
static bool anchor_of(const view_node *old, const ih_step *entry, int64_t *at) {
    int64_t start, end;
    if (old->range.start >= old->range.end) {
        return false;
    }
    if (!entry || entry->kind != IH_EDIT) {
        *at = old->range.start;
        return true;
    }
    start = (int64_t)entry->start;
    end = (int64_t)entry->end;
    if (old->range.start < start) {
        *at = old->range.start;
    } else if (old->range.start >= end || old->range.end > end) {
        *at = (old->range.start > end ? old->range.start : end) - end + start + (int64_t)entry->length;
    } else {
        return false;
    }
    return true;
}

/* 4.2 to 4.5 for one step from `before` to `after`, then the lineage takes
 * the step. */
static void check_identity(run *state, const char *where, size_t step, history *ids, const view *before,
                           const view *after, const ih_step *entry) {
    id_index old_ids = {0}, new_ids = {0};
    bool *changed = (bool *)calloc(after->count, sizeof(bool));
    size_t *continues = (size_t *)malloc(after->count * sizeof(size_t));
    size_t *cursor = (size_t *)calloc(after->count, sizeof(size_t));
    size_t index;
    if (!changed || !continues || !cursor || !index_ids(&old_ids, before) || !index_ids(&new_ids, after) ||
        !history_reserve(ids, (uint64_t)(new_ids.capacity > old_ids.capacity ? new_ids.capacity : old_ids.capacity))) {
        fail(state, "harness", "%s step %zu: out of memory", where, step);
        goto done;
    }
    /* 4.2: unique, and over the lineage kind-stable and never revived. */
    for (index = 0; index < after->count && !stopped(state); index++) {
        const view_node *node = &after->nodes[index];
        if (new_ids.by_id[node->id] != index) {
            fail(state, "4.2", "%s step %zu: id %llu names two nodes", where, step, (unsigned long long)node->id);
        } else if (ids->state[node->id] == 2) {
            fail(state, "4.2", "%s step %zu: retired id %llu names a node again", where, step,
                 (unsigned long long)node->id);
        } else if (ids->state[node->id] && ids->kinds[node->id] != node->kind) {
            fail(state, "4.2", "%s step %zu: id %llu changed its kind", where, step, (unsigned long long)node->id);
        }
    }
    if (stopped(state)) {
        goto done;
    }
    /* 4.4: the root continues the root; within the relation of a matched
     * owner, a node continues the earliest old sibling of its kind whose
     * anchor it holds. An old node's anchor is the first byte of its source,
     * which an append leaves at its offset and an edit moves to its image
     * (anchor_of); a node with no source, or whose bytes the edit removed,
     * has none. One cursor per new owner walks the matched old owner's items, which
     * are in relation and source order, and stands at the earliest not yet
     * passed: a node that continues one passes it and every old sibling
     * before it, and the ones after it its source holds remain for the next
     * new sibling, as the cells of a grid table that span rows start inside
     * the sources of the cells before them (plan 5.9). A node that continues
     * none passes the old siblings no later sibling, starting no earlier,
     * can hold. */
    for (index = 0; index < after->count && !stopped(state); index++) {
        const view_node *node = &after->nodes[index];
        size_t match = SIZE_MAX;
        if (!index) {
            match = 0;
        } else if (continues[node->owner] != SIZE_MAX) {
            const view_node *owner = &before->nodes[continues[node->owner]];
            size_t *at = &cursor[node->owner], scan;
            int64_t anchor = 0;
            for (scan = *at; scan < owner->item_count; scan++) {
                const view_node *old = &before->nodes[before->items[owner->items + scan]];
                const bool anchored = anchor_of(old, entry, &anchor);
                if (old->relation > node->relation ||
                    (old->relation == node->relation && anchored && anchor >= node->range.end)) {
                    break;
                }
                if (old->relation == node->relation && anchored && anchor >= node->range.start &&
                    old->kind == node->kind) {
                    match = before->items[owner->items + scan];
                    *at = scan + 1;
                    break;
                }
            }
            while (match == SIZE_MAX && *at < scan) {
                const view_node *old = &before->nodes[before->items[owner->items + *at]];
                if (old->relation == node->relation && anchor_of(old, entry, &anchor) && anchor >= node->range.start) {
                    break;
                }
                ++*at;
            }
        }
        continues[index] = match;
        if (match != SIZE_MAX ? node->id != before->nodes[match].id : ids->state[node->id] != 0) {
            fail(state, "4.4", "%s step %zu: node %zu (%llu) %s", where, step, index + 1, (unsigned long long)node->id,
                 match != SIZE_MAX ? "does not have the id of the node it continues"
                                   : "continues nothing but has an id the lineage has issued");
        }
    }
    if (stopped(state)) {
        goto done;
    }
    /* 4.3, in post-order: a node is unchanged when the old node of its id
     * has its value and its items, and each item is unchanged. */
    for (index = after->count; index-- > 0;) {
        const view_node *node = &after->nodes[index];
        size_t old = id_lookup(&old_ids, node->id), item;
        bool equal =
            old != SIZE_MAX && before->nodes[old].kind == node->kind &&
            before->nodes[old].value_length == node->value_length &&
            memcmp(before->values + before->nodes[old].value, after->values + node->value, node->value_length) == 0 &&
            before->nodes[old].item_count == node->item_count;
        for (item = 0; equal && item < node->item_count; item++) {
            size_t mine = after->items[node->items + item], theirs = before->items[before->nodes[old].items + item];
            equal = !changed[mine] && after->nodes[mine].id == before->nodes[theirs].id &&
                    after->nodes[mine].relation == before->nodes[theirs].relation;
        }
        changed[index] = !equal;
    }
    /* 4.5: the script's expectations, by position; `flush` has none. */
    if (entry) {
        bool *named = (bool *)calloc(after->count, sizeof(bool));
        bool only = false;
        for (index = 0; named && index < entry->expectation_count; index++) {
            const ih_expectation *expectation = &entry->expectations[index];
            const char *verb = expectation->verb;
            bool ok;
            size_t at;
            if (strcmp(verb, "only") == 0) {
                only = true;
                continue;
            }
            if (strcmp(verb, "retired") == 0) {
                at = view_at(before, expectation->kind, expectation->at);
                ok = at != SIZE_MAX && id_lookup(&new_ids, before->nodes[at].id) == SIZE_MAX;
            } else {
                at = view_at(after, expectation->kind, expectation->at);
                if (at == SIZE_MAX) {
                    ok = false;
                } else if (strcmp(verb, "kept") == 0) {
                    size_t from = view_at(before, expectation->kind, expectation->from);
                    ok = from != SIZE_MAX && before->nodes[from].id == after->nodes[at].id;
                } else if (strcmp(verb, "new") == 0) {
                    ok = ids->state[after->nodes[at].id] == 0;
                    named[at] = true;
                } else {
                    ok = changed[at];
                    named[at] = true;
                }
            }
            if (!ok) {
                fail(state, "4.5", "%s step %zu: expect %s %s %zu does not hold", where, step, verb, expectation->kind,
                     expectation->at);
            }
        }
        for (index = 0; named && only && index < after->count; index++) {
            if (changed[index] && !named[index]) {
                const char *name;
                TS_OK(markdown_core_node_kind_name(after->nodes[index].kind, &name));
                fail(state, "4.5", "%s step %zu: %s at %lld changed, which the script does not name", where, step, name,
                     (long long)after->nodes[index].range.start);
                break;
            }
        }
        if (!named) {
            fail(state, "harness", "%s step %zu: out of memory", where, step);
        }
        free(named);
    }
done:
    /* The lineage takes the step: what the new document names is live, and
     * what the old one named and the new one does not is retired. */
    for (index = 0; ids->capacity && index < before->count; index++) {
        if (id_lookup(&new_ids, before->nodes[index].id) == SIZE_MAX && before->nodes[index].id < ids->capacity) {
            ids->state[before->nodes[index].id] = 2;
        }
    }
    for (index = 0; ids->capacity && index < after->count; index++) {
        if (after->nodes[index].id < ids->capacity && ids->state[after->nodes[index].id] != 2) {
            ids->state[after->nodes[index].id] = 1;
            ids->kinds[after->nodes[index].id] = after->nodes[index].kind;
        }
    }
    free(old_ids.by_id);
    free(new_ids.by_id);
    free(changed);
    free(continues);
    free(cursor);
}

/* A lineage's first document: a fresh parse, numbered from 1 in walk order. */
static void history_open(run *state, const char *where, history *ids, const view *opened) {
    size_t index;
    if (!history_reserve(ids, opened->count + 1)) {
        fail(state, "harness", "%s: out of memory", where);
        return;
    }
    for (index = 0; index < opened->count; index++) {
        if (opened->nodes[index].id != index + 1) {
            fail(state, "4.2", "%s: node %zu of the opened document's walk has id %llu", where, index + 1,
                 (unsigned long long)opened->nodes[index].id);
            return;
        }
        ids->state[index + 1] = 1;
        ids->kinds[index + 1] = opened->nodes[index].kind;
    }
}

/* -------------------------------------------------- 4.6 snapshot meaning */

static bool kind_is(const view *taken, size_t index, const char *kind) {
    const char *name;
    TS_OK(markdown_core_node_kind_name(taken->nodes[index].kind, &name));
    return strcmp(name, kind) == 0;
}

/* Whether node `index` is an inline root: a block whose own inline content
 * asks its lookups, or a block with an inline field, a callout's title or a
 * definition's term, which asks them from its field. */
static bool inline_root(const view *taken, size_t index) {
    static const char *const kinds[] = {"Paragraph", "Heading", "TableCaption", "TableCell", "Callout", "Definition"};
    size_t kind;
    for (kind = 0; kind < sizeof(kinds) / sizeof(kinds[0]); kind++) {
        if (kind_is(taken, index, kinds[kind])) {
            return true;
        }
    }
    return kind_is(taken, index, "DirectiveLabel") && kind_is(taken, taken->nodes[index].owner, "DirectiveBlock");
}

/* A lookup of the expected document: the label it names in one of the
 * tables, and the inline root that asks with the relation its content is
 * in. */
typedef struct asking {
    int table;
    markdown_core_string label;
    size_t root, relation;
} asking;

/* The lookup node `index` answered, if it is one: a reference link or image,
 * or a citation of a footnote or specimen definition. */
static bool lookup_of(const view *taken, size_t index, asking *lookup) {
    const markdown_core_node *node = taken->nodes[index].object;
    size_t below = index, root;
    if (kind_is(taken, index, "Link") || kind_is(taken, index, "Embedded")) {
        markdown_core_destination destination;
        TS_OK(markdown_core_node_destination(node, &destination));
        if (destination.kind != MARKDOWN_CORE_DESTINATION_REFERENCE) {
            return false;
        }
        lookup->table = TABLE_REFERENCE;
        lookup->label = destination.label;
    } else if (kind_is(taken, index, "Citation")) {
        markdown_core_referent referent;
        TS_OK(markdown_core_citation_referent(node, &referent));
        if (referent.kind == MARKDOWN_CORE_REFERENT_BIB || referent.note) {
            return false;
        }
        lookup->table = referent.kind == MARKDOWN_CORE_REFERENT_FOOTNOTE ? TABLE_FOOTNOTE : TABLE_SPECIMEN;
        lookup->label = referent.label;
    } else {
        return false;
    }
    for (root = taken->nodes[index].owner; root != SIZE_MAX && !inline_root(taken, root);
         root = taken->nodes[root].owner) {
        below = root;
    }
    if (root == SIZE_MAX) {
        return false;
    }
    lookup->root = root;
    lookup->relation = taken->nodes[below].relation;
    return true;
}

/* Where the first definition of the lookup's label starts, as the expected
 * document's tables answer it, or INT64_MAX for none. For a reference label
 * that is its winner: the first Reference, or the first heading when no
 * Reference declares it. */
static int64_t definition_start(const markdown_core_document *document, const view *taken, const asking *lookup) {
    const markdown_core_node *first =
        lookup->table == TABLE_FOOTNOTE   ? markdown_core_document_footnote_for(document, lookup->label)
        : lookup->table == TABLE_SPECIMEN ? markdown_core_document_specimen_for(document, lookup->label)
                                          : markdown_core_document_reference_for(document, lookup->label);
    size_t at = view_find(taken, first);
    return at == SIZE_MAX ? INT64_MAX : taken->nodes[at].range.start;
}

/* The places plan 5.5 lets a snapshot differ at, computed from the expected
 * document alone: for each node, the relations of its inline content a
 * snapshot may read otherwise. They are those of every inline root that names
 * a label whose first definition starts at or after the root, and, in turn,
 * of every root that names a label a heading among them declares. NULL when
 * memory ran out. */
static uint32_t *free_relations(const markdown_core_document *document, const view *taken) {
    uint32_t *relations = (uint32_t *)calloc(taken->count, sizeof(*relations));
    asking *lookups = (asking *)malloc(taken->count * sizeof(*lookups));
    bool *done = (bool *)calloc(taken->count, sizeof(*done));
    size_t count = 0, index;
    bool grew = true;
    if (!relations || !lookups || !done) {
        free(relations);
        free(lookups);
        free(done);
        return NULL;
    }
    for (index = 0; index < taken->count; index++) {
        if (lookup_of(taken, index, &lookups[count])) {
            count++;
        }
    }
    while (grew) {
        grew = false;
        for (index = 0; index < count; index++) {
            const asking *lookup = &lookups[index];
            bool reads_otherwise = definition_start(document, taken, lookup) >= taken->nodes[lookup->root].range.start;
            if (!reads_otherwise && lookup->table == TABLE_REFERENCE) {
                size_t winner = view_find(taken, markdown_core_document_reference_for(document, lookup->label));
                reads_otherwise = winner != SIZE_MAX && kind_is(taken, winner, "Heading") && relations[winner];
            }
            if (!done[index] && reads_otherwise) {
                done[index] = true;
                relations[lookup->root] |= (uint32_t)1 << lookup->relation;
                grew = true;
            }
        }
    }
    free(lookups);
    free(done);
    return relations;
}

/* Where each node's subtree ends in walk order. */
static size_t *subtree_ends(const view *taken) {
    size_t *ends = (size_t *)malloc(taken->count * sizeof(*ends)), index;
    if (!ends) {
        return NULL;
    }
    for (index = taken->count; index-- > 0;) {
        const view_node *node = &taken->nodes[index];
        ends[index] = node->item_count ? ends[taken->items[node->items + node->item_count - 1]] : index + 1;
    }
    return ends;
}

/* Where `needle` first occurs in the `length` bytes at `text`, or NULL. */
static const char *find_text(const char *text, size_t length, const char *needle) {
    size_t size = strlen(needle), at;
    for (at = 0; at + size <= length; at++) {
        if (memcmp(text + at, needle, size) == 0) {
            return text + at;
        }
    }
    return NULL;
}

/* Whether two nodes' values are equal; a heading's anchor is compared by
 * check_anchors. */
static bool same_value(const view *actual, size_t mine, const view *expected, size_t theirs) {
    const view_node *a = &actual->nodes[mine], *b = &expected->nodes[theirs];
    const char *left = actual->values + a->value, *right = expected->values + b->value;
    if (kind_is(expected, theirs, "Heading")) {
        const char *left_anchor = find_text(left, a->value_length, " anchor=");
        const char *left_rest = find_text(left, a->value_length, " attributes=");
        const char *right_anchor = find_text(right, b->value_length, " anchor=");
        const char *right_rest = find_text(right, b->value_length, " attributes=");
        if (left_anchor && left_rest && right_anchor && right_rest) {
            return left_anchor - left == right_anchor - right &&
                   memcmp(left, right, (size_t)(left_anchor - left)) == 0 &&
                   (left + a->value_length) - left_rest == (right + b->value_length) - right_rest &&
                   memcmp(left_rest, right_rest, (size_t)((left + a->value_length) - left_rest)) == 0;
        }
    }
    return a->value_length == b->value_length && memcmp(left, right, a->value_length) == 0;
}

/* Marks the subtrees of the items of `owner` from `*at` on that are in
 * `relation` as free, and moves `*at` past them. */
static void free_items(const view *taken, const size_t *ends, bool *flags, const view_node *owner, size_t *at,
                       size_t relation) {
    for (; *at < owner->item_count && taken->nodes[taken->items[owner->items + *at]].relation == relation; ++*at) {
        size_t item = taken->items[owner->items + *at], node;
        for (node = item; node < ends[item]; node++) {
            flags[node] = true;
        }
    }
}

/* Pairs the subject's snapshot with the expected document node for node,
 * from the roots down, everywhere but in the free relations; each pair has
 * the same kind and value, its heading anchors aside, and in a free root
 * the same kind and extent. False after a failure. */
static bool correspond(run *state, const char *where, size_t step, const view *actual, const view *expected,
                       const uint32_t *relations, correspondence *pairs) {
    size_t *actual_ends = subtree_ends(actual), *expected_ends = subtree_ends(expected);
    size_t *stack = (size_t *)malloc(2 * (expected->count + 1) * sizeof(*stack)), depth = 0;
    bool ok = true;
    pairs->to_actual = (size_t *)malloc(expected->count * sizeof(*pairs->to_actual));
    pairs->actual_free = (bool *)calloc(actual->count, sizeof(bool));
    pairs->expected_free = (bool *)calloc(expected->count, sizeof(bool));
    if (!actual_ends || !expected_ends || !stack || !pairs->to_actual || !pairs->actual_free || !pairs->expected_free) {
        fail(state, "harness", "%s step %zu: out of memory", where, step);
        ok = false;
        goto done;
    }
    memset(pairs->to_actual, 0xff, expected->count * sizeof(*pairs->to_actual));
    stack[depth++] = 0;
    stack[depth++] = 0;
    while (ok && depth) {
        size_t theirs = stack[--depth], mine = stack[--depth], mine_at = 0, theirs_at = 0;
        const view_node *a = &actual->nodes[mine], *b = &expected->nodes[theirs];
        uint32_t free_set = relations[theirs];
        pairs->to_actual[theirs] = mine;
        if (a->kind != b->kind ||
            (free_set ? a->extent.lead != b->extent.lead || a->extent.span != b->extent.span
                      : !same_value(actual, mine, expected, theirs) || a->item_count != b->item_count)) {
            fail(state, "4.1", "%s step %zu: node %zu differs from the fresh parse where a snapshot may not differ",
                 where, step, theirs + 1);
            ok = false;
            break;
        }
        while (mine_at < a->item_count || theirs_at < b->item_count) {
            size_t mine_item = mine_at < a->item_count ? actual->items[a->items + mine_at] : SIZE_MAX;
            size_t theirs_item = theirs_at < b->item_count ? expected->items[b->items + theirs_at] : SIZE_MAX;
            size_t relation = theirs_item == SIZE_MAX ? actual->nodes[mine_item].relation
                              : mine_item == SIZE_MAX ? expected->nodes[theirs_item].relation
                              : actual->nodes[mine_item].relation < expected->nodes[theirs_item].relation
                                  ? actual->nodes[mine_item].relation
                                  : expected->nodes[theirs_item].relation;
            if (relation < 32 && (free_set >> relation & 1)) {
                free_items(actual, actual_ends, pairs->actual_free, a, &mine_at, relation);
                free_items(expected, expected_ends, pairs->expected_free, b, &theirs_at, relation);
                continue;
            }
            if (mine_item == SIZE_MAX || theirs_item == SIZE_MAX ||
                actual->nodes[mine_item].relation != expected->nodes[theirs_item].relation) {
                fail(state, "4.1", "%s step %zu: node %zu's items differ from the fresh parse's", where, step,
                     theirs + 1);
                ok = false;
                break;
            }
            stack[depth++] = mine_item;
            stack[depth++] = theirs_item;
            mine_at++;
            theirs_at++;
        }
    }
done:
    free(actual_ends);
    free(expected_ends);
    free(stack);
    return ok;
}

/* A set of the names anchors can share a base under. */
typedef struct name_set {
    markdown_core_string *names;
    size_t count, capacity;
} name_set;

static uint64_t name_hash(markdown_core_string name) {
    uint64_t hash = 14695981039346656037ULL;
    size_t at;
    for (at = 0; at < name.length; at++) {
        hash = (hash ^ name.data[at]) * 1099511628211ULL;
    }
    return hash;
}

static markdown_core_string *name_slot(const name_set *set, markdown_core_string name) {
    size_t at = (size_t)name_hash(name) & (set->capacity - 1);
    while (set->names[at].data &&
           (set->names[at].length != name.length || memcmp(set->names[at].data, name.data, name.length) != 0)) {
        at = (at + 1) & (set->capacity - 1);
    }
    return &set->names[at];
}

/* Adds `name`; false when memory ran out. */
static bool name_add(name_set *set, markdown_core_string name) {
    markdown_core_string *slot;
    if (2 * (set->count + 1) > set->capacity) {
        size_t capacity = set->capacity ? 2 * set->capacity : 64, at;
        name_set grown = {(markdown_core_string *)calloc(capacity, sizeof(markdown_core_string)), 0, capacity};
        if (!grown.names) {
            return false;
        }
        for (at = 0; at < set->capacity; at++) {
            if (set->names[at].data) {
                *name_slot(&grown, set->names[at]) = set->names[at];
                grown.count++;
            }
        }
        free(set->names);
        *set = grown;
    }
    slot = name_slot(set, name);
    if (!slot->data) {
        *slot = name;
        set->count++;
    }
    return true;
}

static bool name_find(const name_set *set, markdown_core_string name) {
    return set->capacity && name_slot(set, name)->data;
}

/* The names an automatic anchor `name` can stand for the base of: itself,
 * and, when it ends in `-` and digits after something, what comes before. */
static size_t anchor_bases(markdown_core_string name, markdown_core_string bases[2]) {
    size_t end = name.length;
    bases[0] = name;
    while (end > 0 && name.data[end - 1] >= '0' && name.data[end - 1] <= '9') {
        end--;
    }
    if (end < name.length && end > 1 && name.data[end - 1] == '-') {
        bases[1] = (markdown_core_string){name.data, end - 1};
        return 2;
    }
    return 1;
}

/* Adds the bases of an anchor. */
static bool add_bases(name_set *set, markdown_core_optional_string anchor) {
    markdown_core_string bases[2];
    size_t count, at;
    if (!anchor.has_value || !anchor.value.data) {
        return true;
    }
    count = anchor_bases(anchor.value, bases);
    for (at = 0; at < count; at++) {
        if (!name_add(set, bases[at])) {
            return false;
        }
    }
    return true;
}

/* Whether the anchor shares a base with a name of the set. */
static bool shares_base(const name_set *set, markdown_core_optional_string anchor) {
    markdown_core_string bases[2];
    size_t count, at;
    if (!anchor.has_value || !anchor.value.data) {
        return false;
    }
    count = anchor_bases(anchor.value, bases);
    for (at = 0; at < count; at++) {
        if (name_find(set, bases[at])) {
            return true;
        }
    }
    return false;
}

static bool same_anchor(markdown_core_optional_string a, markdown_core_optional_string b) {
    return a.has_value == b.has_value && (!a.has_value || (a.value.length == b.value.length &&
                                                           memcmp(a.value.data, b.value.data, a.value.length) == 0));
}

typedef struct placed {
    int64_t start;
    size_t index;
} placed;

static int placed_order(const void *left, const void *right) {
    const placed *a = (const placed *)left, *b = (const placed *)right;
    return a->start < b->start ? -1 : a->start > b->start ? 1 : a->index < b->index ? -1 : a->index > b->index;
}

/* Each heading's anchor equals the fresh parse's: a snapshot assigns anchors
 * by the rule a fresh parse does, over the anchors of its own nodes. The
 * harness cannot tell an automatic anchor from an authored one, nor read a
 * heading's base, so it lets a heading's anchor differ when its text is a
 * free root, or when the anchor could share its base with an anchor of a
 * free place or with the fresh or the snapshot's anchor of an earlier
 * heading whose anchor may differ. */
static void check_anchors(run *state, const char *where, size_t step, const view *actual, const view *expected,
                          const uint32_t *relations, const correspondence *pairs) {
    placed *headings = (placed *)malloc(expected->count * sizeof(*headings));
    size_t count = 0, index;
    /* Every anchor of a free place, and of an earlier heading whose anchor
     * may differ. */
    name_set loose = {0}, earlier = {0};
    bool ok = headings != NULL;
    for (index = 0; ok && index < expected->count; index++) {
        if (kind_is(expected, index, "Heading")) {
            headings[count++] = (placed){expected->nodes[index].range.start, index};
        }
        if (pairs->expected_free[index]) {
            ok = add_bases(&loose, markdown_core_node_anchor(expected->nodes[index].object));
        }
    }
    for (index = 0; ok && index < actual->count; index++) {
        if (pairs->actual_free[index]) {
            ok = add_bases(&loose, markdown_core_node_anchor(actual->nodes[index].object));
        }
    }
    if (!ok) {
        fail(state, "harness", "%s step %zu: out of memory", where, step);
        goto done;
    }
    qsort(headings, count, sizeof(*headings), placed_order);
    for (index = 0; index < count && !stopped(state); index++) {
        size_t heading = headings[index].index, mine = pairs->to_actual[heading];
        markdown_core_optional_string theirs = markdown_core_node_anchor(expected->nodes[heading].object), own;
        bool may_differ;
        if (mine == SIZE_MAX) {
            continue;
        }
        own = markdown_core_node_anchor(actual->nodes[mine].object);
        may_differ = relations[heading] || shares_base(&loose, theirs) || shares_base(&earlier, theirs);
        if (!may_differ && !same_anchor(own, theirs)) {
            fail(state, "4.1", "%s step %zu: heading %zu's anchor differs from the fresh parse's", where, step,
                 heading + 1);
            break;
        }
        if (may_differ && (!add_bases(&earlier, theirs) || !add_bases(&earlier, own))) {
            fail(state, "harness", "%s step %zu: out of memory", where, step);
            break;
        }
    }
done:
    free(headings);
    free(loose.names);
    free(earlier.names);
}

/* ------------------------------------------------------------------ steps */

/* The subject's document, viewed in `after` with its dump, equals
 * `expected` everywhere, ids aside: the dump, the extents, the queries and
 * the tables. Failures are `oracle`'s. */
static void check_equal(run *state, const char *oracle, const char *where, size_t step, ih_unit unit,
                        const markdown_core_document *document, const view *after, const uint8_t *dump,
                        size_t dump_length, const markdown_core_document *expected, const ih_text *model) {
    size_t expected_length = 0, index;
    uint8_t *expected_dump = dump_of(expected, model->bytes, model->length, &expected_length);
    view expected_view = {0};
    if (!same(dump, dump_length, expected_dump, expected_length)) {
        fail(state, oracle, "%s step %zu: the document differs from the %s", where, step,
             strcmp(oracle, "4.1") == 0 ? "fresh parse of the text" : "snapshot of one append of the text");
        goto done;
    }
    if (!view_take(&expected_view, expected, NULL, 0)) {
        fail(state, "harness", "%s step %zu: a document could not be viewed", where, step);
        goto done;
    }
    for (index = 0; index < after->count && index < expected_view.count; index++) {
        if (after->nodes[index].extent.lead != expected_view.nodes[index].extent.lead ||
            after->nodes[index].extent.span != expected_view.nodes[index].extent.span) {
            break;
        }
    }
    if (after->count != expected_view.count || index < after->count) {
        fail(state, oracle, "%s step %zu: node %zu's extent differs from the expected document's", where, step,
             index + 1);
        goto done;
    }
    check_queries(state, oracle, where, step, unit, document, after, expected, &expected_view, expected_dump,
                  expected_length, model->bytes, model->length);
    check_tables(state, oracle, where, step, document, after, expected, &expected_view, NULL);
done:
    view_free(&expected_view);
    markdown_core_dump_free(expected_dump);
}

/* 4.6's snapshot meaning: the subject's snapshot, viewed in `after`, equals
 * `fresh`, the fresh parse of the text, everywhere but where plan 5.5 lets
 * it differ: node for node in the dumps, which carry every scope, and in the
 * definition tables. The position queries are chunking independence's.
 * Failures are 4.1's: the snapshot equals the fresh parse there. */
static void check_meaning(run *state, const char *where, size_t step, ih_unit unit,
                          const markdown_core_document *document, const view *after,
                          const markdown_core_document *fresh, const ih_text *model) {
    size_t fresh_length = 0;
    uint8_t *fresh_dump = dump_of(fresh, model->bytes, model->length, &fresh_length);
    view expected = {0};
    uint32_t *relations = NULL;
    correspondence pairs = {0};
    if (!fresh_dump || !view_take(&expected, fresh, fresh_dump, fresh_length)) {
        fail(state, "harness", "%s step %zu: a document could not be viewed", where, step);
        goto done;
    }
    relations = free_relations(fresh, &expected);
    if (!relations) {
        fail(state, "harness", "%s step %zu: out of memory", where, step);
        goto done;
    }
    if (!correspond(state, where, step, after, &expected, relations, &pairs)) {
        goto done;
    }
    check_anchors(state, where, step, after, &expected, relations, &pairs);
    check_tables(state, "4.1", where, step, document, after, fresh, &expected, &pairs);
done:
    correspondence_free(&pairs);
    free(relations);
    view_free(&expected);
    markdown_core_dump_free(fresh_dump);
}

/* Every oracle after the open (`before` NULL) or a step, then, from the view
 * it takes of the document into `after`, 4.2 to 4.5 against the previous
 * view. A snapshot, which the open and every append return, is checked by
 * 4.6: it equals the snapshot a new subject returns for one append of the
 * text, and the fresh parse but where plan 5.5 lets it differ. The flushed
 * document is checked by 4.1. */
static bool check_document(run *state, const char *where, size_t step, ih_unit unit, void *subject,
                           const markdown_core_document *document, const ih_text *model, history *ids,
                           const view *before, view *after, const ih_step *entry, bool snapshot) {
    size_t length = 0, actual_length = 0;
    const uint8_t *text = state->subject->text(subject, &length);
    uint8_t *actual = NULL;
    markdown_core_document *fresh = NULL;
    bool taken = false;
    memset(after, 0, sizeof(*after));
    if (!same(text, length, model->bytes, model->length) && !(length == 0 && model->length == 0)) {
        fail(state, "4.1", "%s step %zu: the subject's text differs from the text model", where, step);
        return false;
    }
    if (markdown_core_document_parse_in(model->bytes, model->length, unit_of(unit), &fresh) != MARKDOWN_CORE_OK) {
        fail(state, "harness", "%s step %zu: the fresh parse failed", where, step);
        return false;
    }
    actual = dump_of(document, model->bytes, model->length, &actual_length);
    if (!actual || !view_take(after, document, actual, actual_length)) {
        fail(state, "harness", "%s step %zu: a document could not be viewed", where, step);
        goto done;
    }
    taken = true;
    if (snapshot) {
        const markdown_core_document *once = NULL;
        void *other = state->subject->open(unit, model->bytes, model->length, &once);
        if (!other) {
            fail(state, "harness", "%s step %zu: a new subject did not open on the text", where, step);
            goto done;
        }
        check_equal(state, "4.6", where, step, unit, document, after, actual, actual_length, once, model);
        state->subject->close(other);
        if (!stopped(state)) {
            check_meaning(state, where, step, unit, document, after, fresh, model);
        }
    } else {
        check_equal(state, "4.1", where, step, unit, document, after, actual, actual_length, fresh, model);
    }
    if (stopped(state)) {
        goto done;
    }
    if (!state->subject->keeps) {
        /* A subject that numbers every parse from 1 has no lineage. */
    } else if (before) {
        check_identity(state, where, step, ids, before, after, entry);
    } else {
        history_open(state, where, ids, after);
    }
done:
    markdown_core_dump_free(actual);
    markdown_core_document_free(fresh);
    if (!taken) {
        view_free(after);
    }
    return taken;
}

/* ------------------------------------------------------------------ runs */

/* The script's last step, `flush`: 4.1 to 4.4 hold against the last
 * snapshot, and then an append or a flush fails with the flushed status
 * and leaves the text as it was (4.7). */
static void check_flush(run *state, const char *where, size_t step, ih_unit unit, void *subject, const ih_text *model,
                        history *ids, view *previous) {
    const markdown_core_document *final = NULL, *refused = NULL;
    size_t length = 0;
    view after;
    if (state->subject->flush(subject, &final) != MARKDOWN_CORE_OK) {
        fail(state, "harness", "%s: flush failed", where);
        return;
    }
    if (check_document(state, where, step, unit, subject, final, model, ids, previous, &after, NULL, false)) {
        view_free(previous);
        *previous = after;
    }
    if (state->subject->append(subject, (const uint8_t *)"x", 1, &refused) != MARKDOWN_CORE_SESSION_FLUSHED ||
        refused) {
        fail(state, "4.7", "%s: an append after flush did not fail with the flushed status", where);
    } else if (!state->subject->text(subject, &length) || length != model->length) {
        fail(state, "4.7", "%s: an append after flush changed the text", where);
    }
    if (state->subject->flush(subject, &refused) != MARKDOWN_CORE_SESSION_FLUSHED || refused) {
        fail(state, "4.7", "%s: a flush after flush did not fail with the flushed status", where);
    }
}

/* 4.12: the bytes the edit read again hold the bytes it wrote, lie in the
 * text, and keep to the script's bound when it states one. */
static void check_reread(run *state, const char *where, size_t step, void *subject, const ih_step *entry,
                         const ih_text *model) {
    size_t from = 0, to = 0;
    if (!state->subject->reread) {
        return;
    }
    state->subject->reread(subject, &from, &to);
    if (from > to || to > model->length || from > entry->start || to < entry->start + entry->length) {
        fail(state, "4.12", "%s step %zu: the edit read again [%zu, %zu), which does not hold the [%zu, %zu) it wrote",
             where, step, from, to, entry->start, entry->start + entry->length);
    } else if (entry->bounded &&
               (entry->exact ? from != entry->from || to != entry->to : from < entry->from || to > entry->to)) {
        fail(state, "4.12", "%s step %zu: the edit read again [%zu, %zu), not %s [%zu, %zu)", where, step, from, to,
             entry->exact ? "exactly" : "within", entry->from, entry->to);
    }
}

/* A position of the text model in `unit`. */
static size_t position_in(const ih_text *model, ih_unit unit, size_t offset) {
    return unit == IH_UTF16 ? ih_utf16_units(model->bytes, offset) : offset;
}

/* Whether `offset` of `model` stands between the CR and the LF of a CRLF. */
static bool inside_crlf(const ih_text *model, size_t offset) {
    return offset && offset < model->length && model->bytes[offset - 1] == '\r' && model->bytes[offset] == '\n';
}

/* The script's edit `entry` as a cursor names it, in `*edit`: a cursor stands
 * at no offset between the CR and the LF of a CRLF, so a start there takes
 * the CR in and writes it back, and an end there the LF, in the text
 * `*joined` holds, which the caller frees; the edit itself otherwise. False
 * when storage could not be had. */
static bool cursor_edit(const ih_text *model, const ih_step *entry, ih_step *edit, uint8_t **joined) {
    const bool before = inside_crlf(model, entry->start), after = inside_crlf(model, entry->end);
    *edit = *entry;
    *joined = NULL;
    if (!before && !after) {
        return true;
    }
    *joined = malloc(entry->length + 2);
    if (!*joined) {
        return false;
    }
    size_t length = 0;
    if (before) {
        (*joined)[length++] = '\r';
        edit->start--;
    }
    if (entry->length) {
        memcpy(*joined + length, entry->text, entry->length);
    }
    length += entry->length;
    if (after) {
        (*joined)[length++] = '\n';
        edit->end++;
    }
    edit->text = *joined;
    edit->length = length;
    return true;
}

/* An edit after the flush, as a cursor names it (cursor_edit): 4.1 to 4.5
 * against the previous document, with 4.4's anchors at their image (4.11),
 * and 4.12. */
static bool check_edit(run *state, const char *where, size_t step, ih_unit unit, void *subject, ih_text *model,
                       history *ids, view *previous, const ih_step *script) {
    const markdown_core_document *next = NULL;
    markdown_core_status status;
    view after;
    ih_step cursor;
    uint8_t *joined;
    const ih_step *entry = &cursor;
    if (!state->subject->edit) {
        fail(state, "harness", "%s step %zu: the %s subject takes no edits", where, step, state->subject->name);
        return false;
    }
    if (script->start > script->end || script->end > model->length ||
        !ih_utf8_boundary(model->bytes, model->length, script->start) ||
        !ih_utf8_boundary(model->bytes, model->length, script->end)) {
        fail(state, "harness", "%s step %zu: the script's edit names no range of the text", where, step);
        return false;
    }
    if (!cursor_edit(model, script, &cursor, &joined)) {
        fail(state, "harness", "%s step %zu: out of memory", where, step);
        return false;
    }
    status = state->subject->edit(subject, position_in(model, unit, entry->start), position_in(model, unit, entry->end),
                                  entry->text, entry->length, &next);
    if (status != MARKDOWN_CORE_OK || !next) {
        fail(state, status == MARKDOWN_CORE_ALLOCATION_FAILED ? "harness" : "4.11",
             "%s step %zu: an edit after flush failed with status %d", where, step, (int)status);
        free(joined);
        return false;
    }
    if (!ih_text_replace(model, entry->start, entry->end, entry->text, entry->length)) {
        fail(state, "harness", "%s step %zu: out of memory", where, step);
        free(joined);
        return false;
    }
    if (!check_document(state, where, step, unit, subject, next, model, ids, previous, &after, entry, false)) {
        free(joined);
        return false;
    }
    view_free(previous);
    *previous = after;
    if (!stopped(state)) {
        check_reread(state, where, step, subject, entry, model);
    }
    free(joined);
    return true;
}

/* 4.13: an edit the subject must refuse fails with its status, answers no
 * document and leaves the text as it was. */
static void check_refusal(run *state, const char *where, size_t step, ih_unit unit, void *subject, const ih_text *model,
                          const ih_step *entry) {
    const markdown_core_document *refused = NULL;
    const uint8_t *text;
    size_t length = 0;
    markdown_core_status status;
    if (entry->unit != unit) {
        return;
    }
    if (!state->subject->edit) {
        fail(state, "harness", "%s step %zu: the %s subject takes no edits", where, step, state->subject->name);
        return;
    }
    status = state->subject->edit(subject, entry->start, entry->end, entry->text, entry->length, &refused);
    text = state->subject->text(subject, &length);
    if (status != entry->refusal || refused) {
        fail(state, "4.13", "%s step %zu: the edit was answered with status %d, not %d", where, step, (int)status,
             (int)entry->refusal);
    } else if (!text || !same(text, length, model->bytes, model->length)) {
        fail(state, "4.13", "%s step %zu: a refused edit changed the text", where, step);
    }
}

void run_steps(run *state, const char *where, ih_unit unit, const uint8_t *opened, size_t opened_length,
               const ih_step *steps, size_t count) {
    ih_text model = {0};
    const markdown_core_document *current = NULL;
    history ids = {0};
    view previous = {0};
    void *subject = state->subject->open(unit, opened, opened_length, &current);
    size_t index;
    bool viewed, flushed = false;
    if (!subject || !ih_text_assign(&model, opened, opened_length)) {
        fail(state, "harness", "%s: the subject did not open", where);
        if (subject) {
            state->subject->close(subject);
        }
        ih_text_free(&model);
        return;
    }
    viewed = check_document(state, where, 0, unit, subject, current, &model, &ids, NULL, &previous, NULL, true);
    for (index = 0; viewed && index < count && !stopped(state); index++) {
        const ih_step *step = &steps[index];
        const markdown_core_document *next = NULL;
        view after;
        if (step->kind == IH_FLUSH || (step->kind == IH_EDIT && !flushed)) {
            /* A script flushes once, before its first edit. */
            if (flushed) {
                fail(state, "harness", "%s step %zu: the script flushes twice", where, index + 1);
                break;
            }
            flushed = true;
            check_flush(state, where, index + 1, unit, subject, &model, &ids, &previous);
            if (step->kind == IH_FLUSH) {
                continue;
            }
        }
        if (step->kind == IH_EDIT) {
            viewed = check_edit(state, where, index + 1, unit, subject, &model, &ids, &previous, step);
            continue;
        }
        if (step->kind == IH_REJECT) {
            check_refusal(state, where, index + 1, unit, subject, &model, step);
            continue;
        }
        if (flushed) {
            fail(state, "harness", "%s step %zu: the script appends after flush", where, index + 1);
            break;
        }
        if (state->subject->append(subject, step->text, step->length, &next) != MARKDOWN_CORE_OK ||
            !ih_text_append(&model, step->text, step->length)) {
            fail(state, "harness", "%s step %zu: an append failed", where, index + 1);
            viewed = false;
            break;
        }
        current = next;
        viewed = check_document(state, where, index + 1, unit, subject, current, &model, &ids, &previous, &after, step,
                                true);
        view_free(&previous);
        previous = after;
    }
    if (viewed && !flushed && !stopped(state)) {
        check_flush(state, where, count + 1, unit, subject, &model, &ids, &previous);
    }
    view_free(&previous);
    history_free(&ids);
    state->subject->close(subject);
    ih_text_free(&model);
}

/* 3.1: parts that keep their meaning when joined. */
void check_parts(run *state, const char *where, const uint8_t *document, size_t length, const ih_case *entry) {
    size_t index, from = 0, sum = 0;
    markdown_core_document *whole;
    for (index = 0; index < entry->part_count; index++) {
        markdown_core_document *part;
        if (entry->parts[index] > length - from) {
            fail(state, "3.1", "%s: part %zu runs past the document", where, index + 1);
            return;
        }
        part = ts_ast_parse(document + from, entry->parts[index]);
        if (!part) {
            fail(state, "harness", "%s: part %zu did not parse", where, index + 1);
            return;
        }
        sum += markdown_core_node_children(markdown_core_document_root(part)).count;
        markdown_core_document_free(part);
        from += entry->parts[index];
    }
    if (from != length) {
        fail(state, "3.1", "%s: the parts do not cover the document", where);
        return;
    }
    whole = ts_ast_parse(document, length);
    if (!whole || markdown_core_node_children(markdown_core_document_root(whole)).count != sum) {
        fail(state, "3.1", "%s: the composite's root children differ from its parts' sum %zu", where, sum);
    }
    markdown_core_document_free(whole);
}
