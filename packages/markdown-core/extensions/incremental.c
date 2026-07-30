#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session_internal.h"

#include <extension.h>
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
//                      that must re-refine. Core and extension owners expose
//                      the same complete inline-ownership-domain contract;
//                      an owner without that semantic capability falls back
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
//                      full path. Rebuilt inline domains use the same adopter
//                      behind bounded dummy roots while their semantic owners
//                      stay stable; ancestors of a changed domain bubble a
//                      revision bump.
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
        markdown_core_bufsize label_len = (markdown_core_bufsize)strlen((const char *)seed->label);
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
        markdown_core_bufsize label_len;

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
            label_len = (markdown_core_bufsize)strlen((const char *)label);
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

/* `stop` bounds the walk once the chain has been spliced into a longer
 * sibling list (the first suffix node, or NULL for an isolated chain). */
static void ids_put_chain(markdown_core_session *session, markdown_core_node *chain, const markdown_core_node *stop) {
    markdown_core_node *top;
    for (top = chain; top && top != stop; top = top->next) {
        markdown_core_node *node = top;
        for (;;) {
            markdown_core_session_ids_put(session, node->id, node);
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
            if (markdown_core_session_node_by_id(session, node->id) == node) {
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
            markdown_core_lookup_table_remove(session->mem, &session->lookups, node->id);
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

// One stable semantic owner outside the stale region whose lookup answers
// changed. `unit` never leaves the committed tree: the transaction adopts and
// swaps its complete child list plus backing buffer against the staged shell.
// The same swap rolls back.
typedef struct {
    markdown_core_node *unit;
    markdown_core_node *staged;
    uint64_t previous_revision;
    uint64_t next_revision;
    markdown_core_footnote_site_list staged_refs;
    bool changed;
    bool installed;
} dependent_unit;

// A committed ancestor collected for a revision bubble. The revision stamp
// lands before the footnote diff (so its ancestor climb sees the node as
// already classified), while the delta id is recorded after the point of
// no return; `previous_rev` restores the stamp when the diff fails.
typedef struct {
    markdown_core_node *node;
    uint64_t previous_rev;
} bubble_ancestor;

/* Builds the core Paragraph/Heading form of an inline ownership domain.
 * Extension owners use their descriptor callback below. */
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

static bool prepare_dependent_unit(markdown_core_session *session, dependent_unit *dependent, bool *invalid) {
    markdown_core_node *unit = dependent->unit;
    markdown_core_node *staged;

    *invalid = false;
    if (!unit->extension && (unit->type == MARKDOWN_CORE_NODE_PARAGRAPH || unit->type == MARKDOWN_CORE_NODE_HEADING) &&
        markdown_core_node_owns_inlines(unit)) {
        staged = clone_unit_shell(session, unit);
        if (!staged) {
            return false;
        }
    } else if (unit->extension && unit->extension->prepare_inline_domain) {
        staged = unit->extension->prepare_inline_domain(unit->extension, unit);
        if (!staged) {
            return false;
        }
    } else {
        *invalid = true;
        return false;
    }

    if (staged->type != unit->type || staged->extension != unit->extension ||
        staged->content.mem != unit->content.mem || staged->parent || staged->prev || staged->next) {
        *invalid = true;
        markdown_core_node_free(staged);
        return false;
    }
    staged->id = unit->id;
    dependent->staged = staged;
    dependent->previous_revision = unit->last_changed_rev;
    return true;
}

static bool dependent_domain_shape_is_valid(const dependent_unit *dependent) {
    const markdown_core_node *child;

    for (child = dependent->unit->first_child; child; child = child->next) {
        if (!MARKDOWN_CORE_NODE_TYPE_INLINE_P((markdown_core_node_type)child->type)) {
            return false;
        }
    }
    for (child = dependent->staged->first_child; child; child = child->next) {
        if (!MARKDOWN_CORE_NODE_TYPE_INLINE_P((markdown_core_node_type)child->type)) {
            return false;
        }
    }
    return true;
}

/* Collects the units that depend on a label whose winner changed by walking
 * the changed labels' postings — O(affected units), not O(units with
 * lookups) — skipping units the staged reparse rebuilds anyway. Returns
 * false with *fallback set when a record no longer matches the tree or no
 * complete inline-ownership-domain implementation exists, and false with
 * *fallback clear on allocation loss. */
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
        qsort(candidates, candidate_count, sizeof(*candidates), id_compare);
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
        if ((!unit->extension &&
             ((unit->type != MARKDOWN_CORE_NODE_PARAGRAPH && unit->type != MARKDOWN_CORE_NODE_HEADING) ||
              !markdown_core_node_owns_inlines(unit))) ||
            (unit->extension && !unit->extension->prepare_inline_domain)) {
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
        memset(&dependents[count], 0, sizeof(dependents[count]));
        dependents[count].unit = unit;
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
// its rebuilt ownership domain's, and every other site survives with its
// anchor classifying it against the restart line. Collection happens before
// install, adoption fixes ids, and the transactional merge runs after the
// reversible splice at O(previous sites + staged material).

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

/* One dependent owner's current reference run inside the persistent list.
 * `start` is also the exact insertion point when `count` is zero: stable
 * semantic owners make that position derivable from document tree order. */
typedef struct {
    size_t start;
    size_t count;
    markdown_core_node *anchor;
} unit_run;

/* Canonical preorder for two live semantic owners. The comparison walks only
 * their ancestor spines plus the one sibling run at the divergence; it never
 * inspects rebuilt inline descendants. */
static int owner_tree_order_compare(const markdown_core_node *left, const markdown_core_node *right) {
    const markdown_core_node *a = left;
    const markdown_core_node *b = right;
    const markdown_core_node *child;
    size_t a_depth = 0;
    size_t b_depth = 0;

    if (a == b) {
        return 0;
    }
    for (child = a; child->parent; child = child->parent) {
        a_depth++;
    }
    for (child = b; child->parent; child = child->parent) {
        b_depth++;
    }
    while (a_depth > b_depth) {
        a = a->parent;
        a_depth--;
        if (a == b) {
            return 1; // right is an ancestor and enters first
        }
    }
    while (b_depth > a_depth) {
        b = b->parent;
        b_depth--;
        if (a == b) {
            return -1; // left is an ancestor and enters first
        }
    }
    while (a->parent != b->parent) {
        a = a->parent;
        b = b->parent;
    }
    assert(a->parent);
    for (child = a->parent->first_child; child; child = child->next) {
        if (child == a) {
            return -1;
        }
        if (child == b) {
            return 1;
        }
    }
    assert(0 && "semantic owners do not share one live tree");
    return 0;
}

typedef struct {
    size_t index;
    size_t start;
    const markdown_core_node *owner;
    bool before_staged;
} clone_insert;

static int clone_insert_compare(const void *a, const void *b) {
    const clone_insert *left = (const clone_insert *)a;
    const clone_insert *right = (const clone_insert *)b;
    if (left->start != right->start) {
        return left->start < right->start ? -1 : 1;
    }
    if (left->before_staged != right->before_staged) {
        return left->before_staged ? -1 : 1;
    }
    {
        int order = owner_tree_order_compare(left->owner, right->owner);
        if (order != 0) {
            return order;
        }
    }
    return left->index < right->index ? -1 : left->index > right->index;
}

/* Merges one document-ordered site list into `out`. The staged run enters at
 * the graveyard/suffix boundary. A dependent run replaces its old run in
 * place; when it gained its first site, stable-owner tree order supplies the
 * exact insertion point. Returns FALLBACK when an untouched anchor no longer
 * parents to the document (the persistent list is stale), FAILED on
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
    const clone_insert *clone_inserts,
    size_t clone_insert_count,
    size_t clone_count,
    markdown_core_footnote_site_list *out
) {
    markdown_core_mem *mem = session->mem;
    markdown_core_node *doc = session->view.root;
    bool staged_emitted = false;
    size_t insert = 0;
    size_t i;

    for (i = 0; i < old_sites->count; i++) {
        const markdown_core_footnote_site *site = &old_sites->items[i];
        size_t found;
        while (insert < clone_insert_count && clone_inserts[insert].start == i && clone_inserts[insert].before_staged) {
            size_t index = clone_inserts[insert++].index;
            if (!site_run_append(mem, out, &clone_runs[index])) {
                return MARKDOWN_CORE_INCREMENTAL_FAILED;
            }
            clone_emitted[index] = true;
        }
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
        } else if (!staged_emitted && site->anchor->start_line + 1 >= restart_line) {
            if (!site_run_append(mem, out, staged_run)) {
                return MARKDOWN_CORE_INCREMENTAL_FAILED;
            }
            staged_emitted = true;
        }
        while (insert < clone_insert_count && clone_inserts[insert].start == i) {
            size_t index = clone_inserts[insert++].index;
            if (!site_run_append(mem, out, &clone_runs[index])) {
                return MARKDOWN_CORE_INCREMENTAL_FAILED;
            }
            clone_emitted[index] = true;
        }
        if (id_set_holds(stale_ids, stale_count, site->anchor->id)) {
            continue;
        }
        found = site->unit && clone_count ? clone_key_find(clone_keys, clone_count, site->unit) : SIZE_MAX;
        if (found == SIZE_MAX && site->anchor->parent != doc) {
            return MARKDOWN_CORE_INCREMENTAL_FALLBACK;
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
    while (insert < clone_insert_count && clone_inserts[insert].before_staged) {
        size_t index = clone_inserts[insert++].index;
        if (!site_run_append(mem, out, &clone_runs[index])) {
            return MARKDOWN_CORE_INCREMENTAL_FAILED;
        }
        clone_emitted[index] = true;
    }
    if (!staged_emitted) {
        if (!site_run_append(mem, out, staged_run)) {
            return MARKDOWN_CORE_INCREMENTAL_FAILED;
        }
        staged_emitted = true;
    }
    while (insert < clone_insert_count) {
        size_t index = clone_inserts[insert++].index;
        if (!site_run_append(mem, out, &clone_runs[index])) {
            return MARKDOWN_CORE_INCREMENTAL_FAILED;
        }
        clone_emitted[index] = true;
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

static size_t site_unit_lower_bound(
    const markdown_core_footnote_site_list *list,
    size_t lo,
    size_t hi,
    const markdown_core_node *unit
) {
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const markdown_core_node *mid_unit = list->items[mid].unit;
        assert(mid_unit);
        if (owner_tree_order_compare(mid_unit, unit) < 0) {
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

/* Refreshes the session's footnote index for one incremental commit. Runs
 * post-adoption (ids final), before the point of no return. A commit whose
 * stale ranges and rebuilt units carry the same label sequences as their
 * replacements patches the persistent index in place — pointer and
 * churned-id updates only, no aggregate can have moved — at
 * O(staged + stale + rebuilt) after two O(log sites) range probes. Any
 * other commit merges the site lists in document order and rebuilds the
 * derived structures from label slots (*out_built set, caller swaps at the
 * point of no return), diffing per node id to bump exactly the answers
 * that changed. Dependent runs were collected before install while their
 * staged domains were isolated; their node pointers now name the installed
 * children under the stable owner. */
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
    clone_insert *clone_inserts = NULL;
    bool *clone_emitted = NULL;
    unit_run *runs = NULL;
    markdown_core_incremental_result result = MARKDOWN_CORE_INCREMENTAL_FAILED;
    size_t d0 = site_lower_bound(&fn->defs, restart_line);
    size_t d1 = site_lower_bound(&fn->defs, boundary_line);
    size_t r0 = site_lower_bound(&fn->refs, restart_line);
    size_t r1 = site_lower_bound(&fn->refs, boundary_line);
    size_t churn = 0;
    size_t clone_insert_count = 0;
    bool fast;
    size_t i;

    if (dependent_count) {
        clone_runs = (markdown_core_footnote_site_list *)mem->calloc(mem, dependent_count, sizeof(*clone_runs));
        clone_keys = (clone_key *)mem->calloc(mem, dependent_count, sizeof(*clone_keys));
        clone_inserts = (clone_insert *)mem->calloc(mem, dependent_count, sizeof(*clone_inserts));
        clone_emitted = (bool *)mem->calloc(mem, dependent_count, sizeof(*clone_emitted));
        runs = (unit_run *)mem->calloc(mem, dependent_count, sizeof(*runs));
        if (!clone_runs || !clone_keys || !clone_inserts || !clone_emitted || !runs) {
            goto done;
        }
        for (i = 0; i < dependent_count; i++) {
            markdown_core_node *top = dependents[i].unit;
            bool before_staged;
            size_t hi;
            size_t lo;
            size_t at;
            while (top->parent && top->parent != doc) {
                top = top->parent;
            }
            runs[i].anchor = top;
            clone_runs[i] = dependents[i].staged_refs;
            before_staged = runs[i].anchor->start_line + 1 < restart_line;
            lo = before_staged ? 0 : r1;
            hi = before_staged ? r0 : fn->refs.count;
            at = site_unit_lower_bound(&fn->refs, lo, hi, dependents[i].unit);
            runs[i].start = at;
            while (at < hi && fn->refs.items[at].unit == dependents[i].unit) {
                at++;
            }
            runs[i].count = at - runs[i].start;
            if (runs[i].count == 0 && clone_runs[i].count) {
                clone_inserts[clone_insert_count].index = i;
                clone_inserts[clone_insert_count].start = runs[i].start;
                clone_inserts[clone_insert_count].owner = dependents[i].unit;
                clone_inserts[clone_insert_count].before_staged = before_staged;
                clone_insert_count++;
            }
            clone_keys[i].unit = dependents[i].unit;
            clone_keys[i].index = i;
        }
        qsort(clone_keys, dependent_count, sizeof(*clone_keys), clone_key_compare);
        if (clone_insert_count > 1) {
            qsort(clone_inserts, clone_insert_count, sizeof(*clone_inserts), clone_insert_compare);
        }
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
        NULL,
        0,
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
            clone_inserts,
            clone_insert_count,
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
        // Entries alias dependent_unit::staged_refs; the pipeline owns their
        // storage until its shared release.
        mem->free(mem, clone_runs);
    }
    if (clone_keys) {
        mem->free(mem, clone_keys);
    }
    if (clone_inserts) {
        mem->free(mem, clone_inserts);
    }
    if (clone_emitted) {
        mem->free(mem, clone_emitted);
    }
    if (runs) {
        mem->free(mem, runs);
    }
    return result;
}

#ifndef NDEBUG
static void assert_child_run(markdown_core_node *first, markdown_core_node *last) {
    markdown_core_node *child;
    markdown_core_node *previous;
    markdown_core_node *slow;
    markdown_core_node *fast;
    markdown_core_node *parent;
    markdown_core_node *before;
    markdown_core_node *after;

    assert(first);
    assert(last);
    child = first;
    previous = first->prev;
    slow = first;
    fast = first;
    parent = first->parent;
    before = first->prev;
    after = last->next;
    assert(previous == NULL || previous->next == first);
    for (;;) {
        assert(child);
        assert(child != before);
        assert(child != after);
        assert(child->parent == parent);
        assert(child->prev == previous);
        if (child == last) {
            break;
        }
        assert(child->next);
        assert(child->next->prev == child);
        previous = child;
        child = child->next;

        slow = slow && slow != last ? slow->next : NULL;
        fast = fast && fast != last ? fast->next : NULL;
        fast = fast && fast != last ? fast->next : NULL;
        assert(!slow || !fast || slow != fast);
    }
    assert(last->next == NULL || last->next->prev == last);
}
#else
#define assert_child_run(first, last) ((void)0)
#endif

/* Detaches the nonempty contiguous child run [`first`, `last`] while keeping
 * the run isolated and its parent pointers intact. `insert_child_chain` with
 * the captured neighbors is its inverse. */
static void detach_child_chain(markdown_core_node *parent, markdown_core_node *first, markdown_core_node *last) {
    markdown_core_node *previous;
    markdown_core_node *next;

    assert(parent);
    assert(first);
    assert(last);
    assert_child_run(first, last);
    previous = first->prev;
    next = last->next;
    assert(first->parent == parent);
    assert(last->parent == parent);
    assert(previous ? previous->next == first : parent->first_child == first);
    assert(next ? next->prev == last : parent->last_child == last);

    if (previous) {
        previous->next = next;
    } else {
        parent->first_child = next;
    }
    if (next) {
        next->prev = previous;
    } else {
        parent->last_child = previous;
    }
    first->prev = NULL;
    last->next = NULL;
    assert_child_run(first, last);
}

/* Inserts the isolated nonempty run [`first`, `last`] between `previous` and
 * `next`, assigning every child to `parent`. A detached run deliberately
 * keeps its old parent stamps until this operation or `free_child_chain`. */
static void insert_child_chain(
    markdown_core_node *parent,
    markdown_core_node *previous,
    markdown_core_node *next,
    markdown_core_node *first,
    markdown_core_node *last
) {
    markdown_core_node *child;

    assert(parent);
    assert(first);
    assert(last);
    assert_child_run(first, last);
    assert(first->prev == NULL);
    assert(last->next == NULL);
    assert(previous == NULL || previous->parent == parent);
    assert(next == NULL || next->parent == parent);
    assert(previous ? previous->next == next : parent->first_child == next);
    assert(next ? next->prev == previous : parent->last_child == previous);

    first->prev = previous;
    last->next = next;
    if (previous) {
        previous->next = first;
    } else {
        parent->first_child = first;
    }
    if (next) {
        next->prev = last;
    } else {
        parent->last_child = last;
    }

    for (child = first;; child = child->next) {
        child->parent = parent;
        if (child == last) {
            break;
        }
    }
    assert_child_run(first, last);
}

/* Appends one isolated child to a temporary staging parent. */
static void park_child(markdown_core_node *parent, markdown_core_node *child) {
    assert(parent);
    assert(child);
    assert(child->parent == NULL);
    assert(child->prev == NULL);
    assert(child->next == NULL);

    insert_child_chain(parent, parent->last_child, NULL, child, child);
}

/* Exchanges a stable owner's complete inline ownership domain with the
 * staged shell's complete child list. The child span and its sole backing
 * buffer move together, so borrowed Text chunks never need rebasing. This is
 * deliberately self-inverse: commit and rollback call the same operation. */
static void swap_dependent_domain(dependent_unit *dependent) {
    markdown_core_node *unit = dependent->unit;
    markdown_core_node *staged = dependent->staged;
    markdown_core_node *live_first = unit->first_child;
    markdown_core_node *live_last = unit->last_child;
    markdown_core_node *staged_first = staged->first_child;
    markdown_core_node *staged_last = staged->last_child;
    markdown_core_strbuf content;

    assert(unit);
    assert(staged);
    assert(unit->content.mem == staged->content.mem);
    assert((live_first == NULL) == (live_last == NULL));
    assert((staged_first == NULL) == (staged_last == NULL));

    if (live_first) {
        detach_child_chain(unit, live_first, live_last);
    }
    if (staged_first) {
        detach_child_chain(staged, staged_first, staged_last);
    }

    content = unit->content;
    unit->content = staged->content;
    staged->content = content;

    if (staged_first) {
        insert_child_chain(unit, NULL, NULL, staged_first, staged_last);
    }
    if (live_first) {
        insert_child_chain(staged, NULL, NULL, live_first, live_last);
    }

    dependent->installed = !dependent->installed;
}

typedef struct {
    ptrdiff_t restart_pos;
    ptrdiff_t boundary_pos;
    markdown_core_node *restart_node;
    markdown_core_node *boundary_node;
    size_t restart_byte;
    int restart_line;
    int boundary_line;
    int fed_lines;
    int total_lines;
    int last_line_length;
    int staged_tail_length;
} incremental_restart_plan;

typedef struct {
    markdown_core_node *first_stale;
    markdown_core_node *last_stale;
    markdown_core_node *prefix_tail;
    markdown_core_node *suffix_head;
    markdown_core_node *staged_first;
    markdown_core_node *staged_last;
    size_t staged_clean;
    size_t prefix_clean;
    size_t boundary_idx;
    size_t suffix_clean;
    size_t clean_count;
    int delta_lines;
    bool graveyard_detached;
    bool staged_installed;
    bool dependents_installed;
    bool revisions_stamped;
} incremental_splice_state;

/* Stack-owned incremental transaction. The parser temporarily borrows `map`
 * and `own_map` must be restored before its lease ends. `root` owns staged
 * children until install; `holder` owns dependent clones while they are
 * parked; a detached graveyard keeps committed parent stamps. After
 * `reconcile.applied`, abort quarantines the map via `refmap_stale`.
 * `footnotes_built` denotes an index owned here until its immediate PONR
 * transfer. False phase returns use `result` to distinguish failure from
 * fallback, and no allocation or failure is allowed after refresh succeeds. */
typedef struct {
    markdown_core_session *session;
    markdown_core_mem *mem;
    markdown_core_map *map;
    const unsigned char *bytes;
    size_t length;
    markdown_core_node *doc;
    markdown_core_edit_summary pending;
    size_t budget;
    uint64_t new_rev;
    markdown_core_delta *changes;
    markdown_core_error **error;
    markdown_core_incremental_result result;

    markdown_core_parser *parser;
    markdown_core_map *own_map;
    markdown_core_map_entry *previous_head;
    uint64_t order_floor;
    markdown_core_node *root;
    offset_list line_offsets;
    reference_list new_defs;
    reference_list old_defs;
    uint64_t *stale_ids;
    size_t stale_count;
    markdown_core_footnote_index footnotes;
    markdown_core_footnote_site_list staged_defs;
    markdown_core_footnote_site_list staged_refs;
    bool footnotes_built;
    markdown_core_lookup_recording recording;
    markdown_core_unit_lookups *bundles;
    size_t bundle_count;
    reconcile_state reconcile;
    bool defs_equal;
    dependent_unit *dependents;
    size_t dependent_count;
    markdown_core_node *holder;
    size_t sealed_nodes;
    bubble_ancestor *bubble_nodes;
    size_t bubble_count;
    size_t bubble_capacity;
    uint64_t previous_doc_rev;
    int *sentinel_lines;
    size_t sentinel_count;

    incremental_restart_plan plan;
    incremental_splice_state splice;
} incremental_pipeline;

static void incremental_pipeline_init(
    incremental_pipeline *pipeline,
    markdown_core_session *session,
    uint64_t new_rev,
    markdown_core_delta *changes,
    markdown_core_error **error
) {
    memset(pipeline, 0, sizeof(*pipeline));
    pipeline->session = session;
    pipeline->mem = session->mem;
    pipeline->map = session->refmap;
    pipeline->bytes = markdown_core_text_bytes(&session->text);
    pipeline->length = markdown_core_text_length(&session->text);
    pipeline->doc = session->view.root;
    pipeline->pending = session->pending;
    pipeline->budget = pipeline->length > MARKDOWN_CORE_SESSION_REF_BUDGET_FLOOR
                           ? pipeline->length
                           : MARKDOWN_CORE_SESSION_REF_BUDGET_FLOOR;
    pipeline->new_rev = new_rev;
    pipeline->changes = changes;
    pipeline->error = error;
    pipeline->result = MARKDOWN_CORE_INCREMENTAL_FAILED;
    pipeline->previous_head = pipeline->map->refs;
    pipeline->order_floor = pipeline->map->next_order + 1;
    pipeline->defs_equal = true;
    pipeline->plan.restart_pos = -1;
    pipeline->plan.boundary_pos = -1;
    pipeline->plan.boundary_line = INT_MAX;
    markdown_core_lookup_recording_init(&pipeline->recording, pipeline->mem);
}

static bool incremental_use_fallback(incremental_pipeline *pipeline) {
    pipeline->result = MARKDOWN_CORE_INCREMENTAL_FALLBACK;
    return false;
}

static bool incremental_plan_restart(incremental_pipeline *pipeline) {
    incremental_restart_plan *plan = &pipeline->plan;
    const unsigned char *bytes = pipeline->bytes;
    size_t length = pipeline->length;
    markdown_core_session *session = pipeline->session;

    plan->restart_pos = restart_position(&session->clean, pipeline->pending.new_lo);

    // An edit can place a '\n' exactly at the chosen boundary while the
    // untouched prefix ends with a lone '\r': the two fuse into one CRLF
    // terminator in the current text, and the boundary no longer starts a
    // line, so feeding from it would count a phantom line. One entry back
    // always restores the invariant: the earlier entry's surrounding bytes
    // lie strictly inside the untouched prefix, where old line starts
    // survive verbatim.
    if (plan->restart_pos >= 0) {
        size_t byte = session->clean.items[plan->restart_pos].start_byte;
        if (byte > 0 && byte < length && bytes[byte - 1] == '\r' && bytes[byte] == '\n') {
            plan->restart_pos--;
        }
    }

    // A sealing anchor is valid only while its first line keeps the shape
    // that closes the footnote definitions open above it; an edit at the
    // restart line can reshape it into a blank or indented continuation
    // those definitions would capture. Sentinels do not record their sealing
    // quality, so they take the same check. Only the chosen entry's line can
    // be damaged — every earlier entry's line lies wholly inside the
    // untouched prefix — so one entry back always restores validity.
    if (plan->restart_pos >= 0) {
        const markdown_core_clean_child *entry = &session->clean.items[plan->restart_pos];
        if ((!entry->node || (entry->node->flags & MARKDOWN_CORE_NODE__CLEAN_START_SEALING)) &&
            !line_seals(bytes, length, entry->start_byte)) {
            plan->restart_pos--;
        }
    }
    plan->restart_node = plan->restart_pos >= 0 ? entry_node_at(&session->clean.items[plan->restart_pos], pipeline->doc)
                                                : pipeline->doc->first_child;
    plan->restart_byte = plan->restart_pos >= 0 ? session->clean.items[plan->restart_pos].start_byte : 0;
    plan->restart_line = plan->restart_pos >= 0 ? session->clean.items[plan->restart_pos].start_line : 1;

    // A restart at or beyond the end of the text feeds nothing, so the
    // parser's line scalars would not describe the surviving prefix (whose
    // final line length is a validated-bytes measure only a parse can give).
    // Rare — the entire tail was deleted at a clean boundary — and bounded:
    // fall back to the full reparse.
    if (plan->restart_byte >= length) {
        return incremental_use_fallback(pipeline);
    }

    return true;
}

static bool incremental_reparse_blocks(incremental_pipeline *pipeline) {
    incremental_restart_plan *plan = &pipeline->plan;
    markdown_core_session *session = pipeline->session;
    markdown_core_parser *parser;
    size_t feed_pos;

    pipeline->parser = markdown_core_session_acquire_parser(session, pipeline->error);
    if (!pipeline->parser) {
        return false;
    }
    parser = pipeline->parser;
    pipeline->own_map = parser->refmap;
    parser->refmap = pipeline->map;
    parser->line_number = plan->restart_line - 1;

    // finalize() dates a block closed in the middle of a line to the end of
    // the line before it, so the staged parser must know the previous line's
    // terminator-stripped length. That line sits wholly inside the untouched
    // prefix (its bytes end at restart_byte, at or before the first edited
    // byte), so the current text still holds exactly what a one-shot parse
    // would have measured.
    if (plan->restart_byte > 0) {
        size_t prev_end = plan->restart_byte;
        size_t prev_start;
        if (pipeline->bytes[prev_end - 1] == '\n') {
            prev_end--;
        }
        if (prev_end > 0 && pipeline->bytes[prev_end - 1] == '\r') {
            prev_end--;
        }
        prev_start = prev_end;
        while (prev_start > 0 && pipeline->bytes[prev_start - 1] != '\n' && pipeline->bytes[prev_start - 1] != '\r') {
            prev_start--;
        }
        parser->last_line_length = (int)(prev_end - prev_start);
    }

    feed_pos = plan->restart_byte;
    while (feed_pos < pipeline->length) {
        size_t next = line_end(pipeline->bytes, pipeline->length, feed_pos);
        if (!offset_push(pipeline->mem, &pipeline->line_offsets, feed_pos)) {
            return false;
        }
        markdown_core_parser_feed(parser, (const char *)pipeline->bytes + feed_pos, next - feed_pos);
        plan->fed_lines++;
        if (parser->oom || pipeline->map->oom) {
            return false;
        }
        feed_pos = next;
        if (feed_pos >= pipeline->pending.new_hi && feed_pos < pipeline->length &&
            (ptrdiff_t)feed_pos >= pipeline->pending.delta &&
            (parser_is_clean(parser) ||
             (parser_open_defs_only(parser) && line_seals(pipeline->bytes, pipeline->length, feed_pos)))) {
            size_t old_byte = (size_t)((ptrdiff_t)feed_pos - pipeline->pending.delta);
            plan->boundary_pos = boundary_position(&session->clean, old_byte, plan->restart_pos);
            if (plan->boundary_pos >= 0) {
                break;
            }
        }
    }

    markdown_core_parser_finalize_blocks(parser);
    if (parser->oom || pipeline->map->oom) {
        return false;
    }
    plan->total_lines = parser->line_number;
    plan->last_line_length = parser->last_line_length;
    // On reflow the last fed line is the one just before the boundary; its
    // terminator-stripped length re-dates transplanted ends below.
    plan->staged_tail_length = parser->last_line_length;
    plan->boundary_line = plan->boundary_pos >= 0 ? session->clean.items[plan->boundary_pos].start_line : INT_MAX;
    return true;
}

static bool incremental_prepare_definitions(incremental_pipeline *pipeline) {
    incremental_restart_plan *plan = &pipeline->plan;
    markdown_core_session *session = pipeline->session;
    markdown_core_node *sibling;
    markdown_core_node *stop =
        plan->boundary_pos >= 0 ? entry_node_at(&session->clean.items[plan->boundary_pos], pipeline->doc) : NULL;
    size_t filled = 0;

    for (sibling = plan->restart_node; sibling && sibling != stop; sibling = sibling->next) {
        pipeline->stale_count++;
    }
    if (pipeline->stale_count) {
        pipeline->stale_ids =
            (uint64_t *)pipeline->mem->calloc(pipeline->mem, pipeline->stale_count, sizeof(*pipeline->stale_ids));
        if (!pipeline->stale_ids) {
            return false;
        }
        for (sibling = plan->restart_node; sibling && sibling != stop; sibling = sibling->next) {
            pipeline->stale_ids[filled++] = sibling->id;
        }
        qsort(pipeline->stale_ids, pipeline->stale_count, sizeof(*pipeline->stale_ids), id_compare);
    }
    if (!collect_new_definitions(pipeline->mem, pipeline->map, pipeline->previous_head, &pipeline->new_defs) ||
        !collect_stale_definitions(
            pipeline->mem,
            session,
            plan->restart_line,
            plan->boundary_line,
            &pipeline->old_defs
        )) {
        return false;
    }

    // Sentinel lines for the staged region: vanished clean definition
    // paragraphs of this parse, one per line, taken from the staged list
    // before any map surgery. `new_defs` is document-ordered, so deduping
    // neighbors suffices.
    if (!pipeline->doc->first_child || plan->restart_node == pipeline->doc->first_child) {
        size_t i;
        size_t upper = 0;
        for (i = 0; i < pipeline->new_defs.count; i++) {
            const markdown_core_map_entry *entry = &pipeline->new_defs.items[i]->entry;
            if (entry->owner == 0 && entry->from_vanished_clean) {
                upper++;
            }
        }
        if (upper) {
            pipeline->sentinel_lines =
                (int *)pipeline->mem->calloc(pipeline->mem, upper, sizeof(*pipeline->sentinel_lines));
            if (!pipeline->sentinel_lines) {
                return false;
            }
            for (i = 0; i < pipeline->new_defs.count; i++) {
                const markdown_core_map_entry *entry = &pipeline->new_defs.items[i]->entry;
                if (entry->owner == 0 && entry->from_vanished_clean &&
                    (pipeline->sentinel_count == 0 ||
                     pipeline->sentinel_lines[pipeline->sentinel_count - 1] != entry->start_line)) {
                    pipeline->sentinel_lines[pipeline->sentinel_count++] = entry->start_line;
                }
            }
        }
    }

    pipeline->defs_equal = definition_sequences_equal(&pipeline->old_defs, &pipeline->new_defs);
    if (!pipeline->defs_equal) {
        bool fallback = false;
        size_t i;
        if (!reconcile_prepare(
                session,
                pipeline->map,
                pipeline->order_floor,
                plan->restart_line,
                plan->boundary_line,
                &pipeline->old_defs,
                &pipeline->new_defs,
                &pipeline->reconcile,
                &fallback
            ) ||
            !collect_dependents(
                session,
                &pipeline->reconcile.dirty,
                pipeline->stale_ids,
                pipeline->stale_count,
                &pipeline->dependents,
                &pipeline->dependent_count,
                &fallback
            )) {
            if (fallback) {
                pipeline->result = MARKDOWN_CORE_INCREMENTAL_FALLBACK;
            }
            return false;
        }
        if (pipeline->dependent_count) {
            pipeline->holder = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_DOCUMENT, pipeline->mem);
            if (!pipeline->holder) {
                return false;
            }
            for (i = 0; i < pipeline->dependent_count; i++) {
                bool invalid;
                if (!prepare_dependent_unit(session, &pipeline->dependents[i], &invalid)) {
                    if (invalid && pipeline->error && !*pipeline->error) {
                        markdown_core_ast_set_error(
                            pipeline->error,
                            MARKDOWN_CORE_ERROR_INTERNAL,
                            "an extension returned an invalid inline ownership domain"
                        );
                    }
                    return false;
                }
                park_child(pipeline->holder, pipeline->dependents[i].staged);
            }
        }
        // The last allocation-bearing definition step is behind; reconcile
        // the map in place so the inline phase resolves the final winners.
        reconcile_apply(session, pipeline->map, &pipeline->new_defs, &pipeline->reconcile);
    }

    // A sentinel boundary's suffix starts at the first real child whose start
    // reaches it; real children between restart and boundary stay stale.
    plan->boundary_node =
        plan->boundary_pos >= 0 ? entry_node_at(&session->clean.items[plan->boundary_pos], pipeline->doc) : NULL;
    return true;
}

static void incremental_arm_inline_seam(incremental_pipeline *pipeline) {
    incremental_restart_plan *plan = &pipeline->plan;
    markdown_core_parser *parser = pipeline->parser;
    markdown_core_node *restart_node = plan->restart_node;

    // A sentinel restart that resolves to the boundary node is never
    // replaced, so its reserved prefix children could not be transplanted.
    if (restart_node && restart_node != plan->boundary_node && restart_node->type == MARKDOWN_CORE_NODE_PARAGRAPH &&
        !restart_node->extension && restart_node->first_child &&
        (restart_node->flags & MARKDOWN_CORE_NODE__SEALED_RELATIVE) && parser->root && parser->root->first_child &&
        parser->root->first_child->type == MARKDOWN_CORE_NODE_PARAGRAPH && !parser->root->first_child->extension &&
        parser->root->first_child->start_line != 0 &&
        parser->root->first_child->start_column == restart_node->start_column &&
        parser->root->first_child->internal_offset == restart_node->internal_offset &&
        markdown_core_node_owns_inlines(parser->root->first_child)) {
        markdown_core_node *staged_leaf = parser->root->first_child;
        markdown_core_bufsize seam;
        seam = markdown_core_inline_seam_prefix(
            parser,
            (const unsigned char *)restart_node->content.ptr,
            (markdown_core_bufsize)restart_node->content.size,
            (const unsigned char *)staged_leaf->content.ptr,
            (markdown_core_bufsize)staged_leaf->content.size,
            parser->options
        );
        if (seam > 0) {
            staged_leaf->user_data = (void *)(uintptr_t)((size_t)seam + 1);
        }
    }
}

static bool incremental_refine_and_preflight(incremental_pipeline *pipeline) {
    markdown_core_session *session = pipeline->session;
    markdown_core_map *map = pipeline->map;
    markdown_core_parser *parser = pipeline->parser;
    size_t phase_expansion;
    bool parse_lost;
    bool domain_replaced = false;
    bool internal_error;
    size_t i;

    incremental_arm_inline_seam(pipeline);

    // Unlimited budget: the estimate check below proves a one-shot parse
    // stays within its own budget, so no lookup can be denied in either.
    map->ref_size = 0;
    map->max_ref_size = (size_t)-1;
    if (session->record_lookups) {
        map->lookup_sink = markdown_core_lookup_recording_sink;
        map->lookup_context = &pipeline->recording;
    }
    pipeline->root = markdown_core_parser_refine_blocks(parser);
    if (pipeline->root && pipeline->dependent_count) {
        for (i = 0; i < pipeline->dependent_count; i++) {
            markdown_core_node *staged = pipeline->dependents[i].staged;
            markdown_core_node *refined = markdown_core_parser_refine_unit(parser, map, staged);
            pipeline->dependents[i].staged = refined;
            if (refined != staged) {
                // A complete ownership-domain rebuild keeps its semantic
                // shell stable. A core postprocessor that promotes the shell
                // names a genuinely different semantic operation, so retry
                // through the full-tree path instead of transplanting a
                // partial representation.
                domain_replaced = true;
                break;
            }
            if (!dependent_domain_shape_is_valid(&pipeline->dependents[i])) {
                parser->internal_error = true;
                break;
            }
        }
    }
    map->lookup_sink = NULL;
    map->lookup_context = NULL;
    map->lookup_unit = NULL;

    phase_expansion = map->ref_size;
    internal_error = parser->internal_error;
    parse_lost = parser->oom || internal_error || map->oom || pipeline->recording.lost;
    map->max_ref_size = pipeline->budget;
    parser->refmap = pipeline->own_map;
    pipeline->own_map = NULL;
    markdown_core_session_release_parser(session, parser);
    pipeline->parser = NULL;
    if (internal_error && pipeline->error && !*pipeline->error) {
        markdown_core_ast_set_error(
            pipeline->error,
            MARKDOWN_CORE_ERROR_INTERNAL,
            "inline ownership-domain refinement violated an extension invariant"
        );
    }
    if (!pipeline->root || parse_lost) {
        return false;
    }
    if (domain_replaced) {
        return incremental_use_fallback(pipeline);
    }
    // The budget shrinks with the text, so the estimate may already sit above
    // it; both cases mean a one-shot parse could deny lookups this phase
    // resolved, and only a full reparse settles that.
    if (session->expansion_estimate > pipeline->budget ||
        phase_expansion > pipeline->budget - session->expansion_estimate) {
        return incremental_use_fallback(pipeline);
    }
    session->expansion_estimate += phase_expansion; // a failed later step keeps the safe upper bound

    // The seal walks double as the node count for the id reservation below:
    // the parse root's count includes the root holder itself, and the
    // dependents contribute only their replacement descendants: their staged
    // semantic shells remain temporary and never receive session identity.
    pipeline->sealed_nodes = markdown_core_session_seal_positions(pipeline->root) - 1;
    for (i = 0; i < pipeline->dependent_count; i++) {
        pipeline->sealed_nodes += markdown_core_session_seal_positions(pipeline->dependents[i].staged) - 1;
    }

    // Collect and intern staged footnote sites while failure is still free.
    // Classification waits until adoption has fixed node ids.
    if (session->options.footnotes &&
        (!markdown_core_footnote_collect_sites(
             pipeline->mem,
             pipeline->root,
             NULL,
             &pipeline->staged_defs,
             &pipeline->staged_refs
         ) ||
         !markdown_core_session_footnote_label_sites(session, &pipeline->staged_defs, &pipeline->staged_refs))) {
        return false;
    }
    return true;
}

static void incremental_restore_graveyard(incremental_pipeline *pipeline) {
    incremental_splice_state *splice = &pipeline->splice;
    if (splice->graveyard_detached) {
        insert_child_chain(
            pipeline->doc,
            splice->prefix_tail,
            splice->suffix_head,
            splice->first_stale,
            splice->last_stale
        );
        splice->graveyard_detached = false;
    }
}

static bool incremental_collect_dependent_footnotes(incremental_pipeline *pipeline) {
    size_t i;

    if (!pipeline->session->options.footnotes) {
        return true;
    }
    for (i = 0; i < pipeline->dependent_count; i++) {
        dependent_unit *dependent = &pipeline->dependents[i];
        markdown_core_footnote_site_list definitions = {NULL, 0, 0};
        markdown_core_node *anchor = dependent->unit;
        size_t r;
        bool collected;

        while (anchor->parent && anchor->parent != pipeline->doc) {
            anchor = anchor->parent;
        }
        if (anchor->parent != pipeline->doc) {
            if (pipeline->error && !*pipeline->error) {
                markdown_core_ast_set_error(
                    pipeline->error,
                    MARKDOWN_CORE_ERROR_INTERNAL,
                    "inline ownership-domain owner is detached from its document"
                );
            }
            return false;
        }
        collected = markdown_core_footnote_collect_sites(
            pipeline->mem,
            dependent->staged,
            anchor,
            &definitions,
            &dependent->staged_refs
        );
        if (!collected) {
            markdown_core_footnote_site_list_release(pipeline->mem, &definitions);
            return false;
        }
        if (definitions.count) {
            markdown_core_footnote_site_list_release(pipeline->mem, &definitions);
            if (pipeline->error && !*pipeline->error) {
                markdown_core_ast_set_error(
                    pipeline->error,
                    MARKDOWN_CORE_ERROR_INTERNAL,
                    "an inline ownership domain produced a block footnote definition"
                );
            }
            return false;
        }
        markdown_core_footnote_site_list_release(pipeline->mem, &definitions);
        for (r = 0; r < dependent->staged_refs.count; r++) {
            dependent->staged_refs.items[r].unit = dependent->unit;
        }
        {
            markdown_core_footnote_site_list no_definitions = {NULL, 0, 0};
            if (!markdown_core_session_footnote_label_sites(
                    pipeline->session,
                    &no_definitions,
                    &dependent->staged_refs
                )) {
                return false;
            }
        }
    }
    return true;
}

static bool incremental_adopt(incremental_pipeline *pipeline) {
    incremental_restart_plan *plan = &pipeline->plan;
    incremental_splice_state *splice = &pipeline->splice;
    markdown_core_session *session = pipeline->session;
    markdown_core_node *sibling;
    bool staged_ok;
    size_t i;

    splice->first_stale = plan->restart_node == plan->boundary_node ? NULL : plan->restart_node;
    splice->suffix_head = plan->boundary_node;
    splice->prefix_clean = plan->restart_pos >= 0 ? (size_t)plan->restart_pos : 0;
    splice->boundary_idx = plan->boundary_pos >= 0 ? (size_t)plan->boundary_pos : session->clean.count;
    splice->suffix_clean = session->clean.count - splice->boundary_idx;

    for (sibling = pipeline->root->first_child; sibling; sibling = sibling->next) {
        if (sibling->flags & MARKDOWN_CORE_NODE__CLEAN_START) {
            splice->staged_clean++;
        }
    }
    splice->clean_count = splice->prefix_clean + pipeline->sentinel_count + splice->staged_clean + splice->suffix_clean;

    if (!markdown_core_session_ids_reserve(session, pipeline->sealed_nodes)) {
        return false;
    }
    // The clean index updates in place after the point of no return, so any
    // growth it needs happens here, while failing is still free.
    if (splice->clean_count > session->clean.capacity) {
        markdown_core_clean_child *grown = (markdown_core_clean_child *)pipeline->mem->realloc(
            pipeline->mem,
            session->clean.items,
            splice->clean_count * sizeof(*session->clean.items)
        );
        if (!grown) {
            return false;
        }
        session->clean.items = grown;
        session->clean.capacity = splice->clean_count;
    }

    // Detach the graveyard. Its nodes deliberately keep `doc` parent stamps;
    // the saved gap is the exact inverse until the point of no return.
    if (splice->first_stale) {
        splice->last_stale = splice->suffix_head ? splice->suffix_head->prev : pipeline->doc->last_child;
        splice->prefix_tail = splice->first_stale->prev;
        detach_child_chain(pipeline->doc, splice->first_stale, splice->last_stale);
        splice->graveyard_detached = true;
    } else {
        splice->prefix_tail = splice->suffix_head ? splice->suffix_head->prev : pipeline->doc->last_child;
    }

    // A stack dummy document fronts the graveyard so the standard adoption
    // machine classifies the real document through the staged root.
    {
        markdown_core_node dummy;
        memset(&dummy, 0, sizeof(dummy));
        dummy.type = (uint16_t)MARKDOWN_CORE_NODE_DOCUMENT;
        dummy.id = pipeline->doc->id;
        dummy.last_changed_rev = pipeline->doc->last_changed_rev;
        dummy.first_child = splice->first_stale;
        dummy.last_child = splice->last_stale;
        staged_ok = markdown_core_session_adopt(session, &dummy, pipeline->root, pipeline->new_rev, pipeline->changes);
    }
    for (i = 0; i < pipeline->dependent_count && staged_ok; i++) {
        dependent_unit *dep = &pipeline->dependents[i];
        staged_ok = markdown_core_session_adopt_inline_domain(
            session,
            dep->unit,
            dep->staged,
            pipeline->new_rev,
            pipeline->changes,
            &dep->next_revision
        );
        dep->changed = dep->next_revision == pipeline->new_rev;
    }
    if (staged_ok) {
        staged_ok = incremental_collect_dependent_footnotes(pipeline);
    }
    if (staged_ok) {
        staged_ok =
            markdown_core_lookup_recording_bundle(&pipeline->recording, &pipeline->bundles, &pipeline->bundle_count) &&
            markdown_core_lookup_table_reserve(pipeline->mem, &session->lookups, pipeline->bundle_count) &&
            markdown_core_lookup_postings_reserve(
                pipeline->mem,
                &session->lookups,
                pipeline->bundles,
                pipeline->bundle_count
            );
    }
    for (i = 0; i < pipeline->dependent_count && staged_ok; i++) {
        markdown_core_node *ancestor;
        if (!pipeline->dependents[i].changed) {
            continue;
        }
        for (ancestor = pipeline->dependents[i].unit->parent; ancestor && staged_ok; ancestor = ancestor->parent) {
            size_t k;
            bool collected = false;
            if (ancestor == pipeline->doc && pipeline->root->last_changed_rev == pipeline->new_rev) {
                break; // the dummy verdict already bumps the document
            }
            for (k = 0; k < pipeline->bubble_count && !collected; k++) {
                collected = pipeline->bubble_nodes[k].node == ancestor;
            }
            if (collected) {
                break; // and with it every ancestor above
            }
            if (pipeline->bubble_count == pipeline->bubble_capacity) {
                size_t grown_capacity = pipeline->bubble_capacity ? pipeline->bubble_capacity * 2 : 8;
                bubble_ancestor *grown =
                    (bubble_ancestor *)
                        pipeline->mem->realloc(pipeline->mem, pipeline->bubble_nodes, grown_capacity * sizeof(*grown));
                if (!grown) {
                    staged_ok = false;
                    break;
                }
                pipeline->bubble_nodes = grown;
                pipeline->bubble_capacity = grown_capacity;
            }
            pipeline->bubble_nodes[pipeline->bubble_count].node = ancestor;
            pipeline->bubble_nodes[pipeline->bubble_count].previous_rev = ancestor->last_changed_rev;
            pipeline->bubble_count++;
        }
    }
    if (staged_ok && pipeline->changes && pipeline->bubble_count) {
        staged_ok = markdown_core_id_array_reserve(&pipeline->changes->bubbled, pipeline->bubble_count);
    }
    if (!staged_ok) {
        incremental_restore_graveyard(pipeline);
        return false;
    }
    return true;
}

static void incremental_install_staged(incremental_pipeline *pipeline) {
    incremental_splice_state *splice = &pipeline->splice;
    size_t i;

    splice->staged_first = pipeline->root->first_child;
    splice->staged_last = pipeline->root->last_child;
    if (splice->staged_first) {
        detach_child_chain(pipeline->root, splice->staged_first, splice->staged_last);
        insert_child_chain(
            pipeline->doc,
            splice->prefix_tail,
            splice->suffix_head,
            splice->staged_first,
            splice->staged_last
        );
        splice->staged_installed = true;
    }

    // Semantic owners stay in place; only their complete inline ownership
    // domains move. The staged shells remain parked under `holder` and now
    // own the graveyard domains, ready for either rollback or final release.
    for (i = 0; i < pipeline->dependent_count; i++) {
        swap_dependent_domain(&pipeline->dependents[i]);
    }
    splice->dependents_installed = pipeline->dependent_count != 0;
}

static void incremental_stamp_revisions(incremental_pipeline *pipeline) {
    size_t i;

    pipeline->previous_doc_rev = pipeline->doc->last_changed_rev;
    pipeline->doc->last_changed_rev = pipeline->root->last_changed_rev;
    for (i = 0; i < pipeline->dependent_count; i++) {
        pipeline->dependents[i].unit->last_changed_rev = pipeline->dependents[i].next_revision;
    }
    for (i = 0; i < pipeline->bubble_count; i++) {
        pipeline->bubble_nodes[i].node->last_changed_rev = pipeline->new_rev;
    }
    pipeline->splice.revisions_stamped = true;
}

static void incremental_rollback_splice(incremental_pipeline *pipeline) {
    incremental_splice_state *splice = &pipeline->splice;
    size_t i;

    if (splice->revisions_stamped) {
        pipeline->doc->last_changed_rev = pipeline->previous_doc_rev;
        for (i = 0; i < pipeline->dependent_count; i++) {
            pipeline->dependents[i].unit->last_changed_rev = pipeline->dependents[i].previous_revision;
        }
        for (i = 0; i < pipeline->bubble_count; i++) {
            pipeline->bubble_nodes[i].node->last_changed_rev = pipeline->bubble_nodes[i].previous_rev;
        }
        splice->revisions_stamped = false;
    }

    // The ownership-domain exchange is self-inverse. Staged semantic shells
    // remain parked throughout, so cleanup ownership never changes.
    if (splice->dependents_installed) {
        for (i = 0; i < pipeline->dependent_count; i++) {
            swap_dependent_domain(&pipeline->dependents[i]);
        }
        splice->dependents_installed = false;
    }

    if (splice->staged_installed) {
        detach_child_chain(pipeline->doc, splice->staged_first, splice->staged_last);
        free_child_chain(splice->staged_first);
        splice->staged_installed = false;
    }
    incremental_restore_graveyard(pipeline);

    // A fallback re-records against the full path and a failure reports no
    // partial adoption/footnote delta.
    if (pipeline->changes) {
        pipeline->changes->added.count = 0;
        pipeline->changes->removed.count = 0;
        pipeline->changes->changed.count = 0;
        pipeline->changes->bubbled.count = 0;
    }
}

static bool incremental_install_and_refresh(incremental_pipeline *pipeline) {
    incremental_restart_plan *plan = &pipeline->plan;
    markdown_core_incremental_result refreshed;

    incremental_install_staged(pipeline);

    // Stamp before the footnote diff: its ancestor climb must see every node
    // this commit already classified, or changed/bubbled would overlap.
    incremental_stamp_revisions(pipeline);

    if (!pipeline->session->options.footnotes) {
        return true;
    }
    refreshed = footnote_refresh(
        pipeline->session,
        &pipeline->staged_defs,
        &pipeline->staged_refs,
        pipeline->stale_ids,
        pipeline->stale_count,
        plan->restart_line,
        plan->boundary_line,
        pipeline->dependents,
        pipeline->dependent_count,
        pipeline->new_rev,
        pipeline->changes,
        &pipeline->footnotes,
        &pipeline->footnotes_built
    );
    if (refreshed != MARKDOWN_CORE_INCREMENTAL_COMMITTED) {
        pipeline->result = refreshed;
        incremental_rollback_splice(pipeline);
        return false;
    }
    return true;
}

static void incremental_finalize_geometry(incremental_pipeline *pipeline) {
    incremental_restart_plan *plan = &pipeline->plan;
    incremental_splice_state *splice = &pipeline->splice;
    markdown_core_node *sibling;

    if (plan->boundary_pos >= 0) {
        splice->delta_lines =
            (plan->restart_line + plan->fed_lines) - pipeline->session->clean.items[plan->boundary_pos].start_line;
        plan->total_lines = pipeline->session->total_lines + splice->delta_lines;
        plan->last_line_length = pipeline->session->last_line_length;
        if (splice->delta_lines != 0) {
            for (sibling = splice->suffix_head; sibling; sibling = sibling->next) {
                // Position-free roots keep their raw zeros (see the seal).
                if (sibling->flags & MARKDOWN_CORE_NODE__SEALED_RELATIVE) {
                    sibling->start_line += splice->delta_lines;
                }
            }
        }

        // Re-date transplanted blocks that closed inside the line before the
        // boundary. They sit on the chain of sealed block children sharing
        // their parent's start line, so a pruned walk reaches them all.
        {
            markdown_core_node *node = splice->suffix_head;
            while (node) {
                markdown_core_node *step = NULL;
                markdown_core_node *probe;
                if (node->end_line == -1 && (node->flags & MARKDOWN_CORE_NODE__SEALED_RELATIVE) &&
                    MARKDOWN_CORE_NODE_BLOCK_P(node)) {
                    node->end_column = plan->staged_tail_length;
                }
                for (probe = node->first_child; probe; probe = probe->next) {
                    if ((probe->flags & MARKDOWN_CORE_NODE__SEALED_RELATIVE) && probe->start_line == 0 &&
                        MARKDOWN_CORE_NODE_BLOCK_P(probe)) {
                        step = probe;
                        break;
                    }
                }
                while (!step && node != splice->suffix_head) {
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
    pipeline->doc->end_line = plan->total_lines - 1;
    pipeline->doc->end_column = plan->last_line_length;

    // At-rest definitions beyond the boundary follow the suffix line shift.
    if (plan->boundary_pos >= 0 && splice->delta_lines != 0) {
        size_t start =
            pipeline->reconcile.applied
                ? pipeline->reconcile.splice_lo + pipeline->new_defs.count
                : def_lower_bound(pipeline->session->def_index, pipeline->session->def_count, plan->boundary_line);
        size_t at;
        for (at = start; at < pipeline->session->def_count; at++) {
            pipeline->session->def_index[at]->start_line += splice->delta_lines;
        }
    }
}

static void incremental_finalize_definitions(incremental_pipeline *pipeline) {
    size_t i;
    uint64_t head_owner = pipeline->splice.prefix_tail ? pipeline->splice.prefix_tail->id : 0;

    // Equal sequences keep the old entries and take over the staged anchors
    // and geometry. Reconciled sequences keep staged entries and turn their
    // pointer-stamped anchors into adopted ids.
    if (pipeline->defs_equal) {
        for (i = 0; i < pipeline->old_defs.count; i++) {
            uint64_t anchor = pipeline->new_defs.items[i]->entry.owner;
            pipeline->old_defs.items[i]->entry.owner =
                anchor == 0 ? head_owner : ((const markdown_core_node *)(uintptr_t)anchor)->id;
            pipeline->old_defs.items[i]->entry.start_line = pipeline->new_defs.items[i]->entry.start_line;
            pipeline->old_defs.items[i]->entry.from_vanished_clean =
                pipeline->new_defs.items[i]->entry.from_vanished_clean;
        }
        markdown_core_map_remove_until(pipeline->map, pipeline->previous_head);
    } else {
        for (i = 0; i < pipeline->new_defs.count; i++) {
            uint64_t anchor = pipeline->new_defs.items[i]->entry.owner;
            pipeline->new_defs.items[i]->entry.owner =
                anchor == 0 ? head_owner : ((const markdown_core_node *)(uintptr_t)anchor)->id;
        }
    }
}

static void incremental_transplant_inline_seam(incremental_pipeline *pipeline) {
    incremental_splice_state *splice = &pipeline->splice;

    // Reserved prefix children move from the replaced leaf into its staged
    // successor ahead of the reparsed suffix.
    if (splice->staged_first && splice->staged_first->user_data && splice->first_stale &&
        splice->first_stale == pipeline->plan.restart_node) {
        markdown_core_bufsize seam = (markdown_core_bufsize)((uintptr_t)splice->staged_first->user_data - 1);
        size_t reserved = 0;
        markdown_core_bufsize b;
        for (b = 0; b < seam; b++) {
            if (splice->staged_first->content.ptr[b] == '\n') {
                reserved += 2;
            }
        }
        if (reserved) {
            markdown_core_node *head = splice->first_stale->first_child;
            markdown_core_node *tail = head;
            markdown_core_node *walk;
            size_t k;
            for (k = 1; k < reserved; k++) {
                tail = tail->next;
            }
            splice->first_stale->first_child = tail->next;
            if (tail->next) {
                tail->next->prev = NULL;
            } else {
                splice->first_stale->last_child = NULL;
            }
            tail->next = NULL;
            for (walk = head; walk; walk = walk->next) {
                walk->parent = splice->staged_first;
                // Rebase borrowed text chunks only when they point inside the
                // old parent buffer; static-token chunks stay untouched.
                if (walk->type == MARKDOWN_CORE_NODE_TEXT && walk->as.literal.alloc == 0 && walk->as.literal.data) {
                    uintptr_t data = (uintptr_t)walk->as.literal.data;
                    uintptr_t lo = (uintptr_t)splice->first_stale->content.ptr;
                    uintptr_t hi = lo + splice->first_stale->content.size;
                    if (data >= lo && data + walk->as.literal.len <= hi) {
                        walk->as.literal.data = splice->staged_first->content.ptr + (data - lo);
                    }
                }
            }
            if (splice->staged_first->first_child) {
                tail->next = splice->staged_first->first_child;
                splice->staged_first->first_child->prev = tail;
            } else {
                splice->staged_first->last_child = tail;
            }
            splice->staged_first->first_child = head;
            head->prev = NULL;
        }
        splice->staged_first->user_data = NULL;
    } else if (splice->staged_first && splice->staged_first->user_data) {
        // A committed node must never retain the seam integer as user_data.
        splice->staged_first->user_data = NULL;
    }
}

static void incremental_finalize_identity_indexes(incremental_pipeline *pipeline) {
    incremental_splice_state *splice = &pipeline->splice;
    markdown_core_session *session = pipeline->session;
    size_t i;

    // Repoint adopted ids for the staged document region and each newly live
    // ownership domain, then drop entries still targeting either graveyard.
    ids_put_chain(session, splice->staged_first, splice->suffix_head);
    for (i = 0; i < pipeline->dependent_count; i++) {
        dependent_unit *dependent = &pipeline->dependents[i];
        ids_put_chain(session, dependent->unit->first_child, NULL);
    }
    ids_remove_stale_chain(session, splice->first_stale);
    for (i = 0; i < pipeline->dependent_count; i++) {
        ids_remove_stale_chain(session, pipeline->dependents[i].staged->first_child);
    }

    // Replace stale lookup records with the recording bundles from this
    // commit, transferring each record's ownership into the table.
    lookups_remove_chain(session, splice->first_stale);
    for (i = 0; i < pipeline->dependent_count; i++) {
        markdown_core_lookup_table_remove(pipeline->mem, &session->lookups, pipeline->dependents[i].unit->id);
    }
    for (i = 0; i < pipeline->bundle_count; i++) {
        markdown_core_lookup_table_put(
            pipeline->mem,
            &session->lookups,
            pipeline->bundles[i].unit->id,
            pipeline->bundles[i].record
        );
        pipeline->bundles[i].record.labels = NULL;
        pipeline->bundles[i].record.positions = NULL;
        pipeline->bundles[i].record.count = 0;
    }

    // Revision stamps were installed before footnote diff; append only their
    // pre-reserved delta ids here, preserving the established order.
    if (pipeline->changes) {
        for (i = 0; i < pipeline->bubble_count; i++) {
            bool pushed;
            assert(pipeline->changes->bubbled.count < pipeline->changes->bubbled.capacity);
            pushed = markdown_core_id_array_push(&pipeline->changes->bubbled, pipeline->bubble_nodes[i].node->id);
            assert(pushed);
            (void)pushed;
        }
    }

    if (pipeline->footnotes_built) {
        markdown_core_footnote_index_release(pipeline->mem, &session->footnotes);
        session->footnotes = pipeline->footnotes;
        memset(&pipeline->footnotes, 0, sizeof(pipeline->footnotes));
        pipeline->footnotes_built = false;
    }
}

static void incremental_finalize_clean_index(incremental_pipeline *pipeline) {
    incremental_restart_plan *plan = &pipeline->plan;
    incremental_splice_state *splice = &pipeline->splice;
    markdown_core_clean_child *items = pipeline->session->clean.items;
    ptrdiff_t index_shift = (ptrdiff_t)(splice->prefix_clean + pipeline->sentinel_count + splice->staged_clean) -
                            (ptrdiff_t)splice->boundary_idx;
    size_t filled;
    size_t i;
    markdown_core_node *sibling;

    // Move the suffix before writing the middle because a growing staged run
    // overlaps the suffix's old slots.
    if (splice->suffix_clean && (index_shift != 0 || pipeline->pending.delta != 0 || splice->delta_lines != 0)) {
        if (index_shift <= 0) {
            for (i = 0; i < splice->suffix_clean; i++) {
                markdown_core_clean_child entry = items[splice->boundary_idx + i];
                entry.start_byte = (size_t)((ptrdiff_t)entry.start_byte + pipeline->pending.delta);
                entry.start_line += splice->delta_lines;
                items[(size_t)((ptrdiff_t)(splice->boundary_idx + i) + index_shift)] = entry;
            }
        } else {
            for (i = splice->suffix_clean; i-- > 0;) {
                markdown_core_clean_child entry = items[splice->boundary_idx + i];
                entry.start_byte = (size_t)((ptrdiff_t)entry.start_byte + pipeline->pending.delta);
                entry.start_line += splice->delta_lines;
                items[(size_t)((ptrdiff_t)(splice->boundary_idx + i) + index_shift)] = entry;
            }
        }
    }

    filled = splice->prefix_clean;
    for (i = 0; i < pipeline->sentinel_count; i++) {
        items[filled].start_byte = pipeline->line_offsets.items[pipeline->sentinel_lines[i] - plan->restart_line];
        items[filled].start_line = pipeline->sentinel_lines[i];
        items[filled].node = NULL;
        filled++;
    }
    for (sibling = splice->staged_first; sibling && sibling != splice->suffix_head; sibling = sibling->next) {
        if (sibling->flags & MARKDOWN_CORE_NODE__CLEAN_START) {
            int abs_line = sibling->start_line + 1;
            items[filled].start_byte = pipeline->line_offsets.items[abs_line - plan->restart_line];
            items[filled].start_line = abs_line;
            items[filled].node = sibling;
            filled++;
        }
    }
    pipeline->session->clean.count = splice->clean_count;
}

static void incremental_finalize_commit(incremental_pipeline *pipeline) {
    incremental_splice_state *splice = &pipeline->splice;
    markdown_core_session *session = pipeline->session;
    size_t i;

    assert(splice->graveyard_detached == (splice->first_stale != NULL));
    assert(splice->staged_installed == (splice->staged_first != NULL));
    assert(splice->dependents_installed == (pipeline->dependent_count != 0));
    assert(splice->revisions_stamped);

    // Point of no return: every remaining operation uses pre-reserved storage
    // and must stay infallible.
    incremental_finalize_geometry(pipeline);
    incremental_finalize_definitions(pipeline);
    incremental_transplant_inline_seam(pipeline);
    incremental_finalize_identity_indexes(pipeline);
    incremental_finalize_clean_index(pipeline);

    session->total_lines = pipeline->plan.total_lines;
    session->last_line_length = pipeline->plan.last_line_length;
    session->revision = pipeline->new_rev;
    session->restarted_commits++;
    if (pipeline->plan.boundary_pos >= 0) {
        session->reflowed_commits++;
    }
    session->pending.dirty = false;
    session->pending.new_lo = 0;
    session->pending.new_hi = 0;
    session->pending.delta = 0;

    free_child_chain(splice->first_stale);
    splice->graveyard_detached = false;
    for (i = 0; i < pipeline->dependent_count; i++) {
        pipeline->dependents[i].installed = false;
    }
    splice->dependents_installed = false;
    splice->staged_installed = false;
    splice->revisions_stamped = false;
    markdown_core_node_free(pipeline->root);
    pipeline->root = NULL;
}

static void incremental_abort(incremental_pipeline *pipeline) {
    if (pipeline->splice.graveyard_detached || pipeline->splice.staged_installed ||
        pipeline->splice.dependents_installed || pipeline->splice.revisions_stamped) {
        incremental_rollback_splice(pipeline);
    }
    assert(!pipeline->splice.graveyard_detached);
    assert(!pipeline->splice.staged_installed);
    assert(!pipeline->splice.dependents_installed);
    assert(!pipeline->splice.revisions_stamped);

    if (pipeline->reconcile.applied) {
        // The map was reconciled in place and cannot be reconstructed from
        // the released stale entries. Keep the committed tree valid and force
        // the next commit through the wholesale full-path rebuild.
        pipeline->session->refmap_stale = true;
    } else {
        // Pointer-stamped staged definitions never survive an abandoned
        // pipeline.
        markdown_core_map_remove_until(pipeline->map, pipeline->previous_head);
    }
    if (pipeline->parser) {
        pipeline->parser->refmap = pipeline->own_map;
        markdown_core_session_release_parser(pipeline->session, pipeline->parser);
        pipeline->parser = NULL;
        pipeline->own_map = NULL;
    }
    if (pipeline->root) {
        markdown_core_node_free(pipeline->root);
        pipeline->root = NULL;
    }
    if (pipeline->result == MARKDOWN_CORE_INCREMENTAL_FAILED && pipeline->error && !*pipeline->error) {
        markdown_core_ast_set_error(
            pipeline->error,
            MARKDOWN_CORE_ERROR_ALLOCATION_FAILED,
            "could not commit the session incrementally"
        );
    }
}

static void incremental_pipeline_release(incremental_pipeline *pipeline) {
    assert(!pipeline->parser);
    assert(!pipeline->own_map);
    assert(!pipeline->root);
    assert(!pipeline->footnotes_built);
    assert(!pipeline->splice.graveyard_detached);
    assert(!pipeline->splice.staged_installed);
    assert(!pipeline->splice.dependents_installed);
    assert(!pipeline->splice.revisions_stamped);

    pipeline->map->lookup_sink = NULL;
    pipeline->map->lookup_context = NULL;
    pipeline->map->lookup_unit = NULL;
    markdown_core_footnote_site_list_release(pipeline->mem, &pipeline->staged_defs);
    markdown_core_footnote_site_list_release(pipeline->mem, &pipeline->staged_refs);
    markdown_core_lookup_recording_release(&pipeline->recording);
    markdown_core_unit_lookups_free(pipeline->mem, pipeline->bundles, pipeline->bundle_count);
    reconcile_release(pipeline->mem, &pipeline->reconcile);
    if (pipeline->dependents) {
        size_t i;
        for (i = 0; i < pipeline->dependent_count; i++) {
            assert(!pipeline->dependents[i].installed);
            markdown_core_footnote_site_list_release(pipeline->mem, &pipeline->dependents[i].staged_refs);
        }
    }
    if (pipeline->holder) {
        markdown_core_node_free(pipeline->holder);
    }
    if (pipeline->dependents) {
        pipeline->mem->free(pipeline->mem, pipeline->dependents);
    }
    if (pipeline->bubble_nodes) {
        pipeline->mem->free(pipeline->mem, pipeline->bubble_nodes);
    }
    if (pipeline->sentinel_lines) {
        pipeline->mem->free(pipeline->mem, pipeline->sentinel_lines);
    }
    if (pipeline->line_offsets.items) {
        pipeline->mem->free(pipeline->mem, pipeline->line_offsets.items);
    }
    if (pipeline->new_defs.items) {
        pipeline->mem->free(pipeline->mem, pipeline->new_defs.items);
    }
    if (pipeline->old_defs.items) {
        pipeline->mem->free(pipeline->mem, pipeline->old_defs.items);
    }
    if (pipeline->stale_ids) {
        pipeline->mem->free(pipeline->mem, pipeline->stale_ids);
    }
}

// --- the pipeline --------------------------------------------------------------

markdown_core_incremental_result markdown_core_session_commit_incremental(
    markdown_core_session *session,
    uint64_t new_rev,
    markdown_core_delta *changes,
    markdown_core_error **error
) {
    incremental_pipeline pipeline;

    incremental_pipeline_init(&pipeline, session, new_rev, changes, error);
    if (!incremental_plan_restart(&pipeline) || !incremental_reparse_blocks(&pipeline) ||
        !incremental_prepare_definitions(&pipeline) || !incremental_refine_and_preflight(&pipeline) ||
        !incremental_adopt(&pipeline) || !incremental_install_and_refresh(&pipeline)) {
        incremental_abort(&pipeline);
    } else {
        incremental_finalize_commit(&pipeline);
        pipeline.result = MARKDOWN_CORE_INCREMENTAL_COMMITTED;
    }
    incremental_pipeline_release(&pipeline);
    return pipeline.result;
}
