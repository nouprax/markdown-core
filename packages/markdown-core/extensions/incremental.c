#include <stdlib.h>
#include <string.h>

#include "session_internal.h"

#include "directive.h"

#include <node.h>
#include <parser.h>
#include <references.h>

// Incremental commit pipeline (design step order in
// docs/migration/2026-07-15-v2-incremental-sessions-plan.md):
//
//   1. Restart plan  — the coalesced edit summary picks the restart point:
//                      the last CLEAN_START document child at or before the
//                      first edited byte (byte 0 when none). Children before
//                      it are untouched prefix; it and everything after it
//                      until resync are the stale region.
//   2. Staged reparse — the stale bytes feed line by line through a fresh
//                      parser sharing the session's reference map. Once past
//                      the last edited byte, each clean line boundary that
//                      maps onto an old CLEAN_START child resyncs: the old
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

typedef struct {
    size_t *items;
    size_t count;
    size_t capacity;
} offset_list;

static bool offset_push(markdown_core_mem *mem, offset_list *list, size_t offset) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 64;
        size_t *grown = (size_t *)mem->realloc(list->items, capacity * sizeof(*grown));
        if (!grown) {
            return false;
        }
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count++] = offset;
    return true;
}

bool markdown_core_session_index_clean_children(markdown_core_session *session, markdown_core_node *root,
                                                markdown_core_clean_index *out) {
    const unsigned char *bytes = markdown_core_text_bytes(&session->text);
    size_t length = markdown_core_text_length(&session->text);
    markdown_core_node *child;
    size_t count = 0;
    size_t offset = 0;
    int line = 1;

    for (child = root->first_child; child; child = child->next) {
        if (child->flags & MARKDOWN_CORE_NODE__CLEAN_START) {
            count++;
        }
    }
    out->items = (markdown_core_clean_child *)session->mem->calloc(count ? count : 1, sizeof(*out->items));
    if (!out->items) {
        return false;
    }
    out->capacity = count ? count : 1;
    out->count = 0;

    // Clean children carry strictly increasing parser line numbers, so one
    // forward scan of the text resolves every start byte.
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
            (markdown_core_reference **)mem->realloc(list->items, capacity * sizeof(*grown));
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
static bool collect_new_definitions(markdown_core_mem *mem, markdown_core_map *map,
                                    const markdown_core_map_entry *previous_head, reference_list *out) {
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

static int order_compare(const void *a, const void *b) {
    const markdown_core_reference *ra = *(const markdown_core_reference *const *)a;
    const markdown_core_reference *rb = *(const markdown_core_reference *const *)b;
    if (ra->entry.order != rb->entry.order) {
        return ra->entry.order < rb->entry.order ? -1 : 1;
    }
    return 0;
}

/* Entries anchored in the stale region, in document order. At rest every
 * entry order stems from the most recent full parse, so sorting by order is
 * sorting by document position. */
static bool collect_stale_definitions(markdown_core_mem *mem, markdown_core_map *map,
                                      const markdown_core_map_entry *previous_head, const uint64_t *stale_ids,
                                      size_t stale_count, bool include_head_region, reference_list *out) {
    markdown_core_map_entry *entry = map->refs;
    while (entry && entry != previous_head) {
        entry = entry->next; // skip this parse's own additions
    }
    for (; entry; entry = entry->next) {
        bool stale = entry->owner == 0 ? include_head_region : id_set_holds(stale_ids, stale_count, entry->owner);
        if (stale && !reference_push(mem, out, (markdown_core_reference *)entry)) {
            return false;
        }
    }
    if (out->count > 1) {
        qsort(out->items, out->count, sizeof(*out->items), order_compare);
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
    DEF_REGION_UNKNOWN,
} def_region;

/* Places an at-rest entry relative to the stale region. Anchors are direct
 * document children with sealed document-relative lines, so one comparison
 * against the restart line settles prefix versus suffix. */
static def_region classify_definition(markdown_core_session *session, const markdown_core_map_entry *entry,
                                      const uint64_t *stale_ids, size_t stale_count, bool include_head_region,
                                      int restart_line) {
    const markdown_core_node *anchor;
    if (entry->owner == 0) {
        return include_head_region ? DEF_REGION_STALE : DEF_REGION_PREFIX;
    }
    if (id_set_holds(stale_ids, stale_count, entry->owner)) {
        return DEF_REGION_STALE;
    }
    anchor = markdown_core_session_node_by_id(session, entry->owner);
    if (!anchor || anchor->parent != session->view.root) {
        return DEF_REGION_UNKNOWN;
    }
    return anchor->start_line + 1 < restart_line ? DEF_REGION_PREFIX : DEF_REGION_SUFFIX;
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
        mem->free(state->plans);
    }
    if (state->prefix_entries) {
        mem->free(state->prefix_entries);
    }
    if (state->suffix_entries) {
        mem->free(state->suffix_entries);
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
static bool reconcile_prepare(markdown_core_session *session, markdown_core_map *map, uint64_t order_floor,
                              const markdown_core_map_entry *previous_head, const uint64_t *stale_ids,
                              size_t stale_count, bool include_head_region, int restart_line,
                              const reference_list *old_defs, const reference_list *new_defs, reconcile_state *state,
                              bool *fallback) {
    markdown_core_mem *mem = session->mem;
    markdown_core_map_entry *entry;
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
    state->plans = (label_plan *)mem->calloc(affected_upper ? affected_upper : 1, sizeof(*state->plans));
    if (!state->plans) {
        reconcile_release(mem, state);
        return false;
    }

    // Classify every at-rest entry once: the prefix/suffix arrays feed the
    // renumber path, and the extrema bound the vacated order span.
    {
        size_t at_rest = 0;
        size_t prefix_filled = 0;
        size_t suffix_filled = 0;
        for (entry = map->refs; entry && entry != previous_head; entry = entry->next) {
        }
        for (; entry; entry = entry->next) {
            at_rest++;
        }
        state->prefix_entries =
            (markdown_core_reference **)mem->calloc(at_rest ? at_rest : 1, sizeof(*state->prefix_entries));
        state->suffix_entries =
            (markdown_core_reference **)mem->calloc(at_rest ? at_rest : 1, sizeof(*state->suffix_entries));
        if (!state->prefix_entries || !state->suffix_entries) {
            reconcile_release(mem, state);
            return false;
        }
        entry = map->refs;
        while (entry && entry != previous_head) {
            entry = entry->next;
        }
        for (; entry; entry = entry->next) {
            switch (classify_definition(session, entry, stale_ids, stale_count, include_head_region, restart_line)) {
            case DEF_REGION_PREFIX:
                state->prefix_entries[prefix_filled++] = (markdown_core_reference *)entry;
                if (entry->order > state->prefix_max_order) {
                    state->prefix_max_order = entry->order;
                }
                break;
            case DEF_REGION_SUFFIX:
                state->suffix_entries[suffix_filled++] = (markdown_core_reference *)entry;
                if (entry->order < state->suffix_min_order) {
                    state->suffix_min_order = entry->order;
                }
                break;
            case DEF_REGION_STALE:
                break;
            case DEF_REGION_UNKNOWN:
                reconcile_release(mem, state);
                *fallback = true;
                return false;
            }
        }
        state->prefix_count = prefix_filled;
        state->suffix_count = suffix_filled;
    }

    // The vacated span holds exactly the stale orders; more staged entries
    // than that means the whole map renumbers (rare, O(definitions)).
    {
        uint64_t span =
            state->suffix_min_order == UINT64_MAX ? UINT64_MAX : state->suffix_min_order - state->prefix_max_order - 1;
        state->renumber = span < new_defs->count;
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
                switch (
                    classify_definition(session, cursor, stale_ids, stale_count, include_head_region, restart_line)) {
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
static void reconcile_apply(markdown_core_session *session, markdown_core_map *map,
                            const markdown_core_map_entry *previous_head, const uint64_t *stale_ids, size_t stale_count,
                            bool include_head_region, int restart_line, const reference_list *new_defs,
                            reconcile_state *state) {
    markdown_core_map_entry **link = &map->refs;
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
    // The chain is newest-first, so everything before `previous_head` is a
    // staged entry of this parse — never at-rest, owners still pointer
    // stamps — and must not reach the classifier. A NULL `previous_head`
    // means the map held nothing at rest and nothing is removable.
    {
        bool at_rest = false;
        while (*link) {
            markdown_core_map_entry *entry = *link;
            if (entry == previous_head) {
                at_rest = true;
            }
            if (at_rest && classify_definition(session, entry, stale_ids, stale_count, include_head_region,
                                               restart_line) == DEF_REGION_STALE) {
                *link = entry->next;
                map->size--;
                map->free(map, entry);
                continue;
            }
            link = &entry->next;
        }
    }
}

// --- subtree walks ------------------------------------------------------------

static bool is_wrapper_node(const markdown_core_node *node) { return node->type == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL; }

/* Counts every node of every subtree in the sibling chain starting at
 * `chain` (wrappers included: a small over-reservation is harmless). */
static size_t chain_node_count(const markdown_core_node *chain) {
    size_t count = 0;
    const markdown_core_node *top;
    for (top = chain; top; top = top->next) {
        const markdown_core_node *node = top;
        for (;;) {
            count++;
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
    return count;
}

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
    clone->flags |= unit->flags & MARKDOWN_CORE_NODE__CLEAN_START;
    if (unit->type == MARKDOWN_CORE_NODE_HEADING) {
        clone->as.heading = unit->as.heading;
    }
    return clone;
}

/* Scans the session's lookup records for units that depend on a label whose
 * winner changed, skipping units the staged reparse rebuilds anyway. Returns
 * false with *fallback set when a dependent cannot be rebuilt per-unit (an
 * extension-owned unit, or a record that no longer matches the tree), and
 * false with *fallback clear on allocation loss. */
static bool collect_dependents(markdown_core_session *session, const markdown_core_key_index *dirty,
                               const uint64_t *stale_ids, size_t stale_count, dependent_unit **out, size_t *out_count,
                               bool *fallback) {
    markdown_core_mem *mem = session->mem;
    markdown_core_lookup_table *table = &session->lookups;
    markdown_core_node *doc = session->view.root;
    dependent_unit *dependents = NULL;
    size_t count = 0;
    size_t capacity = 0;
    size_t slot;

    *out = NULL;
    *out_count = 0;
    *fallback = false;
    if (dirty->size == 0) {
        return true;
    }
    for (slot = 0; slot < table->capacity; slot++) {
        const markdown_core_lookup_record *record = &table->records[slot];
        markdown_core_node *unit;
        markdown_core_node *top;
        size_t i;
        bool depends = false;

        if (table->keys[slot] == 0) {
            continue;
        }
        for (i = 0; i < record->count && !depends; i++) {
            depends = markdown_core_key_index_lookup(dirty, record->labels[i],
                                                     (bufsize_t)strlen((const char *)record->labels[i])) != NULL;
        }
        if (!depends) {
            continue;
        }
        unit = (markdown_core_node *)markdown_core_session_node_by_id(session, table->keys[slot]);
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
            dependent_unit *grown = (dependent_unit *)mem->realloc(dependents, grown_capacity * sizeof(*grown));
            if (!grown) {
                if (dependents) {
                    mem->free(dependents);
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
    *out = dependents;
    *out_count = count;
    return true;

fall_back:
    if (dependents) {
        mem->free(dependents);
    }
    *fallback = true;
    return false;
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

markdown_core_incremental_result markdown_core_session_commit_incremental(markdown_core_session *session,
                                                                          uint64_t new_rev,
                                                                          markdown_core_changeset *changes,
                                                                          markdown_core_error **error) {
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
    markdown_core_footnote_index footnotes = {NULL, 0, NULL, 0, NULL, NULL};
    bool footnotes_built = false;
    markdown_core_lookup_recording recording;
    markdown_core_unit_lookups *bundles = NULL;
    size_t bundle_count = 0;
    reconcile_state reconcile;
    bool defs_equal = true;
    dependent_unit *dependents = NULL;
    size_t dependent_count = 0;
    markdown_core_node *holder = NULL; // staging parent for dependent rebuilds
    markdown_core_node **bubble_nodes = NULL;
    size_t bubble_count = 0;
    size_t bubble_capacity = 0;

    markdown_core_lookup_recording_init(&recording, mem);
    memset(&reconcile, 0, sizeof(reconcile));

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
    restart_node = restart_pos >= 0 ? session->clean.items[restart_pos].node : doc->first_child;
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

    // --- 2. staged reparse with resync probing ---
    parser = markdown_core_session_new_parser(session, error);
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
                parser_is_clean(parser)) {
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
    // On resync the last fed line is the one just before the boundary; its
    // terminator-stripped length re-dates transplanted ends below.
    staged_tail_length = parser->last_line_length;

    // --- 3. definition reconciliation ---
    {
        markdown_core_node *sibling;
        markdown_core_node *stop = boundary_pos >= 0 ? session->clean.items[boundary_pos].node : NULL;
        size_t stale_count = 0;
        size_t filled = 0;
        for (sibling = restart_node; sibling && sibling != stop; sibling = sibling->next) {
            stale_count++;
        }
        if (stale_count) {
            stale_ids = (uint64_t *)mem->calloc(stale_count, sizeof(*stale_ids));
            if (!stale_ids) {
                goto failed;
            }
            for (sibling = restart_node; sibling && sibling != stop; sibling = sibling->next) {
                stale_ids[filled++] = sibling->id;
            }
            qsort(stale_ids, stale_count, sizeof(*stale_ids), id_compare);
        }
        if (!collect_new_definitions(mem, map, previous_head, &new_defs) ||
            !collect_stale_definitions(mem, map, previous_head, stale_ids, stale_count, restart_pos < 0, &old_defs)) {
            goto failed;
        }
        defs_equal = definition_sequences_equal(&old_defs, &new_defs);
        if (!defs_equal) {
            bool fallback = false;
            size_t i;
            if (!reconcile_prepare(session, map, order_floor, previous_head, stale_ids, stale_count, restart_pos < 0,
                                   restart_line, &old_defs, &new_defs, &reconcile, &fallback) ||
                !collect_dependents(session, &reconcile.dirty, stale_ids, stale_count, &dependents, &dependent_count,
                                    &fallback)) {
                if (fallback) {
                    result = MARKDOWN_CORE_INCREMENTAL_FALLBACK;
                }
                goto failed;
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
            reconcile_apply(session, map, previous_head, stale_ids, stale_count, restart_pos < 0, restart_line,
                            &new_defs, &reconcile);
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
        markdown_core_parser_free(parser);
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

    markdown_core_session_seal_positions(root);
    {
        // Rebuilt units seal like parse roots (absolute start kept, children
        // relativized), then take the replaced unit's stored start: the
        // position did not change, so the parent-relative value is already
        // right — and stays right when a resync later line-shifts a suffix
        // ancestor.
        size_t i;
        for (i = 0; i < dependent_count; i++) {
            markdown_core_session_seal_positions(dependents[i].staged);
            dependents[i].staged->start_line = dependents[i].unit->start_line;
        }
    }

    // --- 5/6. adoption and the transactional splice ---
    {
        markdown_core_node *boundary_node = boundary_pos >= 0 ? session->clean.items[boundary_pos].node : NULL;
        markdown_core_node *first_stale = restart_node == boundary_node ? NULL : restart_node;
        markdown_core_node *last_stale = NULL;
        markdown_core_node *prefix_tail = NULL;
        markdown_core_node *suffix_head = boundary_node;
        markdown_core_node *staged_first = NULL;
        markdown_core_node *staged_last = NULL;
        size_t staged_nodes = chain_node_count(root->first_child);
        if (holder) {
            staged_nodes += chain_node_count(holder->first_child);
        }
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
        clean_count = prefix_clean + staged_clean + suffix_clean;

        if (!markdown_core_session_ids_reserve(session, staged_nodes)) {
            goto failed;
        }
        // The clean index updates in place after the point of no return
        // (prefix untouched, stale run replaced, suffix slid or biased),
        // so any growth it needs happens here, while failing is still free.
        if (clean_count > session->clean.capacity) {
            markdown_core_clean_child *grown = (markdown_core_clean_child *)mem->realloc(
                session->clean.items, clean_count * sizeof(*session->clean.items));
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
                            markdown_core_lookup_table_reserve(mem, &session->lookups, bundle_count);
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
                        collected = bubble_nodes[k] == ancestor;
                    }
                    if (collected) {
                        break; // and with it every ancestor above
                    }
                    if (bubble_count == bubble_capacity) {
                        size_t grown_capacity = bubble_capacity ? bubble_capacity * 2 : 8;
                        markdown_core_node **grown =
                            (markdown_core_node **)mem->realloc(bubble_nodes, grown_capacity * sizeof(*grown));
                        if (!grown) {
                            staged_ok = false;
                            break;
                        }
                        bubble_nodes = grown;
                        bubble_capacity = grown_capacity;
                    }
                    bubble_nodes[bubble_count++] = ancestor;
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

        // Footnote refresh over the assembled tree: the last fallible step.
        // The two-phase diff mutates nothing on failure, so undoing the
        // splices restores the previous revision exactly.
        if (session->options.footnotes) {
            if (!markdown_core_footnote_index_build(mem, doc, &footnotes) ||
                !markdown_core_footnote_index_diff(mem, &session->footnotes, &footnotes, new_rev, changes)) {
                markdown_core_footnote_index_release(mem, &footnotes);
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
                goto failed;
            }
            footnotes_built = true;
        }

        // --- point of no return: nothing below can fail ---

        // Document node bookkeeping: the staged root carried the adoption
        // verdict under the document's id.
        doc->last_changed_rev = root->last_changed_rev;
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

        // Definitions. Equal sequences: the old entries stay (their orders
        // are document order), take over the staged anchors, and the staged
        // duplicates leave. Reconciled sequences: the staged entries are the
        // truth and their pointer-stamped anchors resolve to ids.
        {
            size_t i;
            uint64_t head_owner = prefix_tail ? prefix_tail->id : 0;
            if (defs_equal) {
                for (i = 0; i < old_defs.count; i++) {
                    uint64_t anchor = new_defs.items[i]->entry.owner;
                    old_defs.items[i]->entry.owner =
                        anchor == 0 ? head_owner : ((const markdown_core_node *)(uintptr_t)anchor)->id;
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
                bundles[i].record.count = 0;
            }
        }

        // Ancestors of a changed rebuilt unit bubble now; the document's own
        // verdict landed above, so it only appears here when the staged
        // adoption left it untouched.
        {
            size_t i;
            for (i = 0; i < bubble_count; i++) {
                bubble_nodes[i]->last_changed_rev = new_rev;
                if (changes) {
                    markdown_core_id_array_push(&changes->bubbled, bubble_nodes[i]->id);
                }
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
            ptrdiff_t index_shift = (ptrdiff_t)(prefix_clean + staged_clean) - (ptrdiff_t)boundary_idx;
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
        markdown_core_parser_free(parser);
    }
    if (root) {
        markdown_core_node_free(root);
    }
    if (result == MARKDOWN_CORE_INCREMENTAL_FAILED && error && !*error) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED,
                                    "could not commit the session incrementally");
    }

done:
    map->lookup_sink = NULL;
    map->lookup_context = NULL;
    map->lookup_unit = NULL;
    markdown_core_lookup_recording_release(&recording);
    markdown_core_unit_lookups_free(mem, bundles, bundle_count);
    reconcile_release(mem, &reconcile);
    if (holder) {
        markdown_core_node_free(holder); // any rebuilt unit not spliced in goes with it
    }
    if (dependents) {
        mem->free(dependents);
    }
    if (bubble_nodes) {
        mem->free(bubble_nodes);
    }
    if (line_offsets.items) {
        mem->free(line_offsets.items);
    }
    if (new_defs.items) {
        mem->free(new_defs.items);
    }
    if (old_defs.items) {
        mem->free(old_defs.items);
    }
    if (stale_ids) {
        mem->free(stale_ids);
    }
    return result;
}
