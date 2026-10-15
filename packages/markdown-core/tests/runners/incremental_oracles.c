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

#define SCOPE_SAMPLE 4

/* 4.1's queries on a sample of nodes: scope(of:in:) and node(at:in:) at each
 * end of a node's scope and beside it answer as the fresh parse's
 * corresponding node does, and the fresh parse's own scopes are the dump's.
 * Each step takes SCOPE_SAMPLE nodes spread over the walk, and the next step
 * the nodes after them, so consecutive steps cover the whole document: every
 * query costs a walk of the document. */
static void check_queries(run *state, const char *where, size_t step, ih_unit unit,
                          const markdown_core_document *document, const view *actual,
                          const markdown_core_document *fresh, const view *expected, const uint8_t *fresh_dump,
                          size_t fresh_dump_size, const uint8_t *text, size_t length) {
    size_t lines = 0, stride = actual->count / SCOPE_SAMPLE + 1, index, line = 0;
    size_t *starts = line_starts(text, length, &lines);
    const char *cursor = (const char *)fresh_dump, *end = cursor + fresh_dump_size;
    if (!starts) {
        fail(state, "harness", "%s step %zu: out of memory", where, step);
        return;
    }
    for (index = 0; index < actual->count && !stopped(state); index++) {
        markdown_core_scope *mine = NULL, *theirs = NULL;
        size_t mine_count = 0, theirs_count = 0, range;
        const char *scope = NULL;
        /* The fresh dump's line of node `index`. */
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
            markdown_core_document_scope(fresh, expected->nodes[index].object, text, length, &theirs, &theirs_count) !=
                MARKDOWN_CORE_OK) {
            fail(state, "4.1", "%s step %zu: a scope query failed for node %zu", where, step, index + 1);
        } else if (!same_scopes(mine, mine_count, theirs, theirs_count)) {
            fail(state, "4.1", "%s step %zu: node %zu's scopes differ from the fresh parse's", where, step, index + 1);
        } else if (!scope || !dump_scopes(scope, text, starts, lines, unit, theirs, theirs_count)) {
            fail(state, "4.1", "%s step %zu: node %zu's fresh scopes differ from its dump", where, step, index + 1);
        }
        for (range = 0; range < mine_count && same_scopes(mine, mine_count, theirs, theirs_count); range++) {
            markdown_core_position positions[4] = {mine[range].start, mine[range].end, mine[range].start,
                                                   mine[range].end};
            size_t at;
            positions[2].column--;
            positions[3].column++;
            for (at = 0; at < 4; at++) {
                const markdown_core_node *hit = NULL, *fresh_hit = NULL;
                if (positions[at].column < 1) {
                    continue;
                }
                if (markdown_core_document_node_at(document, positions[at], text, length, &hit) != MARKDOWN_CORE_OK ||
                    markdown_core_document_node_at(fresh, positions[at], text, length, &fresh_hit) !=
                        MARKDOWN_CORE_OK ||
                    view_find(actual, hit) != view_find(expected, fresh_hit)) {
                    fail(state, "4.1", "%s step %zu: node(at: %d:%d) differs from the fresh parse's", where, step,
                         positions[at].line, positions[at].column);
                    break;
                }
            }
        }
        markdown_core_scopes_free(mine);
        markdown_core_scopes_free(theirs);
    }
    free(starts);
}

