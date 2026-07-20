#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session_internal.h"

#include "directive.h"

#include <node.h>
#include <inlines.h>
#include <parser.h>
#include <references.h>

// Incremental commit pipeline (design step order in
// docs/migration/2026-07-15-v2-incremental-sessions-plan.md):
//
//   1. Restart plan  — the coalesced edit summary picks the restart point:
//                      the last CLEAN_START document child at or before the
//                      first edited byte (byte 0 when none). Children before
//                      it are untouched prefix; it and everything after it
//                      until reflow are the stale region.
//   2. Staged reparse — the stale bytes feed line by line through a fresh
//                      parser sharing the session's reference map. Once past
//                      the last edited byte, each clean line boundary that
//                      maps onto an old CLEAN_START child reflows: the old
//                      suffix survives wholesale with a one-line relative
//                      shift per child. Otherwise parsing continues to EOF
//                      (graceful degradation, never worse than a full parse).
//   3. Definition reconciliation — definitions harvested by the staged
//                      block phase are compared, in document order, against
//                      the definitions previously anchored in the stale
//                      region. An identical (label, url, title) sequence
//                      proves no lookup answer outside the region can
//                      change, so the old entries stay (keeping their
//                      full-parse document orders) and the duplicates are
//                      dropped after adoption. A differing sequence
//                      reconciles the map in place (winner-delta machinery
//                      above): the stale entries leave, the staged entries
//                      take their document-order span, and the labels whose
//                      winning payload changed name the dependent units —
//                      found through the session's per-unit lookup records —
//                      that must re-refine. Units that cannot be rebuilt
//                      per-unit (extension-owned) fall the commit back
//                      before anything is touched.
//   4. Inline phase  — runs on the staged region plus the rebuilt dependent
//                      units, with an unlimited expansion budget; the
//                      session-tracked expansion estimate proves a one-shot
//                      parse would not have hit its budget either, or the
//                      commit falls back. Lookups feed the recording that
//                      refreshes the session's lookup records at the end.
//   5. Adoption      — the stale children pair against the staged children
//                      through the standard adoption machine (a stack dummy
//                      document fronts the graveyard), so block- and
//                      inline-level id stability behave exactly like the
//                      full path. Rebuilt units adopt pairwise against the
//                      nodes they replace; ancestors of a changed rebuilt
//                      unit bubble a revision bump.
//   6. Transactional splice — every fallible step runs before the committed
//                      tree changes hands or is undone with pointer surgery
//                      only; after the footnote refresh succeeds, the
//                      remaining bookkeeping (id table, lookup records,
//                      clean index, geometry, graveyard release) cannot
//                      fail. Once the map has been reconciled, a failing
//                      commit leaves the session valid at its previous
//                      revision but marks the map stale, and the next
//                      commit takes the full path, which rebuilds it.

#ifndef MARKDOWN_CORE_SESSION_REF_BUDGET_FLOOR
#define MARKDOWN_CORE_SESSION_REF_BUDGET_FLOOR 100000
#endif

// Shared with session.c; sessions and documents share the error type but not
// a translation unit private to either.
void markdown_core_ast_set_error(markdown_core_error **error, markdown_core_error_code code, const char *message);

// --- line geometry -----------------------------------------------------------

/* Returns the offset one past the line's terminator (\n, \r, or \r\n), or
 * `length` for an unterminated final line. NUL bytes never end a line: the
 * feed machinery replaces them inline and keeps accumulating. */
static size_t line_end(const unsigned char *bytes, size_t length, size_t start) {
    size_t i = start;
    while (i < length) {
        if (bytes[i] == '\n') {
            return i + 1;
        }
        if (bytes[i] == '\r') {
            i++;
            if (i < length && bytes[i] == '\n') {
                i++;
            }
            return i;
        }
        i++;
    }
    return length;
}

static bool parser_is_clean(const markdown_core_parser *parser) {
    const markdown_core_node *last = parser->root->last_child;
    return !(last && (last->flags & MARKDOWN_CORE_NODE__OPEN));
}

/* Whether the line starting at `offset` seals a chain of open footnote
 * definitions: non-blank, with its first non-space before the continuation
 * indent (column 4, tabs advancing to the next stop). Blank and indented
 * lines keep such a chain open, so they can neither validate a sealing
 * restart anchor nor close a staged boundary. */
static bool line_seals(const unsigned char *bytes, size_t length, size_t offset) {
    int column = 0;
    size_t i;
    for (i = offset; i < length; i++) {
        unsigned char c = bytes[i];
        if (c == ' ') {
            column++;
        } else if (c == '\t') {
            column += 4 - (column % 4);
        } else {
            return c != '\n' && c != '\r';
        }
        if (column >= 4) {
            return false;
        }
    }
    return false;
}

/* The staged parser's open chain is nonempty and consists solely of footnote
 * definitions. Together with a sealing upcoming line this makes the position
 * a valid reflow boundary: a one-shot parse closes those definitions on that
 * very line, so finalizing them before the splice reproduces its tree, ends
 * dated to the line before the boundary either way. */
static bool parser_open_defs_only(const markdown_core_parser *parser) {
    const markdown_core_node *node = parser->root->last_child;
    if (!node || !(node->flags & MARKDOWN_CORE_NODE__OPEN)) {
        return false;
    }
    while (node && (node->flags & MARKDOWN_CORE_NODE__OPEN)) {
        if (node->type != MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION) {
            return false;
        }
        node = node->last_child;
    }
    return true;
}

typedef struct {
    size_t *items;
    size_t count;
    size_t capacity;
} offset_list;

static bool offset_push(markdown_core_mem *mem, offset_list *list, size_t offset) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 64;
        size_t *grown = (size_t *)mem->realloc(mem, list->items, capacity * sizeof(*grown));
        if (!grown) {
            return false;
        }
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count++] = offset;
    return true;
}

static int line_compare(const void *a, const void *b) {
    int la = *(const int *)a;
    int lb = *(const int *)b;
    return la < lb ? -1 : la > lb ? 1 : 0;
}

bool markdown_core_session_index_clean_children(
    markdown_core_session *session,
    markdown_core_node *root,
    const markdown_core_map *map,
    markdown_core_clean_index *out
) {
    const unsigned char *bytes = markdown_core_text_bytes(&session->text);
    size_t length = markdown_core_text_length(&session->text);
    const markdown_core_map_entry *entry;
    markdown_core_node *child;
    int *sentinel_lines = NULL;
    size_t sentinel_count = 0;
    size_t count = 0;
    size_t offset = 0;
    size_t i;
    int line = 1;

    for (child = root->first_child; child; child = child->next) {
        if (child->flags & MARKDOWN_CORE_NODE__CLEAN_START) {
            count++;
        }
    }
    // Head sentinels: vanished clean definition paragraphs leave no tree
    // node but remain safe restart and reflow points; one entry per
    // distinct line, and every one precedes the first real child.
    for (entry = map ? map->refs : NULL; entry; entry = entry->next) {
        if (entry->owner == 0 && entry->from_vanished_clean) {
            sentinel_count++;
        }
    }
    if (sentinel_count) {
        size_t filled = 0;
        size_t kept = 0;
        sentinel_lines = (int *)session->mem->calloc(session->mem, sentinel_count, sizeof(*sentinel_lines));
        if (!sentinel_lines) {
            return false;
        }
        for (entry = map->refs; entry; entry = entry->next) {
            if (entry->owner == 0 && entry->from_vanished_clean) {
                sentinel_lines[filled++] = entry->start_line;
            }
        }
        qsort(sentinel_lines, filled, sizeof(*sentinel_lines), line_compare);
        for (i = 0; i < filled; i++) {
            if (kept == 0 || sentinel_lines[kept - 1] != sentinel_lines[i]) {
                sentinel_lines[kept++] = sentinel_lines[i];
            }
        }
        sentinel_count = kept;
    }
    out->items = (markdown_core_clean_child *)session->mem
                     ->calloc(session->mem, count + sentinel_count ? count + sentinel_count : 1, sizeof(*out->items));
    if (!out->items) {
        if (sentinel_lines) {
            session->mem->free(session->mem, sentinel_lines);
        }
        return false;
    }
    out->capacity = count + sentinel_count ? count + sentinel_count : 1;
    out->count = 0;

    // Sentinels first (they precede every child), then clean children:
    // both carry strictly increasing line numbers, so one forward scan of
    // the text resolves every start byte.
    for (i = 0; i < sentinel_count; i++) {
        while (line < sentinel_lines[i] && offset < length) {
            offset = line_end(bytes, length, offset);
            line++;
        }
        out->items[out->count].start_byte = offset;
        out->items[out->count].start_line = sentinel_lines[i];
        out->items[out->count].node = NULL;
        out->count++;
    }
    if (sentinel_lines) {
        session->mem->free(session->mem, sentinel_lines);
    }
    for (child = root->first_child; child; child = child->next) {
        int start_line;
        if (!(child->flags & MARKDOWN_CORE_NODE__CLEAN_START)) {
            continue;
        }
        start_line = child->start_line + 1; // sealed relative to the document (absolute line 1)
        while (line < start_line && offset < length) {
            offset = line_end(bytes, length, offset);
            line++;
        }
        out->items[out->count].start_byte = offset;
        out->items[out->count].start_line = start_line;
        out->items[out->count].node = child;
        out->count++;
    }
    return true;
}

/* Index of the last clean child with start_byte <= old_lo, or -1. */
/* The real document child a clean entry stands for: a sentinel entry (a
 * vanished definition paragraph) resolves to the first child whose sealed
 * start reaches the sentinel's line — real children can precede it inside a
 * definition cluster (a paragraph that stopped vanishing), and those belong
 * to the prefix, not the stale region. */
static markdown_core_node *entry_node_at(const markdown_core_clean_child *entry, markdown_core_node *doc) {
    markdown_core_node *child;
    if (entry->node) {
        return entry->node;
    }
    for (child = doc->first_child; child && child->start_line + 1 < entry->start_line; child = child->next) {
    }
    return child;
}

static int def_index_compare(const void *a, const void *b) {
    const markdown_core_map_entry *ea = *(const markdown_core_map_entry *const *)a;
    const markdown_core_map_entry *eb = *(const markdown_core_map_entry *const *)b;
    if (ea->start_line != eb->start_line) {
        return ea->start_line < eb->start_line ? -1 : 1;
    }
    if (ea->order != eb->order) {
        return ea->order < eb->order ? -1 : 1;
    }
    return 0;
}

bool markdown_core_session_index_definitions(
    markdown_core_session *session,
    markdown_core_map *map,
    markdown_core_map_entry ***out_items,
    size_t *out_count
) {
    markdown_core_map_entry **items;
    markdown_core_map_entry *entry;
    size_t count = map ? map->size : 0;
    size_t filled = 0;

    items = (markdown_core_map_entry **)session->mem->calloc(session->mem, count ? count : 1, sizeof(*items));
    if (!items) {
        return false;
    }
    for (entry = map ? map->refs : NULL; entry; entry = entry->next) {
        items[filled++] = entry;
    }
    if (filled > 1) {
        qsort(items, filled, sizeof(*items), def_index_compare);
    }
    *out_items = items;
    *out_count = filled;
    return true;
}

/* First index whose entry starts at or beyond `line`. */
static size_t def_lower_bound(markdown_core_map_entry **items, size_t count, int line) {
    size_t lo = 0;
    size_t hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (items[mid]->start_line < line) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static ptrdiff_t restart_position(const markdown_core_clean_index *clean, size_t old_lo) {
    size_t lo = 0;
    size_t hi = clean->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (clean->items[mid].start_byte <= old_lo) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return (ptrdiff_t)lo - 1;
}

/* Index of the clean child starting exactly at old_byte after `after`, or
 * -1. */
static ptrdiff_t boundary_position(const markdown_core_clean_index *clean, size_t old_byte, ptrdiff_t after) {
    size_t lo = 0;
    size_t hi = clean->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (clean->items[mid].start_byte < old_byte) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < clean->count && clean->items[lo].start_byte == old_byte && (ptrdiff_t)lo > after) {
        return (ptrdiff_t)lo;
    }
    return -1;
}

// --- definition reconciliation ------------------------------------------------

typedef struct {
    markdown_core_reference **items;
    size_t count;
    size_t capacity;
} reference_list;

static bool reference_push(markdown_core_mem *mem, reference_list *list, markdown_core_reference *ref) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 16;
        markdown_core_reference **grown =
            (markdown_core_reference **)mem->realloc(mem, list->items, capacity * sizeof(*grown));
        if (!grown) {
            return false;
        }
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count++] = ref;
    return true;
}