/* 4.1's definition tables: the lists, and the lookup of every label. */
static void check_tables(run *state, const char *where, size_t step, const markdown_core_document *document,
                         const view *actual, const markdown_core_document *fresh, const view *expected) {
    size_t kind, index;
    for (kind = 0; kind < 2; kind++) {
        size_t count =
            kind ? markdown_core_document_specimen_count(document) : markdown_core_document_footnote_count(document);
        size_t fresh_count =
            kind ? markdown_core_document_specimen_count(fresh) : markdown_core_document_footnote_count(fresh);
        if (count != fresh_count) {
            fail(state, "4.1", "%s step %zu: the %s table differs from the fresh parse's", where, step,
                 kind ? "specimen" : "footnote");
            continue;
        }
        for (index = 0; index < count; index++) {
            const markdown_core_node *mine = NULL, *theirs = NULL;
            markdown_core_optional_string label = {0};
            markdown_core_optional_i64 start;
            TS_OK(kind ? markdown_core_document_specimen_at(document, index, &mine)
                       : markdown_core_document_footnote_at(document, index, &mine));
            TS_OK(kind ? markdown_core_document_specimen_at(fresh, index, &theirs)
                       : markdown_core_document_footnote_at(fresh, index, &theirs));
            if (view_find(actual, mine) != view_find(expected, theirs)) {
                fail(state, "4.1", "%s step %zu: entry %zu of the %s table differs from the fresh parse's", where, step,
                     index, kind ? "specimen" : "footnote");
                break;
            }
            TS_OK(kind ? markdown_core_specimen_properties(theirs, &label, &start)
                       : markdown_core_footnote_label(theirs, &label));
            if (label.has_value &&
                view_find(actual, kind ? markdown_core_document_specimen_for(document, label.value)
                                       : markdown_core_document_footnote_for(document, label.value)) !=
                    view_find(expected, kind ? markdown_core_document_specimen_for(fresh, label.value)
                                             : markdown_core_document_footnote_for(fresh, label.value))) {
                fail(state, "4.1", "%s step %zu: the %s lookup of entry %zu's label differs", where, step,
                     kind ? "specimen" : "footnote", index);
                break;
            }
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

/* 4.2 to 4.5 for one append from `before` to `after`, then the lineage
 * takes the step. */
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
     * which an append leaves at its offset; a node with no source has none.
     * One cursor per new owner walks the matched old owner's items, which
     * are in relation and source order. */
    for (index = 0; index < after->count && !stopped(state); index++) {
        const view_node *node = &after->nodes[index];
        size_t match = SIZE_MAX;
        if (!index) {
            match = 0;
        } else if (continues[node->owner] != SIZE_MAX) {
            const view_node *owner = &before->nodes[continues[node->owner]];
            size_t *at = &cursor[node->owner];
            while (*at < owner->item_count) {
                const view_node *old = &before->nodes[before->items[owner->items + *at]];
                const bool anchored = old->range.start < old->range.end;
                if (old->relation > node->relation ||
                    (old->relation == node->relation && anchored && old->range.start >= node->range.end)) {
                    break;
                }
                if (old->relation == node->relation && anchored && old->range.start >= node->range.start &&
                    match == SIZE_MAX && old->kind == node->kind) {
                    match = before->items[owner->items + *at];
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
        if (equal && before->nodes[old].object != node->object) {
            fail(state, "4.3", "%s step %zu: unchanged node %llu is a new object", where, step,
                 (unsigned long long)node->id);
        } else if (!equal && view_find(before, node->object) != SIZE_MAX) {
            fail(state, "4.3", "%s step %zu: changed node %llu is an object of the previous document", where, step,
                 (unsigned long long)node->id);
        }
    }
    if (stopped(state)) {
        goto done;
    }
    /* 4.5: the script's expectations, by position. */
    {
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

/* ------------------------------------------------------------------ steps */

/* Every oracle after the open (`before` NULL) or a step: 4.1, then, from the
 * view it takes of the document into `after`, 4.2 to 4.5 against the
 * previous view. */
static bool check_document(run *state, const char *where, size_t step, ih_unit unit, void *subject,
                           const markdown_core_document *document, const ih_text *model, history *ids,
                           const view *before, view *after, const ih_step *entry) {
    size_t length = 0, actual_length = 0, expected_length = 0, index;
    const uint8_t *text = state->subject->text(subject, &length);
    uint8_t *actual = NULL, *expected = NULL;
    markdown_core_document *fresh = NULL;
    view expected_view = {0};
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
    expected = dump_of(fresh, model->bytes, model->length, &expected_length);
    if (!same(actual, actual_length, expected, expected_length)) {
        fail(state, "4.1", "%s step %zu: the document differs from a fresh parse of the text", where, step);
        goto done;
    }
    if (!view_take(after, document, actual, actual_length) || !view_take(&expected_view, fresh, NULL, 0)) {
        fail(state, "harness", "%s step %zu: a document could not be viewed", where, step);
        goto done;
    }
    taken = true;
    for (index = 0; index < after->count && index < expected_view.count; index++) {
        if (after->nodes[index].extent.lead != expected_view.nodes[index].extent.lead ||
            after->nodes[index].extent.span != expected_view.nodes[index].extent.span) {
            break;
        }
    }
    if (after->count != expected_view.count || index < after->count) {
        fail(state, "4.1", "%s step %zu: node %zu's extent differs from the fresh parse's", where, step, index + 1);
        goto done;
    }
    check_queries(state, where, step, unit, document, after, fresh, &expected_view, expected, expected_length,
                  model->bytes, model->length);
    check_tables(state, where, step, document, after, fresh, &expected_view);
    if (stopped(state)) {
        goto done;
    }
    if (before) {
        check_identity(state, where, step, ids, before, after, entry);
    } else {
        history_open(state, where, ids, after);
    }
done:
    view_free(&expected_view);
    markdown_core_dump_free(actual);
    markdown_core_dump_free(expected);
    markdown_core_document_free(fresh);
    if (!taken) {
        view_free(after);
    }
    return taken;
}

/* ------------------------------------------------------------------ runs */

void run_appends(run *state, const char *where, ih_unit unit, const uint8_t *opened, size_t opened_length,
                 const ih_step *steps, size_t count) {
    ih_text model = {0};
    const markdown_core_document *current = NULL;
    history ids = {0};
    view previous = {0};
    void *subject = state->subject->open(unit, opened, opened_length, &current);
    size_t index;
    bool viewed;
    if (!subject || !ih_text_assign(&model, opened, opened_length)) {
        fail(state, "harness", "%s: the subject did not open", where);
        if (subject) {
            state->subject->close(subject);
        }
        ih_text_free(&model);
        return;
    }
    viewed = check_document(state, where, 0, unit, subject, current, &model, &ids, NULL, &previous, NULL);
    for (index = 0; viewed && index < count && !stopped(state); index++) {
        const ih_step *step = &steps[index];
        const markdown_core_document *next = NULL;
        view after;
        if (!state->subject->append(subject, step->text, step->length, &next) ||
            !ih_text_append(&model, step->text, step->length)) {
            fail(state, "harness", "%s step %zu: an append failed", where, index + 1);
            break;
        }
        current = next;
        viewed = check_document(state, where, index + 1, unit, subject, current, &model, &ids, &previous, &after, step);
        view_free(&previous);
        previous = after;
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
        sum += markdown_core_node_child_count(markdown_core_document_root(part));
        markdown_core_document_free(part);
        from += entry->parts[index];
    }
    if (from != length) {
        fail(state, "3.1", "%s: the parts do not cover the document", where);
        return;
    }
    whole = ts_ast_parse(document, length);
    if (!whole || markdown_core_node_child_count(markdown_core_document_root(whole)) != sum) {
        fail(state, "3.1", "%s: the composite's root children differ from its parts' sum %zu", where, sum);
    }
    markdown_core_document_free(whole);
}