/* Entries the staged block phase added: the head run of the live chain down
 * to (excluding) the pre-parse head, reversed into document order. */
static bool collect_new_definitions(
    markdown_core_mem *mem,
    markdown_core_map *map,
    const markdown_core_map_entry *previous_head,
    reference_list *out
) {
    markdown_core_map_entry *entry;
    size_t i;
    for (entry = map->refs; entry && entry != previous_head; entry = entry->next) {
        if (!reference_push(mem, out, (markdown_core_reference *)entry)) {
            return false;
        }
    }
    for (i = 0; i < out->count / 2; i++) {
        markdown_core_reference *swap = out->items[i];
        out->items[i] = out->items[out->count - 1 - i];
        out->items[out->count - 1 - i] = swap;
    }
    return true;
}

static int id_compare(const void *a, const void *b) {
    uint64_t ia = *(const uint64_t *)a;
    uint64_t ib = *(const uint64_t *)b;
    return ia < ib ? -1 : (ia > ib ? 1 : 0);
}

static bool id_set_holds(const uint64_t *ids, size_t count, uint64_t id) {
    size_t lo = 0;
    size_t hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ids[mid] == id) {
            return true;
        }
        if (ids[mid] < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return false;
}

/* Entries anchored in the stale region, in document order. At rest every
 * entry order stems from the most recent full parse, so sorting by order is
 * sorting by document position. */
/* A definition's stamped line lies inside its harvesting paragraph, and a
 * paragraph is reparsed exactly when its lines are: the stale entries are
 * one contiguous range of the session's line-ordered index. */
static bool collect_stale_definitions(
    markdown_core_mem *mem,
    markdown_core_session *session,
    int restart_line,
    int boundary_line,
    reference_list *out
) {
    size_t lo = def_lower_bound(session->def_index, session->def_count, restart_line);
    size_t hi = def_lower_bound(session->def_index, session->def_count, boundary_line);
    size_t i;
    for (i = lo; i < hi; i++) {
        if (!reference_push(mem, out, (markdown_core_reference *)session->def_index[i])) {
            return false;
        }
    }
    return true;
}

static bool chunks_equal(const markdown_core_chunk *a, const markdown_core_chunk *b) {
    return a->len == b->len && (a->len == 0 || memcmp(a->data, b->data, (size_t)a->len) == 0);
}

static bool definition_sequences_equal(const reference_list *old_defs, const reference_list *new_defs) {
    size_t i;
    if (old_defs->count != new_defs->count) {
        return false;
    }
    for (i = 0; i < old_defs->count; i++) {
        const markdown_core_reference *old_ref = old_defs->items[i];
        const markdown_core_reference *new_ref = new_defs->items[i];
        if (strcmp((const char *)old_ref->entry.label, (const char *)new_ref->entry.label) != 0 ||
            !chunks_equal(&old_ref->url, &new_ref->url) || !chunks_equal(&old_ref->title, &new_ref->title)) {
            return false;
        }
    }
    return true;
}

// --- winner-delta reconciliation ---------------------------------------------
//
// When the definition sequences differ, the map is reconciled in place
// instead of abandoning the commit: the stale entries leave, the staged
// entries take document orders in the vacated span (or the whole map
// renumbers when the span is too small), and each affected label's bucket is
// relinked to prefix -> staged -> suffix. Labels whose winning payload
// changed name the units to re-refine, found through the session's lookup
// records. Every allocation-bearing step runs before the first destructive
// map step; once surgery has run, a failing commit marks the map stale and
// the next commit takes the full path, which rebuilds the map wholesale.

typedef enum {
    DEF_REGION_PREFIX,
    DEF_REGION_STALE,
    DEF_REGION_SUFFIX,
} def_region;

/* Places an at-rest entry relative to the stale region by its stamped line:
 * the line lies inside the harvesting paragraph, whose position against the
 * restart and boundary lines settles the region exactly. */
static def_region classify_definition(const markdown_core_map_entry *entry, int restart_line, int boundary_line) {
    if (entry->start_line < restart_line) {
        return DEF_REGION_PREFIX;
    }
    return entry->start_line < boundary_line ? DEF_REGION_STALE : DEF_REGION_SUFFIX;
}

/* One affected label's bucket, partitioned into its region runs. The bucket
 * is ascending in document order, and staged entries carry the largest
 * orders, so the runs are contiguous: prefix, stale, suffix, staged. */
typedef struct {
    markdown_core_map_entry *prefix_head, *prefix_tail;
    markdown_core_map_entry *staged_head, *staged_tail;
    markdown_core_map_entry *suffix_head, *suffix_tail;
} label_plan;

typedef struct {
    markdown_core_key_index affected; // label -> label_plan
    label_plan *plans;
    size_t plan_count;
    markdown_core_key_index dirty; // labels whose winning payload changed
    size_t dirty_count;
    markdown_core_reference **prefix_entries; // surviving entries by document order
    size_t prefix_count;
    markdown_core_reference **suffix_entries;
    size_t suffix_count;
    uint64_t prefix_max_order;
    uint64_t suffix_min_order; // UINT64_MAX when the suffix holds none
    size_t splice_lo;          // stale range of the session's definition index
    size_t splice_hi;
    bool renumber;
    bool prepared;
    bool applied; // surgery ran: a failing commit leaves the map stale
} reconcile_state;

static void reconcile_release(markdown_core_mem *mem, reconcile_state *state) {
    if (!state->prepared) {
        return;
    }
    markdown_core_key_index_free(&state->affected);
    markdown_core_key_index_free(&state->dirty);
    if (state->plans) {
        mem->free(mem, state->plans);
    }
    if (state->prefix_entries) {
        mem->free(mem, state->prefix_entries);
    }
    if (state->suffix_entries) {
        mem->free(mem, state->suffix_entries);
    }
    memset(state, 0, sizeof(*state));
}

static bool reference_payloads_equal(const markdown_core_map_entry *a, const markdown_core_map_entry *b) {
    if (!a || !b) {
        return a == b;
    }
    return chunks_equal(&((const markdown_core_reference *)a)->url, &((const markdown_core_reference *)b)->url) &&
           chunks_equal(&((const markdown_core_reference *)a)->title, &((const markdown_core_reference *)b)->title);
}

/* Builds the whole reconciliation plan without touching the map. Returns
 * false with *fallback set when only a full reparse can settle the commit
 * (index build failure, an anchor that defies classification), and false
 * with *fallback clear on allocation loss. */
static bool reconcile_prepare(
    markdown_core_session *session,
    markdown_core_map *map,
    uint64_t order_floor,
    int restart_line,
    int boundary_line,
    const reference_list *old_defs,
    const reference_list *new_defs,
    reconcile_state *state,
    bool *fallback
) {
    markdown_core_mem *mem = session->mem;
    size_t affected_upper = old_defs->count + new_defs->count;
    size_t i;

    *fallback = false;
    memset(state, 0, sizeof(*state));
    state->suffix_min_order = UINT64_MAX;

    // Bucket surgery needs the hash-indexed shape; the sorted fallback array
    // cannot absorb a mid-chain splice.
    if (!markdown_core_map_ensure_index(map)) {
        *fallback = true;
        return false;
    }

    if (!markdown_core_key_index_init(&state->affected, mem, affected_upper) ||
        !markdown_core_key_index_init(&state->dirty, mem, affected_upper)) {
        markdown_core_key_index_free(&state->affected);
        return false;
    }
    state->prepared = true;
    state->plans = (label_plan *)mem->calloc(mem, affected_upper ? affected_upper : 1, sizeof(*state->plans));
    if (!state->plans) {
        reconcile_release(mem, state);
        return false;
    }

    // The stale entries form one line range of the session's definition
    // index; its neighbors bound the vacated order span. More staged
    // entries than the span holds means the whole map renumbers (rare,
    // O(definitions)) — only then are the surviving slices copied.
    state->splice_lo = def_lower_bound(session->def_index, session->def_count, restart_line);
    state->splice_hi = def_lower_bound(session->def_index, session->def_count, boundary_line);
    state->prefix_max_order = state->splice_lo ? session->def_index[state->splice_lo - 1]->order : 0;
    state->suffix_min_order =
        state->splice_hi < session->def_count ? session->def_index[state->splice_hi]->order : UINT64_MAX;
    {
        uint64_t span =
            state->suffix_min_order == UINT64_MAX ? UINT64_MAX : state->suffix_min_order - state->prefix_max_order - 1;
        state->renumber = span < new_defs->count;
    }
    if (state->renumber) {
        size_t prefix = state->splice_lo;
        size_t suffix = session->def_count - state->splice_hi;
        state->prefix_entries =
            (markdown_core_reference **)mem->calloc(mem, prefix ? prefix : 1, sizeof(*state->prefix_entries));
        state->suffix_entries =
            (markdown_core_reference **)mem->calloc(mem, suffix ? suffix : 1, sizeof(*state->suffix_entries));
        if (!state->prefix_entries || !state->suffix_entries) {
            reconcile_release(mem, state);
            return false;
        }
        memcpy(state->prefix_entries, session->def_index, prefix * sizeof(*state->prefix_entries));
        memcpy(state->suffix_entries, session->def_index + state->splice_hi, suffix * sizeof(*state->suffix_entries));
        state->prefix_count = prefix;
        state->suffix_count = suffix;
    }

    // Reserve the definition-index splice room while failing is still free.
    {
        size_t needed = session->def_count - (state->splice_hi - state->splice_lo) + new_defs->count;
        if (needed > session->def_capacity) {
            markdown_core_map_entry **grown =
                (markdown_core_map_entry **)
                    mem->realloc(mem, session->def_index, (needed ? needed : 1) * sizeof(*grown));
            if (!grown) {
                reconcile_release(mem, state);
                return false;
            }
            session->def_index = grown;
            session->def_capacity = needed ? needed : 1;
        }
    }

    // Partition each affected label's bucket and elect its winners: the old
    // winner is the head when it predates this parse, the new winner is the
    // first surviving run in prefix -> staged -> suffix order.
    for (i = 0; i < old_defs->count + new_defs->count; i++) {
        const markdown_core_map_entry *seed =
            i < old_defs->count ? &old_defs->items[i]->entry : &new_defs->items[i - old_defs->count]->entry;
        bufsize_t label_len = (bufsize_t)strlen((const char *)seed->label);
        void *existing = NULL;
        label_plan *plan = &state->plans[state->plan_count];
        markdown_core_map_entry *cursor;
        const markdown_core_map_entry *old_winner;
        const markdown_core_map_entry *new_winner;

        if (!markdown_core_key_index_insert(&state->affected, seed->label, label_len, plan, 0, &existing)) {
            reconcile_release(mem, state);
            return false;
        }
        if (existing) {
            continue; // label already planned
        }
        state->plan_count++;

        cursor = (markdown_core_map_entry *)markdown_core_key_index_lookup(&map->index, seed->label, label_len);
        old_winner = cursor && cursor->order < order_floor ? cursor : NULL;
        for (; cursor; cursor = cursor->bucket_next) {
            markdown_core_map_entry **head;
            markdown_core_map_entry **tail;
            if (cursor->order >= order_floor) {
                head = &plan->staged_head;
                tail = &plan->staged_tail;
            } else {
                switch (classify_definition(cursor, restart_line, boundary_line)) {
                case DEF_REGION_PREFIX:
                    head = &plan->prefix_head;
                    tail = &plan->prefix_tail;
                    break;
                case DEF_REGION_SUFFIX:
                    head = &plan->suffix_head;
                    tail = &plan->suffix_tail;
                    break;
                case DEF_REGION_STALE:
                    head = NULL;
                    tail = NULL;
                    break;
                default:
                    reconcile_release(mem, state);
                    *fallback = true;
                    return false;
                }
            }
            if (head) {
                if (!*head) {
                    *head = cursor;
                }
                *tail = cursor;
            }
        }

        new_winner =
            plan->prefix_head ? plan->prefix_head : (plan->staged_head ? plan->staged_head : plan->suffix_head);
        if (!reference_payloads_equal(old_winner, new_winner)) {
            if (!markdown_core_key_index_insert(&state->dirty, seed->label, label_len, plan, 0, NULL)) {
                reconcile_release(mem, state);
                return false;
            }
            state->dirty_count++;
        }
    }
    return true;
}

static int reference_order_compare(const void *a, const void *b) {
    const markdown_core_reference *ra = *(const markdown_core_reference *const *)a;
    const markdown_core_reference *rb = *(const markdown_core_reference *const *)b;
    return ra->entry.order < rb->entry.order ? -1 : (ra->entry.order > rb->entry.order ? 1 : 0);
}

/* The destructive half: frees the stale entries out of the live chain,
 * assigns the staged orders (spreading into the vacated span, or renumbering
 * everything in document order), and relinks every affected bucket. Nothing
 * here can fail; from the first unlink onward the map only converges. */
static void reconcile_apply(
    markdown_core_session *session,
    markdown_core_map *map,
    const reference_list *new_defs,
    reconcile_state *state
) {
    size_t i;

    state->applied = true;

    // Orders: spread into the vacated span, or renumber the whole map. The
    // relative order of any two surviving entries never changes, so buckets
    // outside the affected labels stay valid as they are.
    if (!state->renumber) {
        for (i = 0; i < new_defs->count; i++) {
            new_defs->items[i]->entry.order = state->prefix_max_order + 1 + i;
        }
    } else {
        uint64_t next = 0;
        if (state->prefix_count > 1) {
            qsort(state->prefix_entries, state->prefix_count, sizeof(*state->prefix_entries), reference_order_compare);
        }
        if (state->suffix_count > 1) {
            qsort(state->suffix_entries, state->suffix_count, sizeof(*state->suffix_entries), reference_order_compare);
        }
        for (i = 0; i < state->prefix_count; i++) {
            state->prefix_entries[i]->entry.order = ++next;
        }
        for (i = 0; i < new_defs->count; i++) {
            new_defs->items[i]->entry.order = ++next;
        }
        for (i = 0; i < state->suffix_count; i++) {
            state->suffix_entries[i]->entry.order = ++next;
        }
        map->next_order = next;
    }

    // Affected buckets relink to prefix -> staged -> suffix; the index slot
    // follows the new head (replacing an existing slot never allocates), or
    // leaves when the label vanished entirely.
    for (i = 0; i < state->plan_count; i++) {
        label_plan *plan = &state->plans[i];
        markdown_core_map_entry *head =
            plan->prefix_head ? plan->prefix_head : (plan->staged_head ? plan->staged_head : plan->suffix_head);
        markdown_core_map_entry *label_owner = head ? head : NULL;
        const unsigned char *label;
        bufsize_t label_len;

        if (plan->prefix_tail) {
            plan->prefix_tail->bucket_next = plan->staged_head ? plan->staged_head : plan->suffix_head;
        }
        if (plan->staged_tail) {
            plan->staged_tail->bucket_next = plan->suffix_head;
        }
        if (plan->suffix_tail) {
            plan->suffix_tail->bucket_next = NULL;
        }

        label = label_owner ? label_owner->label : NULL;
        if (head) {
            label_len = (bufsize_t)strlen((const char *)label);
            markdown_core_key_index_insert(&map->index, label, label_len, head, 1, NULL);
        } else {
            // With no surviving entry the index slot (still keyed by the
            // soon-to-be-freed old head's bytes) must leave; the affected
            // set stored the same label under a still-live seed.
            markdown_core_key_index_slot *slot = NULL;
            size_t s;
            for (s = 0; s < state->affected.capacity; s++) {
                if (state->affected.slots[s].key && state->affected.slots[s].value == plan) {
                    slot = &state->affected.slots[s];
                    break;
                }
            }
            if (slot) {
                markdown_core_key_index_remove(&map->index, slot->key, slot->key_len);
            }
        }
    }

    // Live-chain removal of the stale entries: last, because the index and
    // bucket surgery above still reads label bytes that stale entries own
    // (old winners key their index slots until the relink repoints them).
    // The stale set is exactly the collected index range; back links make
    // each unlink O(1).
    for (i = state->splice_lo; i < state->splice_hi; i++) {
        markdown_core_map_entry *entry = session->def_index[i];
        if (entry->prev) {
            entry->prev->next = entry->next;
        } else {
            map->refs = entry->next;
        }
        if (entry->next) {
            entry->next->prev = entry->prev;
        }
        map->size--;
        map->free(map, entry);
    }

    // Definition-index splice: the staged entries take the stale range's
    // place (room reserved during prepare); the array stays line-ordered
    // because staged lines lie strictly between the surviving neighbors.
    {
        size_t staged = new_defs->count;
        size_t tail = session->def_count - state->splice_hi;
        memmove(
            session->def_index + state->splice_lo + staged,
            session->def_index + state->splice_hi,
            tail * sizeof(*session->def_index)
        );
        for (i = 0; i < staged; i++) {
            session->def_index[state->splice_lo + i] = &new_defs->items[i]->entry;
        }
        session->def_count = state->splice_lo + staged + tail;
    }
}

// --- subtree walks ------------------------------------------------------------

static bool is_wrapper_node(const markdown_core_node *node) { return node->type == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL; }

/* `stop` bounds the walk once the chain has been spliced into a longer
 * sibling list (the first suffix node, or NULL for an isolated chain). */
static void ids_put_chain(markdown_core_session *session, markdown_core_node *chain, const markdown_core_node *stop) {
    markdown_core_node *top;
    for (top = chain; top && top != stop; top = top->next) {
        markdown_core_node *node = top;
        for (;;) {
            if (!is_wrapper_node(node)) {
                markdown_core_session_ids_put(session, node->id, node);
            }
            if (node->first_child) {
                node = node->first_child;
                continue;
            }
            while (node != top && !node->next) {
                node = node->parent;
            }
            if (node == top) {
                break;
            }
            node = node->next;
        }
    }
}

/* Drops ids the table still points at graveyard nodes for; adopted ids were
 * repointed at their staged counterparts beforehand and stay. */
static void ids_remove_stale_chain(markdown_core_session *session, markdown_core_node *chain) {
    markdown_core_node *top;
    for (top = chain; top; top = top->next) {
        markdown_core_node *node = top;
        for (;;) {
            if (!is_wrapper_node(node) && markdown_core_session_node_by_id(session, node->id) == node) {
                markdown_core_session_ids_remove(session, node->id);
            }
            if (node->first_child) {
                node = node->first_child;
                continue;
            }
            while (node != top && !node->next) {
                node = node->parent;
            }
            if (node == top) {
                break;
            }
            node = node->next;
        }
    }
}

/* Frees an isolated sibling chain. Parents are cleared first so the unlink
 * inside markdown_core_node_free can never touch the committed tree. */
static void free_child_chain(markdown_core_node *chain) {
    while (chain) {
        markdown_core_node *next = chain->next;
        chain->parent = NULL;
        chain->prev = NULL;
        chain->next = NULL;
        markdown_core_node_free(chain);
        chain = next;
    }
}

/* Drops every subtree id of the sibling chain from the session's lookup
 * records (missing ids are no-ops; only units ever hold records). Ids that a
 * staged unit adopted get their fresh records re-installed afterwards. */
static void lookups_remove_chain(markdown_core_session *session, markdown_core_node *chain) {
    markdown_core_node *top;
    for (top = chain; top; top = top->next) {
        markdown_core_node *node = top;
        for (;;) {
            if (!is_wrapper_node(node)) {
                markdown_core_lookup_table_remove(session->mem, &session->lookups, node->id);
            }
            if (node->first_child) {
                node = node->first_child;
                continue;
            }
            while (node != top && !node->next) {
                node = node->parent;
            }
            if (node == top) {
                break;
            }
            node = node->next;
        }
    }
}

// --- dependent units ----------------------------------------------------------

// One unit outside the stale region whose lookup answers changed: the
// committed node it replaces, the freshly refined rebuild, and the adoption
// verdict that drives ancestor revision bubbling.
typedef struct {
    markdown_core_node *unit;   // committed node, owned by the tree until the splice
    markdown_core_node *staged; // rebuilt unit, owned here until the splice
    bool changed;
} dependent_unit;

// A committed ancestor collected for a revision bubble. The revision stamp
// lands before the footnote diff (so its ancestor climb sees the node as
// already classified), while the delta id is recorded after the point of
// no return; `previous_rev` restores the stamp when the diff fails.
typedef struct {
    markdown_core_node *node;
    uint64_t previous_rev;
} bubble_ancestor;

/* Rebuilds the block shell of a committed unit for a fresh inline parse:
 * same raw kind, a copy of the block's string content, and absolute
 * positions (the parse writes absolute inline positions exactly like the
 * one-shot path; sealing relativizes them again). Only paragraphs and
 * headings qualify as rebuildable — anything else falls the commit back. */
static markdown_core_node *clone_unit_shell(markdown_core_session *session, markdown_core_node *unit) {
    markdown_core_scope scope = markdown_core_node_scope(unit);
    markdown_core_node *clone = markdown_core_node_new_with_mem((markdown_core_node_type)unit->type, session->mem);
    if (!clone) {
        return NULL;
    }
    markdown_core_strbuf_put(&clone->content, unit->content.ptr, unit->content.size);
    if (clone->content.oom) {
        markdown_core_node_free(clone);
        return NULL;
    }
    clone->start_line = scope.start.line;
    clone->start_column = unit->start_column;
    clone->end_line = scope.end.line;
    clone->end_column = unit->end_column;
    clone->internal_offset = unit->internal_offset;
    // Both restart-anchor bits: a sealing anchor rebuilt as a dependent must
    // keep its shape-conditional qualifier, or restart planning would trust
    // the anchor after an edit reshapes its line into a continuation.
    clone->flags |= unit->flags & MARKDOWN_CORE_NODE__CLEAN_ANCHOR;
    if (unit->type == MARKDOWN_CORE_NODE_HEADING) {
        clone->as.heading = unit->as.heading;
    }
    return clone;
}

static int candidate_id_compare(const void *a, const void *b) {
    markdown_core_node_id x = *(const markdown_core_node_id *)a;
    markdown_core_node_id y = *(const markdown_core_node_id *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Collects the units that depend on a label whose winner changed by walking
 * the changed labels' postings — O(affected units), not O(units with
 * lookups) — skipping units the staged reparse rebuilds anyway. Returns
 * false with *fallback set when a dependent cannot be rebuilt per-unit (an
 * extension-owned unit, or a record that no longer matches the tree), and
 * false with *fallback clear on allocation loss. */
static bool collect_dependents(
    markdown_core_session *session,
    const markdown_core_key_index *dirty,
    const uint64_t *stale_ids,
    size_t stale_count,
    dependent_unit **out,
    size_t *out_count,
    bool *fallback
) {
    markdown_core_mem *mem = session->mem;
    markdown_core_lookup_table *table = &session->lookups;
    markdown_core_node *doc = session->view.root;
    dependent_unit *dependents = NULL;
    size_t count = 0;
    size_t capacity = 0;
    markdown_core_node_id *candidates = NULL;
    size_t candidate_count = 0;
    size_t candidate_capacity = 0;
    size_t slot;
    size_t c;

    *out = NULL;
    *out_count = 0;
    *fallback = false;
    if (dirty->size == 0) {
        return true;
    }
    // Union of the dirty labels' postings; the same unit may sit under
    // several dirty labels, so sort and unique before the per-unit checks.
    for (slot = 0; slot < dirty->capacity; slot++) {
        const markdown_core_lookup_posting *posting;
        size_t i;
        if (!dirty->slots[slot].key) {
            continue;
        }
        posting = markdown_core_lookup_postings_find(table, dirty->slots[slot].key);
        if (!posting) {
            continue;
        }
        for (i = 0; i < posting->count; i++) {
            if (candidate_count == candidate_capacity) {
                size_t grown_capacity = candidate_capacity ? candidate_capacity * 2 : 16;
                markdown_core_node_id *grown =
                    (markdown_core_node_id *)mem->realloc(mem, candidates, grown_capacity * sizeof(*grown));
                if (!grown) {
                    if (candidates) {
                        mem->free(mem, candidates);
                    }
                    return false;
                }
                candidates = grown;
                candidate_capacity = grown_capacity;
            }
            candidates[candidate_count++] = posting->items[i].unit;
        }
    }
    if (candidate_count > 1) {
        qsort(candidates, candidate_count, sizeof(*candidates), candidate_id_compare);
    }
    for (c = 0; c < candidate_count; c++) {
        markdown_core_node *unit;
        markdown_core_node *top;

        if (c > 0 && candidates[c] == candidates[c - 1]) {
            continue;
        }
        unit = (markdown_core_node *)markdown_core_session_node_by_id(session, candidates[c]);
        if (!unit) {
            goto fall_back;
        }
        for (top = unit; top->parent && top->parent != doc; top = top->parent) {
        }
        if (top->parent != doc) {
            goto fall_back;
        }
        if (id_set_holds(stale_ids, stale_count, top->id)) {
            continue; // the staged reparse rebuilds this unit anyway
        }
        if ((unit->type != MARKDOWN_CORE_NODE_PARAGRAPH && unit->type != MARKDOWN_CORE_NODE_HEADING) ||
            unit->extension || !markdown_core_node_owns_inlines(unit)) {
            goto fall_back;
        }
        if (count == capacity) {
            size_t grown_capacity = capacity ? capacity * 2 : 8;
            dependent_unit *grown = (dependent_unit *)mem->realloc(mem, dependents, grown_capacity * sizeof(*grown));
            if (!grown) {
                if (dependents) {
                    mem->free(mem, dependents);
                }
                if (candidates) {
                    mem->free(mem, candidates);
                }
                return false;
            }
            dependents = grown;
            capacity = grown_capacity;
        }
        dependents[count].unit = unit;
        dependents[count].staged = NULL;
        dependents[count].changed = false;
        count++;
    }
    if (candidates) {
        mem->free(mem, candidates);
    }
    *out = dependents;
    *out_count = count;
    return true;

fall_back:
    if (dependents) {
        mem->free(mem, dependents);
    }
    if (candidates) {
        mem->free(mem, candidates);
    }
    *fallback = true;
    return false;
}

// --- footnote sites -----------------------------------------------------------
//
// With footnotes enabled, an incremental commit rebuilds the session's
// footnote index from site lists merged in document order rather than from a
// whole-tree walk: the graveyard's sites leave (the staged region's freshly
// collected sites take their place), a rebuilt unit's sites are replaced by
// its clone's, and every other site survives with its anchor classifying it
// against the restart line. The merge runs before adoption, while falling
// back is still free, and costs O(previous sites + staged material).

static bool site_run_append(
    markdown_core_mem *mem,
    markdown_core_footnote_site_list *out,
    const markdown_core_footnote_site_list *run
) {
    size_t i;
    for (i = 0; i < run->count; i++) {
        if (!markdown_core_footnote_site_push(mem, out, run->items[i])) {
            return false;
        }
    }
    return true;
}

// A rebuilt unit keyed for the pointer bsearch that spots its sites in the
// previous list; `index` follows the dependents array.
typedef struct {
    const markdown_core_node *unit;
    size_t index;
} clone_key;

static int clone_key_compare(const void *a, const void *b) {
    uintptr_t ua = (uintptr_t)((const clone_key *)a)->unit;
    uintptr_t ub = (uintptr_t)((const clone_key *)b)->unit;
    return ua < ub ? -1 : (ua > ub ? 1 : 0);
}

static size_t clone_key_find(const clone_key *keys, size_t count, const markdown_core_node *unit) {
    size_t lo = 0;
    size_t hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (keys[mid].unit == unit) {
            return keys[mid].index;
        }
        if ((uintptr_t)keys[mid].unit < (uintptr_t)unit) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return SIZE_MAX;
}

/* Merges one document-ordered site list into `out`. Emission follows the old
 * list's own order: the staged run enters at the first graveyard site or
 * ahead of the first surviving suffix site (at the end when neither exists),
 * and a clone's run enters at its unit's first site — after the staged run
 * when that site is the first suffix one, since the staged region precedes
 * every surviving suffix child. Returns FALLBACK when an anchor no longer
 * parents to the document (the lists no longer describe the tree), FAILED on
 * allocation loss. */
static markdown_core_incremental_result merge_site_list(
    markdown_core_session *session,
    const markdown_core_footnote_site_list *old_sites,
    const uint64_t *stale_ids,
    size_t stale_count,
    int restart_line,
    const markdown_core_footnote_site_list *staged_run,
    const clone_key *clone_keys,
    markdown_core_footnote_site_list *clone_runs,
    bool *clone_emitted,
    size_t clone_count,
    markdown_core_footnote_site_list *out
) {
    markdown_core_mem *mem = session->mem;
    markdown_core_node *doc = session->view.root;
    bool staged_emitted = false;
    size_t i;

    for (i = 0; i < old_sites->count; i++) {
        const markdown_core_footnote_site *site = &old_sites->items[i];
        size_t found;
        if (id_set_holds(stale_ids, stale_count, site->anchor->id)) {
            // Graveyard: the staged run takes this run's place. A rebuilt
            // unit is never anchored here (collect_dependents skips the
            // stale region), so no clone test applies.
            if (!staged_emitted) {
                if (!site_run_append(mem, out, staged_run)) {
                    return MARKDOWN_CORE_INCREMENTAL_FAILED;
                }
                staged_emitted = true;
            }
            continue;
        }
        found = site->unit && clone_count ? clone_key_find(clone_keys, clone_count, site->unit) : SIZE_MAX;
        // A top-level rebuilt unit was already swapped out of the tree (the
        // merge runs post-adoption), so its sites anchor at an unlinked
        // node; the clone match below replaces them, and their stored line
        // still orders the staged emission correctly.
        if (found == SIZE_MAX && site->anchor->parent != doc) {
            return MARKDOWN_CORE_INCREMENTAL_FALLBACK;
        }
        if (!staged_emitted && site->anchor->start_line + 1 >= restart_line) {
            if (!site_run_append(mem, out, staged_run)) {
                return MARKDOWN_CORE_INCREMENTAL_FAILED;
            }
            staged_emitted = true;
        }
        if (found != SIZE_MAX) {
            if (!clone_emitted[found]) {
                if (!site_run_append(mem, out, &clone_runs[found])) {
                    return MARKDOWN_CORE_INCREMENTAL_FAILED;
                }
                clone_emitted[found] = true;
            }
            continue;
        }
        if (!markdown_core_footnote_site_push(mem, out, *site)) {
            return MARKDOWN_CORE_INCREMENTAL_FAILED;
        }
    }
    if (!staged_emitted && !site_run_append(mem, out, staged_run)) {
        return MARKDOWN_CORE_INCREMENTAL_FAILED;
    }
    return MARKDOWN_CORE_INCREMENTAL_COMMITTED;
}

/* An anchor's absolute first line. Stale and graveyard anchors keep their
 * pre-commit lines — exactly the coordinate space of restart_line and
 * boundary_line — and the suffix shift runs after the refresh, so every
 * comparison below happens in old coordinates. */
static int site_anchor_line(const markdown_core_footnote_site *site) { return site->anchor->start_line + 1; }

/* First site whose anchor starts at or beyond `line`. */
static size_t site_lower_bound(const markdown_core_footnote_site_list *list, int line) {
    size_t lo = 0;
    size_t hi = list->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (site_anchor_line(&list->items[mid]) < line) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* One churned reference id staged for reinsertion: its record already
 * repointed at the replacement node, plus the group position that repoints
 * the label's group entry. */
typedef struct {
    markdown_core_footnote_record record;
    size_t group_pos;
} churn_stash;

/* Fast-path apply, pass 1: tombstone every churned id in the run, stashing
 * its record for reinsertion. Adoption can swap ids between positions of
 * one commit, so every removal must land before any reinsert. */
static size_t stash_ref_churn(
    markdown_core_footnote_index *fn,
    const markdown_core_footnote_site *old_sites,
    const markdown_core_footnote_site *new_sites,
    size_t count,
    churn_stash *stash,
    size_t filled
) {
    size_t i;
    for (i = 0; i < count; i++) {
        if (old_sites[i].node->id != new_sites[i].node->id) {
            markdown_core_footnote_record *record =
                markdown_core_footnote_table_find(&fn->records, old_sites[i].node->id);
            stash[filled].record = *record;
            stash[filled].record.node = new_sites[i].node;
            stash[filled].group_pos = old_sites[i].group_pos;
            markdown_core_footnote_table_remove(&fn->records, old_sites[i].node->id);
            filled++;
        }
    }
    return filled;
}

/* Fast-path apply, pass 2: refresh the run's pointers in place; the label
 * (equal by the fast-path check) and group position survive. Unchurned ids
 * repoint their records here; churned ones reinsert from the stash. */
static void patch_ref_run(
    markdown_core_footnote_index *fn,
    markdown_core_footnote_site *old_sites,
    const markdown_core_footnote_site *new_sites,
    size_t count
) {
    size_t i;
    for (i = 0; i < count; i++) {
        markdown_core_footnote_site *old_site = &old_sites[i];
        const markdown_core_footnote_site *new_site = &new_sites[i];
        if (old_site->node->id == new_site->node->id) {
            markdown_core_footnote_table_find(&fn->records, old_site->node->id)->node = new_site->node;
        }
        old_site->node = new_site->node;
        old_site->anchor = new_site->anchor;
        old_site->unit = new_site->unit;
    }
}

/* One rebuilt unit's current reference run inside the persistent list, and
 * the anchor its surviving sites carry (the old unit itself for a top-level
 * rebuild — the clone took its place in the tree — or the surviving
 * container for a nested one). */
typedef struct {
    size_t start;
    size_t count;
    markdown_core_node *anchor;
} unit_run;

/* Refreshes the session's footnote index for one incremental commit. Runs
 * post-adoption (ids final), before the point of no return. A commit whose
 * stale ranges and rebuilt units carry the same label sequences as their
 * replacements patches the persistent index in place — pointer and
 * churned-id updates only, no aggregate can have moved — at
 * O(staged + stale + rebuilt) after two O(log sites) range probes. Any
 * other commit merges the site lists in document order and rebuilds the
 * derived structures from label slots (*out_built set, caller swaps at the
 * point of no return), diffing per node id to bump exactly the answers
 * that changed. A clone that gains sites where its unit had none, or holds
 * a definition, falls back to the full path as before. */
static markdown_core_incremental_result footnote_refresh(
    markdown_core_session *session,
    markdown_core_footnote_site_list *staged_defs,
    markdown_core_footnote_site_list *staged_refs,
    const uint64_t *stale_ids,
    size_t stale_count,
    int restart_line,
    int boundary_line,
    dependent_unit *dependents,
    size_t dependent_count,
    uint64_t new_rev,
    markdown_core_delta *changes,
    markdown_core_footnote_index *out_index,
    bool *out_built
) {
    markdown_core_mem *mem = session->mem;
    markdown_core_node *doc = session->view.root;
    markdown_core_footnote_index *fn = &session->footnotes;
    markdown_core_footnote_site_list def_sites = {NULL, 0, 0};
    markdown_core_footnote_site_list ref_sites = {NULL, 0, 0};
    markdown_core_footnote_site_list *clone_runs = NULL;
    clone_key *clone_keys = NULL;
    bool *clone_emitted = NULL;
    unit_run *runs = NULL;
    markdown_core_incremental_result result = MARKDOWN_CORE_INCREMENTAL_FAILED;
    size_t d0 = site_lower_bound(&fn->defs, restart_line);
    size_t d1 = site_lower_bound(&fn->defs, boundary_line);
    size_t r0 = site_lower_bound(&fn->refs, restart_line);
    size_t r1 = site_lower_bound(&fn->refs, boundary_line);
    size_t churn = 0;
    bool fast;
    size_t i;

    if (dependent_count) {
        clone_runs = (markdown_core_footnote_site_list *)mem->calloc(mem, dependent_count, sizeof(*clone_runs));
        clone_keys = (clone_key *)mem->calloc(mem, dependent_count, sizeof(*clone_keys));
        clone_emitted = (bool *)mem->calloc(mem, dependent_count, sizeof(*clone_emitted));
        runs = (unit_run *)mem->calloc(mem, dependent_count, sizeof(*runs));
        if (!clone_runs || !clone_keys || !clone_emitted || !runs) {
            goto done;
        }
        for (i = 0; i < dependent_count; i++) {
            markdown_core_node *top = dependents[i].staged;
            markdown_core_footnote_site_list clone_defs = {NULL, 0, 0};
            markdown_core_footnote_site_list no_defs = {NULL, 0, 0};
            bool collected;
            size_t at;
            while (top->parent && top->parent != doc) {
                top = top->parent;
            }
            // A top-level unit anchors its clone's sites at the clone (the
            // splice put it where the unit was); a nested one at its
            // surviving container. The unit's surviving sites carry the old
            // unit / the same container respectively.
            runs[i].anchor = top == dependents[i].staged ? dependents[i].unit : top;
            collected = markdown_core_footnote_collect_sites(
                mem,
                dependents[i].staged,
                top == dependents[i].staged ? dependents[i].staged : top,
                &clone_defs,
                &clone_runs[i]
            );
            if (clone_defs.count) {
                // A rebuilt leaf can never hold a definition; one appearing
                // means the lists no longer describe the tree.
                markdown_core_footnote_site_list_release(mem, &clone_defs);
                result = MARKDOWN_CORE_INCREMENTAL_FALLBACK;
                goto done;
            }
            markdown_core_footnote_site_list_release(mem, &clone_defs);
            if (!collected || !markdown_core_session_footnote_label_sites(session, &no_defs, &clone_runs[i])) {
                goto done;
            }
            at = site_lower_bound(&fn->refs, runs[i].anchor->start_line + 1);
            while (at < fn->refs.count && fn->refs.items[at].anchor == runs[i].anchor &&
                   fn->refs.items[at].unit != dependents[i].unit) {
                at++;
            }
            runs[i].start = at;
            while (at < fn->refs.count && fn->refs.items[at].unit == dependents[i].unit) {
                at++;
            }
            runs[i].count = at - runs[i].start;
            if (runs[i].count == 0 && clone_runs[i].count) {
                // Sites gained where the unit had none have no position (the
                // link/footnote-reference kind flip).
                result = MARKDOWN_CORE_INCREMENTAL_FALLBACK;
                goto done;
            }
            clone_keys[i].unit = dependents[i].unit;
            clone_keys[i].index = i;
        }
        qsort(clone_keys, dependent_count, sizeof(*clone_keys), clone_key_compare);
    }

    // --- fast path: sequence-preserving commit ---
    fast = staged_defs->count == d1 - d0 && staged_refs->count == r1 - r0;
    for (i = 0; fast && i < staged_defs->count; i++) {
        const markdown_core_footnote_site *old_site = &fn->defs.items[d0 + i];
        const markdown_core_footnote_site *new_site = &staged_defs->items[i];
        // A churned definition id can be a label's winner, embedded in every
        // record of the label; the rebuild handles that cascade.
        if (old_site->label != new_site->label || old_site->node->id != new_site->node->id) {
            fast = false;
        }
    }
    for (i = 0; fast && i < staged_refs->count; i++) {
        const markdown_core_footnote_site *old_site = &fn->refs.items[r0 + i];
        const markdown_core_footnote_site *new_site = &staged_refs->items[i];
        if (old_site->label != new_site->label) {
            fast = false;
        } else if (old_site->node->id != new_site->node->id) {
            if (!markdown_core_footnote_table_find(&fn->records, old_site->node->id)) {
                fast = false;
            } else {
                churn++;
            }
        }
    }
    for (i = 0; fast && i < dependent_count; i++) {
        size_t k;
        if (runs[i].start + runs[i].count > r0 && runs[i].start < r1) {
            fast = false; // a rebuilt unit inside the stale range: not a position we patch
            break;
        }
        if (clone_runs[i].count != runs[i].count) {
            fast = false;
            break;
        }
        for (k = 0; fast && k < runs[i].count; k++) {
            const markdown_core_footnote_site *old_site = &fn->refs.items[runs[i].start + k];
            const markdown_core_footnote_site *new_site = &clone_runs[i].items[k];
            if (old_site->label != new_site->label) {
                fast = false;
            } else if (old_site->node->id != new_site->node->id) {
                if (!markdown_core_footnote_table_find(&fn->records, old_site->node->id)) {
                    fast = false;
                } else {
                    churn++;
                }
            }
        }
    }

    if (fast) {
        churn_stash *stash = NULL;
        size_t stashed = 0;
        if (churn) {
            stash = (churn_stash *)mem->calloc(mem, churn, sizeof(*stash));
            if (!stash || !markdown_core_footnote_table_reserve(mem, &fn->records, churn)) {
                if (stash) {
                    mem->free(mem, stash);
                }
                goto done;
            }
        }
        // Infallible from here: pointer and id patches only.
        for (i = 0; i < staged_defs->count; i++) {
            markdown_core_footnote_site *old_site = &fn->defs.items[d0 + i];
            const markdown_core_footnote_site *new_site = &staged_defs->items[i];
            markdown_core_footnote_table_find(&fn->records, new_site->node->id)->node = new_site->node;
            old_site->node = new_site->node;
            old_site->anchor = new_site->anchor;
        }
        // Empty runs skip the calls outright: offsetting a NULL items array
        // by even zero is undefined.
        if (staged_refs->count) {
            stashed = stash_ref_churn(fn, fn->refs.items + r0, staged_refs->items, staged_refs->count, stash, stashed);
        }
        for (i = 0; i < dependent_count; i++) {
            if (runs[i].count) {
                stashed = stash_ref_churn(
                    fn,
                    fn->refs.items + runs[i].start,
                    clone_runs[i].items,
                    runs[i].count,
                    stash,
                    stashed
                );
            }
        }
        if (staged_refs->count) {
            patch_ref_run(fn, fn->refs.items + r0, staged_refs->items, staged_refs->count);
        }
        for (i = 0; i < dependent_count; i++) {
            if (runs[i].count) {
                patch_ref_run(fn, fn->refs.items + runs[i].start, clone_runs[i].items, runs[i].count);
            }
        }
        for (i = 0; i < stashed; i++) {
            markdown_core_footnote_table_put(&fn->records, stash[i].record);
            if (stash[i].record.info.number) {
                size_t group = (size_t)(stash[i].record.info.number - 1);
                fn->references[fn->reference_offsets[group] + stash[i].group_pos] = stash[i].record.node->id;
            }
        }
        if (stash) {
            mem->free(mem, stash);
        }
        (void)new_rev;
        (void)changes;
        *out_built = false;
        result = MARKDOWN_CORE_INCREMENTAL_COMMITTED;
        goto done;
    }

    // --- slow path: merge in document order, rebuild, diff ---
    result = merge_site_list(
        session,
        &fn->defs,
        stale_ids,
        stale_count,
        restart_line,
        staged_defs,
        NULL,
        NULL,
        NULL,
        0,
        &def_sites
    );
    if (result == MARKDOWN_CORE_INCREMENTAL_COMMITTED) {
        result = merge_site_list(
            session,
            &fn->refs,
            stale_ids,
            stale_count,
            restart_line,
            staged_refs,
            clone_keys,
            clone_runs,
            clone_emitted,
            dependent_count,
            &ref_sites
        );
    }
    if (result == MARKDOWN_CORE_INCREMENTAL_COMMITTED) {
        result = MARKDOWN_CORE_INCREMENTAL_FAILED;
        if (markdown_core_footnote_index_build_sites(mem, &def_sites, &ref_sites, out_index)) {
            if (markdown_core_footnote_index_diff(mem, fn, out_index, new_rev, changes)) {
                *out_built = true;
                result = MARKDOWN_CORE_INCREMENTAL_COMMITTED;
            } else {
                markdown_core_footnote_index_release(mem, out_index);
            }
        }
    }

done:
    markdown_core_footnote_site_list_release(mem, &def_sites);
    markdown_core_footnote_site_list_release(mem, &ref_sites);
    if (clone_runs) {
        for (i = 0; i < dependent_count; i++) {
            markdown_core_footnote_site_list_release(mem, &clone_runs[i]);
        }
        mem->free(mem, clone_runs);
    }
    if (clone_keys) {
        mem->free(mem, clone_keys);
    }
    if (clone_emitted) {
        mem->free(mem, clone_emitted);
    }
    if (runs) {
        mem->free(mem, runs);
    }
    return result;
}

/* Swaps `staged` into `unit`'s place in the committed tree. Pure pointer
 * surgery, and its own inverse: swapping the arguments undoes it. */
static void splice_replace(markdown_core_node *unit, markdown_core_node *staged) {
    staged->parent = unit->parent;
    staged->prev = unit->prev;
    staged->next = unit->next;
    if (unit->prev) {
        unit->prev->next = staged;
    } else if (unit->parent) {
        unit->parent->first_child = staged;
    }
    if (unit->next) {
        unit->next->prev = staged;
    } else if (unit->parent) {
        unit->parent->last_child = staged;
    }
    unit->parent = NULL;
    unit->prev = NULL;
    unit->next = NULL;
}

// --- the pipeline --------------------------------------------------------------

markdown_core_incremental_result markdown_core_session_commit_incremental(
    markdown_core_session *session,
    uint64_t new_rev,
    markdown_core_delta *changes,
    markdown_core_error **error
) {
    markdown_core_mem *mem = session->mem;
    markdown_core_map *map = session->refmap;
    const unsigned char *bytes = markdown_core_text_bytes(&session->text);
    size_t length = markdown_core_text_length(&session->text);
    markdown_core_node *doc = session->view.root;
    const markdown_core_edit_summary pending = session->pending;
    size_t old_lo = pending.new_lo;
    size_t budget = length > MARKDOWN_CORE_SESSION_REF_BUDGET_FLOOR ? length : MARKDOWN_CORE_SESSION_REF_BUDGET_FLOOR;

    markdown_core_incremental_result result = MARKDOWN_CORE_INCREMENTAL_FAILED;
    markdown_core_parser *parser = NULL;
    markdown_core_map *own_map = NULL; // the staged parser's unused fresh map
    markdown_core_map_entry *previous_head = map->refs;
    uint64_t order_floor = map->next_order + 1; // entries this parse harvests sit at or above this
    markdown_core_node *root = NULL;            // refined staged tree, once detached
    offset_list line_offsets = {NULL, 0, 0};
    reference_list new_defs = {NULL, 0, 0};
    reference_list old_defs = {NULL, 0, 0};
    uint64_t *stale_ids = NULL;
    size_t stale_count = 0;
    markdown_core_footnote_index footnotes;
    markdown_core_footnote_site_list staged_defs = {NULL, 0, 0};
    markdown_core_footnote_site_list staged_refs = {NULL, 0, 0};
    bool footnotes_built = false;
    markdown_core_lookup_recording recording;
    markdown_core_unit_lookups *bundles = NULL;
    size_t bundle_count = 0;
    reconcile_state reconcile;
    bool defs_equal = true;
    dependent_unit *dependents = NULL;
    size_t dependent_count = 0;
    markdown_core_node *holder = NULL; // staging parent for dependent rebuilds
    size_t sealed_nodes = 0;           // filled by the seal walks, sizes the id reserve
    bubble_ancestor *bubble_nodes = NULL;
    size_t bubble_count = 0;
    size_t bubble_capacity = 0;
    uint64_t previous_doc_rev = 0;
    int *sentinel_lines = NULL; // vanished clean definition paragraphs staged by this commit
    size_t sentinel_count = 0;

    markdown_core_lookup_recording_init(&recording, mem);
    memset(&reconcile, 0, sizeof(reconcile));
    memset(&footnotes, 0, sizeof(footnotes));

    // --- 1. restart plan ---
    ptrdiff_t restart_pos = restart_position(&session->clean, old_lo);
    markdown_core_node *restart_node;
    size_t restart_byte;
    int restart_line;

    // An edit can place a '\n' exactly at the chosen boundary while the
    // untouched prefix ends with a lone '\r': the two fuse into one CRLF
    // terminator in the current text, and the boundary no longer starts a
    // line, so feeding from it would count a phantom line. One entry back
    // always restores the invariant: the earlier entry's surrounding bytes
    // lie strictly inside the untouched prefix, where old line starts
    // survive verbatim.
    if (restart_pos >= 0) {
        size_t byte = session->clean.items[restart_pos].start_byte;
        if (byte > 0 && byte < length && bytes[byte - 1] == '\r' && bytes[byte] == '\n') {
            restart_pos--;
        }
    }

    // A sealing anchor is valid only while its first line keeps the shape
    // that closes the footnote definitions open above it; an edit at the
    // restart line can reshape it into a blank or indented continuation
    // those definitions would capture. Sentinels do not record their sealing
    // quality, so they take the same check. Only the chosen entry's line can
    // be damaged — every earlier entry's line lies wholly inside the
    // untouched prefix — so one entry back always restores validity.
    if (restart_pos >= 0) {
        const markdown_core_clean_child *entry = &session->clean.items[restart_pos];
        if ((!entry->node || (entry->node->flags & MARKDOWN_CORE_NODE__CLEAN_START_SEALING)) &&
            !line_seals(bytes, length, entry->start_byte)) {
            restart_pos--;
        }
    }
    restart_node = restart_pos >= 0 ? entry_node_at(&session->clean.items[restart_pos], doc) : doc->first_child;
    restart_byte = restart_pos >= 0 ? session->clean.items[restart_pos].start_byte : 0;
    restart_line = restart_pos >= 0 ? session->clean.items[restart_pos].start_line : 1;

    ptrdiff_t boundary_pos = -1;
    int fed_lines = 0;
    int total_lines;
    int last_line_length;
    int staged_tail_length;

    // A restart at or beyond the end of the text feeds nothing, so the
    // parser's line scalars would not describe the surviving prefix (whose
    // final line length is a validated-bytes measure only a parse can give).
    // Rare — the entire tail was deleted at a clean boundary — and bounded:
    // fall back to the full reparse.
    if (restart_byte >= length) {
        return MARKDOWN_CORE_INCREMENTAL_FALLBACK;
    }

    // Routing: a restart at the document head with no clean boundary at or
    // beyond the damage reparses everything and no reflow can cut the
    // suffix. The full path runs the same parse with wholesale table and
    // index rebuilds instead of per-node splice maintenance (measured up to
    // 31% cheaper on whole-document shapes), and falling back here is free —
    // nothing has been staged yet.
    if (restart_byte == 0 &&
        (session->clean.count == 0 || session->clean.items[session->clean.count - 1].start_byte <
                                          (size_t)((ptrdiff_t)session->pending.new_hi - session->pending.delta))) {
        return MARKDOWN_CORE_INCREMENTAL_FALLBACK;
    }

    // --- 2. staged reparse with reflow probing ---
    parser = markdown_core_session_acquire_parser(session, error);
    if (!parser) {
        goto failed;
    }
    own_map = parser->refmap;
    parser->refmap = map;
    parser->line_number = restart_line - 1;

    // finalize() dates a block closed in the middle of a line to the end of
    // the line before it, so the staged parser must know the previous line's
    // terminator-stripped length. That line sits wholly inside the untouched
    // prefix (its bytes end at restart_byte, at or before the first edited
    // byte), so the current text still holds exactly what a one-shot parse
    // would have measured.
    if (restart_byte > 0) {
        size_t prev_end = restart_byte;
        size_t prev_start;
        if (bytes[prev_end - 1] == '\n') {
            prev_end--;
        }
        if (prev_end > 0 && bytes[prev_end - 1] == '\r') {
            prev_end--;
        }
        prev_start = prev_end;
        while (prev_start > 0 && bytes[prev_start - 1] != '\n' && bytes[prev_start - 1] != '\r') {
            prev_start--;
        }
        parser->last_line_length = (int)(prev_end - prev_start);
    }

    {
        size_t feed_pos = restart_byte;
        while (feed_pos < length) {
            size_t next = line_end(bytes, length, feed_pos);
            if (!offset_push(mem, &line_offsets, feed_pos)) {
                goto failed;
            }
            markdown_core_parser_feed(parser, (const char *)bytes + feed_pos, next - feed_pos);
            fed_lines++;
            if (parser->oom || map->oom) {
                goto failed;
            }
            feed_pos = next;
            if (feed_pos >= pending.new_hi && feed_pos < length && (ptrdiff_t)feed_pos >= pending.delta &&
                (parser_is_clean(parser) || (parser_open_defs_only(parser) && line_seals(bytes, length, feed_pos)))) {
                size_t old_byte = (size_t)((ptrdiff_t)feed_pos - pending.delta);
                boundary_pos = boundary_position(&session->clean, old_byte, restart_pos);
                if (boundary_pos >= 0) {
                    break;
                }
            }
        }
    }

    markdown_core_parser_finalize_blocks(parser);
    if (parser->oom || map->oom) {
        goto failed;
    }
    total_lines = parser->line_number;
    last_line_length = parser->last_line_length;
    // On reflow the last fed line is the one just before the boundary; its
    // terminator-stripped length re-dates transplanted ends below.
    staged_tail_length = parser->last_line_length;

    // --- 3. definition reconciliation ---
    int boundary_line = boundary_pos >= 0 ? session->clean.items[boundary_pos].start_line : INT_MAX;
    {
        markdown_core_node *sibling;
        markdown_core_node *stop = boundary_pos >= 0 ? entry_node_at(&session->clean.items[boundary_pos], doc) : NULL;
        size_t filled = 0;
        for (sibling = restart_node; sibling && sibling != stop; sibling = sibling->next) {
            stale_count++;
        }
        if (stale_count) {
            stale_ids = (uint64_t *)mem->calloc(mem, stale_count, sizeof(*stale_ids));
            if (!stale_ids) {
                goto failed;
            }
            for (sibling = restart_node; sibling && sibling != stop; sibling = sibling->next) {
                stale_ids[filled++] = sibling->id;
            }
            qsort(stale_ids, stale_count, sizeof(*stale_ids), id_compare);
        }
        if (!collect_new_definitions(mem, map, previous_head, &new_defs) ||
            !collect_stale_definitions(mem, session, restart_line, boundary_line, &old_defs)) {
            goto failed;
        }
        // Sentinel lines for the staged region: vanished clean definition
        // paragraphs of this parse, one per line, taken from the staged
        // list before any map surgery. `new_defs` is document-ordered, so
        // deduping neighbors suffices.
        if (!doc->first_child || restart_node == doc->first_child) {
            size_t i;
            size_t upper = 0;
            for (i = 0; i < new_defs.count; i++) {
                const markdown_core_map_entry *entry = &new_defs.items[i]->entry;
                if (entry->owner == 0 && entry->from_vanished_clean) {
                    upper++;
                }
            }
            if (upper) {
                sentinel_lines = (int *)mem->calloc(mem, upper, sizeof(*sentinel_lines));
                if (!sentinel_lines) {
                    goto failed;
                }
                for (i = 0; i < new_defs.count; i++) {
                    const markdown_core_map_entry *entry = &new_defs.items[i]->entry;
                    if (entry->owner == 0 && entry->from_vanished_clean &&
                        (sentinel_count == 0 || sentinel_lines[sentinel_count - 1] != entry->start_line)) {
                        sentinel_lines[sentinel_count++] = entry->start_line;
                    }
                }
            }
        }

        defs_equal = definition_sequences_equal(&old_defs, &new_defs);
        if (!defs_equal) {
            bool fallback = false;
            size_t i;
            if (!reconcile_prepare(
                    session,
                    map,
                    order_floor,
                    restart_line,
                    boundary_line,
                    &old_defs,
                    &new_defs,
                    &reconcile,
                    &fallback
                ) ||
                !collect_dependents(
                    session,
                    &reconcile.dirty,
                    stale_ids,
                    stale_count,
                    &dependents,
                    &dependent_count,
                    &fallback
                )) {
                if (fallback) {
                    result = MARKDOWN_CORE_INCREMENTAL_FALLBACK;
                }
                goto failed;
            }
            // Routing: when the changed labels' dependents approach the
            // whole document, the full path's wholesale rebuilds beat
            // per-unit splice maintenance (measured 31% on the
            // every-unit-affected shape). Only the small staged region's
            // parse is discarded by falling back here.
            if (dependent_count >= 64) {
                // Count one child past the threshold: stopping exactly at
                // it would make the comparison true for every larger
                // document too.
                size_t children = 0;
                markdown_core_node *child;
                for (child = doc->first_child; child && children <= dependent_count * 2; child = child->next) {
                    children++;
                }
                if (dependent_count * 2 >= children) {
                    result = MARKDOWN_CORE_INCREMENTAL_FALLBACK;
                    goto failed;
                }
            }
            if (dependent_count) {
                holder = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_DOCUMENT, mem);
                if (!holder) {
                    goto failed;
                }
                for (i = 0; i < dependent_count; i++) {
                    markdown_core_node *clone = clone_unit_shell(session, dependents[i].unit);
                    if (!clone) {
                        goto failed;
                    }
                    clone->parent = holder;
                    clone->prev = holder->last_child;
                    if (holder->last_child) {
                        holder->last_child->next = clone;
                    } else {
                        holder->first_child = clone;
                    }
                    holder->last_child = clone;
                    dependents[i].staged = clone;
                }
            }
            // The last allocation-bearing step is behind; reconcile the map
            // in place so the inline phase resolves the final winners.
            reconcile_apply(session, map, &new_defs, &reconcile);
        }
    }

    // --- inline seam: reuse the restart unit's inert inline prefix ---
    // When the first staged unit reparses the restart paragraph and both
    // contents share a line-aligned prefix free of inline special
    // characters, that prefix's inline nodes (one Text and one break per
    // line) survive as-is: inline parsing starts at the seam
    // (S_parse_node_inlines), adoption skips the reserved old children
    // (adopt_push), and the splice transplants them into the staged leaf.
    // Columns are raw line-local values that sealing never adjusts, so the
    // transplant is only sound when the two leaves share the same column
    // environment (start column and internal offset) and neither is a
    // position-free synthesized block (start_line 0).
    if (restart_node && restart_node->type == MARKDOWN_CORE_NODE_PARAGRAPH && !restart_node->extension &&
        restart_node->first_child && (restart_node->flags & MARKDOWN_CORE_NODE__SEALED_RELATIVE) && parser->root &&
        parser->root->first_child && parser->root->first_child->type == MARKDOWN_CORE_NODE_PARAGRAPH &&
        !parser->root->first_child->extension && parser->root->first_child->start_line != 0 &&
        parser->root->first_child->start_column == restart_node->start_column &&
        parser->root->first_child->internal_offset == restart_node->internal_offset &&
        markdown_core_node_owns_inlines(parser->root->first_child)) {
        markdown_core_node *staged_leaf = parser->root->first_child;
        bufsize_t seam;
        // The scan must see every attached extension's special characters
        // (directive ':', strikethrough '~', ...); outside process_inlines
        // they are not yet folded into the parser's table.
        markdown_core_parser_manage_extensions_special_characters(parser, true);
        seam = markdown_core_inline_seam_prefix(
            parser,
            (const unsigned char *)restart_node->content.ptr,
            (bufsize_t)restart_node->content.size,
            (const unsigned char *)staged_leaf->content.ptr,
            (bufsize_t)staged_leaf->content.size,
            parser->options
        );
        markdown_core_parser_manage_extensions_special_characters(parser, false);
        if (seam > 0) {
            staged_leaf->user_data = (void *)(uintptr_t)((size_t)seam + 1);
        }
    }

    // --- 4. inline phase over the staged region and the dependent units ---
    // Unlimited budget: the estimate check below proves a one-shot parse
    // stays within its own budget, so no lookup can be denied in either.
    map->ref_size = 0;
    map->max_ref_size = (size_t)-1;
    if (session->record_lookups) {
        map->lookup_sink = markdown_core_lookup_recording_sink;
        map->lookup_context = &recording;
    }
    root = markdown_core_parser_refine_blocks(parser);
    if (root && dependent_count) {
        size_t i;
        markdown_core_parser_manage_extensions_special_characters(parser, true);
        for (i = 0; i < dependent_count; i++) {
            // A promoted replacement frees the parsed shell, but a shell that
            // performed lookups always keeps bracket-derived children and is
            // never promoted, so recorded unit pointers stay alive.
            dependents[i].staged = markdown_core_parser_refine_unit(parser, map, dependents[i].staged);
        }
        markdown_core_parser_manage_extensions_special_characters(parser, false);
    }
    map->lookup_sink = NULL;
    map->lookup_context = NULL;
    map->lookup_unit = NULL;
    {
        size_t phase_expansion = map->ref_size;
        bool parse_lost = parser->oom || map->oom || recording.lost;
        map->max_ref_size = budget;
        parser->refmap = own_map;
        own_map = NULL;
        markdown_core_session_release_parser(session, parser);
        parser = NULL;
        if (!root || parse_lost) {
            goto failed;
        }
        // The budget shrinks with the text, so the estimate may already sit
        // above it; both cases mean a one-shot parse could deny lookups this
        // phase resolved, and only a full reparse settles that.
        if (session->expansion_estimate > budget || phase_expansion > budget - session->expansion_estimate) {
            result = MARKDOWN_CORE_INCREMENTAL_FALLBACK;
            goto failed;
        }
        session->expansion_estimate += phase_expansion; // an upper bound stays one if a later step fails
    }

    // The seal walks double as the node count for the id reservation below:
    // the parse root's count includes the root holder itself, and the
    // dependents' counts sum to exactly the holder's child chain.
    sealed_nodes = markdown_core_session_seal_positions(root) - 1;
    {
        // Rebuilt units seal like parse roots (absolute start kept, children
        // relativized), then take the replaced unit's stored start: the
        // position did not change, so the parent-relative value is already
        // right — and stays right when a reflow later line-shifts a suffix
        // ancestor.
        size_t i;
        for (i = 0; i < dependent_count; i++) {
            sealed_nodes += markdown_core_session_seal_positions(dependents[i].staged);
            dependents[i].staged->start_line = dependents[i].unit->start_line;
        }
    }

    // Footnote sites of the staged region, collected and interned while
    // falling back is still free. Classification against the persistent
    // lists waits until adoption has fixed node ids: a sequence-preserving
    // commit then patches the index in place, anything else merges and
    // rebuilds.
    if (session->options.footnotes) {
        if (!markdown_core_footnote_collect_sites(mem, root, NULL, &staged_defs, &staged_refs) ||
            !markdown_core_session_footnote_label_sites(session, &staged_defs, &staged_refs)) {
            goto failed;
        }
    }

    // --- 5/6. adoption and the transactional splice ---
    {
        // A sentinel boundary's suffix starts at the first real child whose
        // start reaches it; real children between restart and boundary stay
        // in the stale walk.
        markdown_core_node *boundary_node =
            boundary_pos >= 0 ? entry_node_at(&session->clean.items[boundary_pos], doc) : NULL;
        markdown_core_node *first_stale = restart_node == boundary_node ? NULL : restart_node;
        markdown_core_node *last_stale = NULL;
        markdown_core_node *prefix_tail = NULL;
        markdown_core_node *suffix_head = boundary_node;
        markdown_core_node *staged_first = NULL;
        markdown_core_node *staged_last = NULL;
        size_t staged_nodes = sealed_nodes;
        size_t staged_clean = 0;
        size_t prefix_clean = restart_pos >= 0 ? (size_t)restart_pos : 0;
        size_t boundary_idx = boundary_pos >= 0 ? (size_t)boundary_pos : session->clean.count;
        size_t suffix_clean = session->clean.count - boundary_idx;
        size_t clean_count = 0;
        int delta_lines = 0;
        markdown_core_node *sibling;

        for (sibling = root->first_child; sibling; sibling = sibling->next) {
            if (sibling->flags & MARKDOWN_CORE_NODE__CLEAN_START) {
                staged_clean++;
            }
        }

        clean_count = prefix_clean + sentinel_count + staged_clean + suffix_clean;

        if (!markdown_core_session_ids_reserve(session, staged_nodes)) {
            goto failed;
        }
        // The clean index updates in place after the point of no return
        // (prefix untouched, stale run replaced, suffix slid or biased),
        // so any growth it needs happens here, while failing is still free.
        if (clean_count > session->clean.capacity) {
            markdown_core_clean_child *grown =
                (markdown_core_clean_child *)
                    mem->realloc(mem, session->clean.items, clean_count * sizeof(*session->clean.items));
            if (!grown) {
                goto failed;
            }
            session->clean.items = grown;
            session->clean.capacity = clean_count;
        }

        // Detach the graveyard (reversible pointer surgery).
        if (first_stale) {
            last_stale = suffix_head ? suffix_head->prev : doc->last_child;
            prefix_tail = first_stale->prev;
            if (prefix_tail) {
                prefix_tail->next = suffix_head;
            } else {
                doc->first_child = suffix_head;
            }
            if (suffix_head) {
                suffix_head->prev = prefix_tail;
            } else {
                doc->last_child = prefix_tail;
            }
            first_stale->prev = NULL;
            last_stale->next = NULL;
        } else {
            prefix_tail = suffix_head ? suffix_head->prev : doc->last_child;
        }

        // Adoption: a stack dummy document fronts the graveyard so the
        // standard machine classifies the real document node through the
        // staged root (same id, same changed/bubbled semantics). Rebuilt
        // units adopt pairwise; a kind change (formula promotion) never
        // adopts and reports removed + added instead. The recording bundle,
        // its table reservation, and the ancestor-bubble reservation are the
        // last allocation-bearing steps before the splice.
        {
            bool staged_ok;
            size_t i;
            {
                markdown_core_node dummy;
                memset(&dummy, 0, sizeof(dummy));
                dummy.type = (uint16_t)MARKDOWN_CORE_NODE_DOCUMENT;
                dummy.id = doc->id;
                dummy.last_changed_rev = doc->last_changed_rev;
                dummy.first_child = first_stale;
                dummy.last_child = last_stale;
                staged_ok = markdown_core_session_adopt(session, &dummy, root, new_rev, changes);
            }
            for (i = 0; i < dependent_count && staged_ok; i++) {
                dependent_unit *dep = &dependents[i];
                if (dep->unit->type == dep->staged->type) {
                    staged_ok = markdown_core_session_adopt(session, dep->unit, dep->staged, new_rev, changes);
                    dep->changed = dep->staged->last_changed_rev == new_rev;
                } else {
                    staged_ok = markdown_core_session_record_removed(session, dep->unit, changes) &&
                                markdown_core_session_adopt(session, NULL, dep->staged, new_rev, changes);
                    dep->changed = true;
                }
            }
            if (staged_ok) {
                staged_ok = markdown_core_lookup_recording_bundle(&recording, &bundles, &bundle_count) &&
                            markdown_core_lookup_table_reserve(mem, &session->lookups, bundle_count) &&
                            markdown_core_lookup_postings_reserve(mem, &session->lookups, bundles, bundle_count);
            }
            for (i = 0; i < dependent_count && staged_ok; i++) {
                markdown_core_node *ancestor;
                if (!dependents[i].changed) {
                    continue;
                }
                for (ancestor = dependents[i].unit->parent; ancestor && staged_ok; ancestor = ancestor->parent) {
                    size_t k;
                    bool collected = false;
                    if (ancestor == doc && root->last_changed_rev == new_rev) {
                        break; // the dummy verdict already bumps the document
                    }
                    for (k = 0; k < bubble_count && !collected; k++) {
                        collected = bubble_nodes[k].node == ancestor;
                    }
                    if (collected) {
                        break; // and with it every ancestor above
                    }
                    if (bubble_count == bubble_capacity) {
                        size_t grown_capacity = bubble_capacity ? bubble_capacity * 2 : 8;
                        bubble_ancestor *grown =
                            (bubble_ancestor *)mem->realloc(mem, bubble_nodes, grown_capacity * sizeof(*grown));
                        if (!grown) {
                            staged_ok = false;
                            break;
                        }
                        bubble_nodes = grown;
                        bubble_capacity = grown_capacity;
                    }
                    bubble_nodes[bubble_count].node = ancestor;
                    bubble_nodes[bubble_count].previous_rev = ancestor->last_changed_rev;
                    bubble_count++;
                }
            }
            if (staged_ok && changes && bubble_count) {
                staged_ok = markdown_core_id_array_reserve(&changes->bubbled, bubble_count);
            }
            if (!staged_ok) {
                // Undo the detach; nothing else has changed.
                if (first_stale) {
                    first_stale->prev = prefix_tail;
                    last_stale->next = suffix_head;
                    if (prefix_tail) {
                        prefix_tail->next = first_stale;
                    } else {
                        doc->first_child = first_stale;
                    }
                    if (suffix_head) {
                        suffix_head->prev = last_stale;
                    } else {
                        doc->last_child = last_stale;
                    }
                }
                goto failed;
            }
        }

        // Splice the staged children in.
        staged_first = root->first_child;
        staged_last = root->last_child;
        root->first_child = NULL;
        root->last_child = NULL;
        if (staged_first) {
            for (sibling = staged_first; sibling; sibling = sibling->next) {
                sibling->parent = doc;
            }
            staged_first->prev = prefix_tail;
            staged_last->next = suffix_head;
            if (prefix_tail) {
                prefix_tail->next = staged_first;
            } else {
                doc->first_child = staged_first;
            }
            if (suffix_head) {
                suffix_head->prev = staged_last;
            } else {
                doc->last_child = staged_last;
            }
        }

        // Rebuilt units swap into place (pure pointer surgery). The suffix
        // and prefix cursors follow a replaced boundary node so the line
        // bookkeeping below walks the live chain.
        {
            size_t i;
            for (i = 0; i < dependent_count; i++) {
                dependent_unit *dep = &dependents[i];
                markdown_core_node *staged = dep->staged;
                if (holder->first_child == staged) {
                    holder->first_child = staged->next;
                }
                if (holder->last_child == staged) {
                    holder->last_child = staged->prev;
                }
                if (staged->prev) {
                    staged->prev->next = staged->next;
                }
                if (staged->next) {
                    staged->next->prev = staged->prev;
                }
                staged->parent = NULL;
                staged->prev = NULL;
                staged->next = NULL;
                splice_replace(dep->unit, staged);
                if (dep->unit == suffix_head) {
                    suffix_head = staged;
                }
                if (dep->unit == prefix_tail) {
                    prefix_tail = staged;
                }
            }
        }

        // Footnote refresh from the merged site lists: the last fallible
        // step, O(sites) rather than a tree walk. The two-phase diff mutates
        // nothing on failure, so undoing the splices restores the previous
        // revision exactly.
        // Stamp the document and the bubbling ancestors before the footnote
        // diff runs: its ancestor climb must see every node this commit
        // already classifies as bumped, or it re-records them (a `changed`
        // document would also land in `bubbled`, breaking the disjointness
        // contract). The delta ids are still recorded after the point of
        // no return; on a failed diff the stamps roll back below.
        {
            size_t i;
            previous_doc_rev = doc->last_changed_rev;
            doc->last_changed_rev = root->last_changed_rev;
            for (i = 0; i < bubble_count; i++) {
                bubble_nodes[i].node->last_changed_rev = new_rev;
            }
        }

        if (session->options.footnotes) {
            markdown_core_incremental_result refreshed = footnote_refresh(
                session,
                &staged_defs,
                &staged_refs,
                stale_ids,
                stale_count,
                restart_line,
                boundary_line,
                dependents,
                dependent_count,
                new_rev,
                changes,
                &footnotes,
                &footnotes_built
            );
            if (refreshed != MARKDOWN_CORE_INCREMENTAL_COMMITTED) {
                result = refreshed;
                {
                    size_t i;
                    doc->last_changed_rev = previous_doc_rev;
                    for (i = 0; i < bubble_count; i++) {
                        bubble_nodes[i].node->last_changed_rev = bubble_nodes[i].previous_rev;
                    }
                }
                // Swap the committed units back in and re-park the rebuilt
                // ones under the holder so the shared cleanup frees them.
                {
                    size_t i;
                    for (i = 0; i < dependent_count; i++) {
                        dependent_unit *dep = &dependents[i];
                        markdown_core_node *staged = dep->staged;
                        splice_replace(staged, dep->unit);
                        if (staged == suffix_head) {
                            suffix_head = dep->unit;
                        }
                        if (staged == prefix_tail) {
                            prefix_tail = dep->unit;
                        }
                        staged->parent = holder;
                        staged->prev = holder->last_child;
                        if (holder->last_child) {
                            holder->last_child->next = staged;
                        } else {
                            holder->first_child = staged;
                        }
                        holder->last_child = staged;
                    }
                }
                // Splice the staged children back out ...
                if (staged_first) {
                    if (prefix_tail) {
                        prefix_tail->next = suffix_head;
                    } else {
                        doc->first_child = suffix_head;
                    }
                    if (suffix_head) {
                        suffix_head->prev = prefix_tail;
                    } else {
                        doc->last_child = prefix_tail;
                    }
                    staged_first->prev = NULL;
                    staged_last->next = NULL;
                    free_child_chain(staged_first);
                }
                // ... and the graveyard back in.
                if (first_stale) {
                    first_stale->prev = prefix_tail;
                    last_stale->next = suffix_head;
                    if (prefix_tail) {
                        prefix_tail->next = first_stale;
                    } else {
                        doc->first_child = first_stale;
                    }
                    if (suffix_head) {
                        suffix_head->prev = last_stale;
                    } else {
                        doc->last_child = last_stale;
                    }
                }
                // The refresh runs post-adoption, so the delta already
                // holds this attempt's ids; a fallback re-records against
                // the full path and a failure reports nothing.
                if (changes) {
                    changes->added.count = 0;
                    changes->removed.count = 0;
                    changes->changed.count = 0;
                    changes->bubbled.count = 0;
                }
                goto failed;
            }
        }

        // --- point of no return: nothing below can fail ---

        // The document's revision (the staged root carried the adoption
        // verdict under the document's id) and the bubbling ancestors'
        // revisions were already stamped before the footnote diff.
        if (boundary_pos >= 0) {
            delta_lines = (restart_line + fed_lines) - session->clean.items[boundary_pos].start_line;
            total_lines = session->total_lines + delta_lines;
            last_line_length = session->last_line_length;
            if (delta_lines != 0) {
                for (sibling = suffix_head; sibling; sibling = sibling->next) {
                    // Position-free roots keep their raw zeros (see the seal).
                    if (sibling->flags & MARKDOWN_CORE_NODE__SEALED_RELATIVE) {
                        sibling->start_line += delta_lines;
                    }
                }
            }
            // A transplanted block that closed inside its own first line dated
            // its end to the line before the boundary (see finalize): a staged
            // line whose length may just have changed. Every such block sits
            // on the boundary line, i.e. on the chain of sealed block children
            // that share their parent's start line, so a pruned walk from the
            // first suffix child re-dates them all.
            {
                markdown_core_node *node = suffix_head;
                while (node) {
                    markdown_core_node *step = NULL;
                    markdown_core_node *probe;
                    if (node->end_line == -1 && (node->flags & MARKDOWN_CORE_NODE__SEALED_RELATIVE) &&
                        MARKDOWN_CORE_NODE_BLOCK_P(node)) {
                        node->end_column = staged_tail_length;
                    }
                    for (probe = node->first_child; probe; probe = probe->next) {
                        if ((probe->flags & MARKDOWN_CORE_NODE__SEALED_RELATIVE) && probe->start_line == 0 &&
                            MARKDOWN_CORE_NODE_BLOCK_P(probe)) {
                            step = probe;
                            break;
                        }
                    }
                    while (!step && node != suffix_head) {
                        for (probe = node->next; probe; probe = probe->next) {
                            if ((probe->flags & MARKDOWN_CORE_NODE__SEALED_RELATIVE) && probe->start_line == 0 &&
                                MARKDOWN_CORE_NODE_BLOCK_P(probe)) {
                                step = probe;
                                break;
                            }
                        }
                        node = node->parent;
                    }
                    node = step;
                }
            }
        }
        doc->end_line = total_lines - 1;
        doc->end_column = last_line_length;

        // At-rest entries beyond the boundary follow the same line shift as
        // the suffix children and clean entries below. The reconciled path
        // shifts by index position (the array briefly mixes old suffix
        // coordinates with current staged ones); the equal path still holds
        // old lines everywhere, so the boundary line bounds the range.
        if (boundary_pos >= 0 && delta_lines != 0) {
            size_t start = reconcile.applied ? reconcile.splice_lo + new_defs.count
                                             : def_lower_bound(session->def_index, session->def_count, boundary_line);
            size_t at;
            for (at = start; at < session->def_count; at++) {
                session->def_index[at]->start_line += delta_lines;
            }
        }

        // Definitions. Equal sequences: the old entries stay (their orders
        // are document order), take over the staged anchors and geometry,
        // and the staged duplicates leave. Reconciled sequences: the staged
        // entries are the truth and their pointer-stamped anchors resolve to
        // ids. Staged lines are already absolute in current coordinates.
        {
            size_t i;
            uint64_t head_owner = prefix_tail ? prefix_tail->id : 0;
            if (defs_equal) {
                for (i = 0; i < old_defs.count; i++) {
                    uint64_t anchor = new_defs.items[i]->entry.owner;
                    old_defs.items[i]->entry.owner =
                        anchor == 0 ? head_owner : ((const markdown_core_node *)(uintptr_t)anchor)->id;
                    old_defs.items[i]->entry.start_line = new_defs.items[i]->entry.start_line;
                    old_defs.items[i]->entry.from_vanished_clean = new_defs.items[i]->entry.from_vanished_clean;
                }
                markdown_core_map_remove_until(map, previous_head);
            } else {
                for (i = 0; i < new_defs.count; i++) {
                    uint64_t anchor = new_defs.items[i]->entry.owner;
                    new_defs.items[i]->entry.owner =
                        anchor == 0 ? head_owner : ((const markdown_core_node *)(uintptr_t)anchor)->id;
                }
            }
        }

        // Inline seam transplant: the reserved prefix children move from the
        // replaced leaf into its staged successor ahead of the reparsed
        // suffix, keeping their ids, revisions, sealed positions, and delta
        // silence; the stale walks below then never see them.
        if (staged_first && staged_first->user_data && first_stale && first_stale == restart_node) {
            bufsize_t seam = (bufsize_t)((uintptr_t)staged_first->user_data - 1);
            size_t reserved = 0;
            bufsize_t b;
            for (b = 0; b < seam; b++) {
                if (staged_first->content.ptr[b] == '\n') {
                    reserved += 2;
                }
            }
            if (reserved) {
                markdown_core_node *head = first_stale->first_child;
                markdown_core_node *tail = head;
                markdown_core_node *walk;
                size_t k;
                for (k = 1; k < reserved; k++) {
                    tail = tail->next;
                }
                first_stale->first_child = tail->next;
                if (tail->next) {
                    tail->next->prev = NULL;
                } else {
                    first_stale->last_child = NULL;
                }
                tail->next = NULL;
                for (walk = head; walk; walk = walk->next) {
                    walk->parent = staged_first;
                    // Text literals borrow the parent block's content
                    // buffer; the old leaf's buffer dies with it, so rebase
                    // each borrowed chunk onto the staged leaf's identical
                    // prefix bytes.
                    if (walk->type == MARKDOWN_CORE_NODE_TEXT && walk->as.literal.alloc == 0 && walk->as.literal.data) {
                        walk->as.literal.data =
                            staged_first->content.ptr + (walk->as.literal.data - first_stale->content.ptr);
                    }
                }
                if (staged_first->first_child) {
                    tail->next = staged_first->first_child;
                    staged_first->first_child->prev = tail;
                } else {
                    staged_first->last_child = tail;
                }
                staged_first->first_child = head;
                head->prev = NULL;
            }
            staged_first->user_data = NULL;
        }

        // Id table: repoint adopted ids at their staged nodes, then drop
        // whatever still points into the graveyard or a replaced unit.
        ids_put_chain(session, staged_first, suffix_head);
        {
            size_t i;
            for (i = 0; i < dependent_count; i++) {
                ids_put_chain(session, dependents[i].staged, dependents[i].staged->next);
            }
        }
        ids_remove_stale_chain(session, first_stale);
        {
            size_t i;
            for (i = 0; i < dependent_count; i++) {
                ids_remove_stale_chain(session, dependents[i].unit);
            }
        }

        // Lookup records: every id of the graveyard and of the replaced
        // units leaves (adopted ids included), then the fresh bundles
        // re-install the records of everything this commit parsed.
        lookups_remove_chain(session, first_stale);
        {
            size_t i;
            for (i = 0; i < dependent_count; i++) {
                lookups_remove_chain(session, dependents[i].unit);
            }
            for (i = 0; i < bundle_count; i++) {
                markdown_core_lookup_table_put(mem, &session->lookups, bundles[i].unit->id, bundles[i].record);
                bundles[i].record.labels = NULL; // moved into the table
                bundles[i].record.positions = NULL;
                bundles[i].record.count = 0;
            }
        }

        // Ancestors of a changed rebuilt unit were stamped before the
        // footnote diff; only their delta ids are recorded here. The
        // document never appears: its verdict rode the staged root.
        if (changes) {
            size_t i;
            for (i = 0; i < bubble_count; i++) {
                markdown_core_id_array_push(&changes->bubbled, bubble_nodes[i].node->id);
            }
        }

        // Footnote index swap.
        if (footnotes_built) {
            markdown_core_footnote_index_release(mem, &session->footnotes);
            session->footnotes = footnotes;
        }

        // Clean-child index, updated in place: the prefix is untouched, the
        // suffix run slides and takes the edit deltas (skipped outright when
        // nothing moved — the steady cost of an interior same-length edit is
        // then just the stale run), and the staged entries land in the
        // middle. The suffix moves before the middle is written because a
        // growing middle overlaps the suffix's old slots.
        {
            markdown_core_clean_child *items = session->clean.items;
            ptrdiff_t index_shift = (ptrdiff_t)(prefix_clean + sentinel_count + staged_clean) - (ptrdiff_t)boundary_idx;
            size_t filled;
            size_t i;
            if (suffix_clean && (index_shift != 0 || pending.delta != 0 || delta_lines != 0)) {
                if (index_shift <= 0) {
                    for (i = 0; i < suffix_clean; i++) {
                        markdown_core_clean_child entry = items[boundary_idx + i];
                        entry.start_byte = (size_t)((ptrdiff_t)entry.start_byte + pending.delta);
                        entry.start_line += delta_lines;
                        items[(size_t)((ptrdiff_t)(boundary_idx + i) + index_shift)] = entry;
                    }
                } else {
                    for (i = suffix_clean; i-- > 0;) {
                        markdown_core_clean_child entry = items[boundary_idx + i];
                        entry.start_byte = (size_t)((ptrdiff_t)entry.start_byte + pending.delta);
                        entry.start_line += delta_lines;
                        items[(size_t)((ptrdiff_t)(boundary_idx + i) + index_shift)] = entry;
                    }
                }
            }
            filled = prefix_clean;
            // Sentinels precede every staged child: vanished definition
            // paragraphs only exist ahead of the first real child.
            for (i = 0; i < sentinel_count; i++) {
                items[filled].start_byte = line_offsets.items[sentinel_lines[i] - restart_line];
                items[filled].start_line = sentinel_lines[i];
                items[filled].node = NULL;
                filled++;
            }
            for (sibling = staged_first; sibling && sibling != suffix_head; sibling = sibling->next) {
                if (sibling->flags & MARKDOWN_CORE_NODE__CLEAN_START) {
                    int abs_line = sibling->start_line + 1;
                    items[filled].start_byte = line_offsets.items[abs_line - restart_line];
                    items[filled].start_line = abs_line;
                    items[filled].node = sibling;
                    filled++;
                }
            }
            session->clean.count = clean_count;
        }

        // A replaced top-level unit leaves its pointer in the clean index;
        // repoint it before the old node goes away. The index is ascending
        // in (final, post-slide) start lines, and a top-level unit's sealed
        // start is document-relative, so a binary probe lands exactly.
        {
            size_t i;
            for (i = 0; i < dependent_count; i++) {
                dependent_unit *dep = &dependents[i];
                int start_line;
                size_t lo = 0;
                size_t hi = session->clean.count;
                if (dep->staged->parent != doc) {
                    continue;
                }
                start_line = dep->staged->start_line + 1;
                while (lo < hi) {
                    size_t mid = lo + (hi - lo) / 2;
                    if (session->clean.items[mid].start_line < start_line) {
                        lo = mid + 1;
                    } else {
                        hi = mid;
                    }
                }
                if (lo < session->clean.count && session->clean.items[lo].node == dep->unit) {
                    session->clean.items[lo].node = dep->staged;
                }
            }
        }

        session->total_lines = total_lines;
        session->last_line_length = last_line_length;
        session->revision = new_rev;
        session->restarted_commits++;
        if (boundary_pos >= 0) {
            session->reflowed_commits++;
        }
        session->pending.dirty = false;
        session->pending.new_lo = 0;
        session->pending.new_hi = 0;
        session->pending.delta = 0;

        free_child_chain(first_stale);
        {
            size_t i;
            for (i = 0; i < dependent_count; i++) {
                free_child_chain(dependents[i].unit);
            }
        }
        markdown_core_node_free(root);
        root = NULL;
    }

    result = MARKDOWN_CORE_INCREMENTAL_COMMITTED;
    goto done;

failed:
    if (reconcile.applied) {
        // The map was reconciled in place and the commit could not finish:
        // its entries (including staged ones with pointer-stamped anchors)
        // no longer describe the committed tree. The tree itself was
        // restored, so the session stays valid at its previous revision, and
        // the flag routes the next commit through the full path, which
        // rebuilds the map wholesale without ever reading it.
        session->refmap_stale = true;
    } else {
        // Pointer-stamped duplicates never survive a failed or abandoned
        // pipeline: later commits must find only id-anchored entries at
        // rest.
        markdown_core_map_remove_until(map, previous_head);
    }
    if (parser) {
        parser->refmap = own_map;
        markdown_core_session_release_parser(session, parser);
    }
    if (root) {
        markdown_core_node_free(root);
    }
    if (result == MARKDOWN_CORE_INCREMENTAL_FAILED && error && !*error) {
        markdown_core_ast_set_error(
            error,
            MARKDOWN_CORE_ERROR_ALLOCATION_FAILED,
            "could not commit the session incrementally"
        );
    }

done:
    map->lookup_sink = NULL;
    map->lookup_context = NULL;
    map->lookup_unit = NULL;
    markdown_core_footnote_site_list_release(mem, &staged_defs);
    markdown_core_footnote_site_list_release(mem, &staged_refs);
    markdown_core_lookup_recording_release(&recording);
    markdown_core_unit_lookups_free(mem, bundles, bundle_count);
    reconcile_release(mem, &reconcile);
    if (holder) {
        markdown_core_node_free(holder); // any rebuilt unit not spliced in goes with it
    }
    if (dependents) {
        mem->free(mem, dependents);
    }
    if (bubble_nodes) {
        mem->free(mem, bubble_nodes);
    }
    if (sentinel_lines) {
        mem->free(mem, sentinel_lines);
    }
    if (line_offsets.items) {
        mem->free(mem, line_offsets.items);
    }
    if (new_defs.items) {
        mem->free(mem, new_defs.items);
    }
    if (old_defs.items) {
        mem->free(mem, old_defs.items);
    }
    if (stale_ids) {
        mem->free(mem, stale_ids);
    }
    return result;
}
