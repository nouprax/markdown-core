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
//                      dropped after adoption. Any difference falls the
//                      whole commit back to a full reparse, which rebuilds
//                      the map: at rest every entry order therefore stems
//                      from a full parse and per-label winner election sees
//                      true document order.
//   4. Inline phase  — runs on the staged region only, with an unlimited
//                      expansion budget; the session-tracked expansion
//                      estimate proves a one-shot parse would not have hit
//                      its budget either, or the commit falls back.
//   5. Adoption      — the stale children pair against the staged children
//                      through the standard adoption machine (a stack dummy
//                      document fronts the graveyard), so block- and
//                      inline-level id stability behave exactly like the
//                      full path.
//   6. Transactional splice — every fallible step runs before the committed
//                      tree changes hands or is undone with pointer surgery
//                      only; after the footnote refresh succeeds, the
//                      remaining bookkeeping (id table, clean index,
//                      geometry, graveyard release) cannot fail.

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
    markdown_core_node *root = NULL; // refined staged tree, once detached
    offset_list line_offsets = {NULL, 0, 0};
    reference_list new_defs = {NULL, 0, 0};
    reference_list old_defs = {NULL, 0, 0};
    uint64_t *stale_ids = NULL;
    markdown_core_footnote_index footnotes = {NULL, 0, NULL, 0, NULL, NULL};
    bool footnotes_built = false;

    // --- 1. restart plan ---
    ptrdiff_t restart_pos = restart_position(&session->clean, old_lo);
    markdown_core_node *restart_node = restart_pos >= 0 ? session->clean.items[restart_pos].node : doc->first_child;
    size_t restart_byte = restart_pos >= 0 ? session->clean.items[restart_pos].start_byte : 0;
    int restart_line = restart_pos >= 0 ? session->clean.items[restart_pos].start_line : 1;

    ptrdiff_t boundary_pos = -1;
    int fed_lines = 0;
    int total_lines;
    int last_line_length;

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
        if (!definition_sequences_equal(&old_defs, &new_defs)) {
            result = MARKDOWN_CORE_INCREMENTAL_FALLBACK;
            goto failed;
        }
    }

    // --- 4. inline phase over the staged region ---
    // Unlimited budget: the estimate check below proves a one-shot parse
    // stays within its own budget, so no lookup can be denied in either.
    map->ref_size = 0;
    map->max_ref_size = (size_t)-1;
    root = markdown_core_parser_refine_blocks(parser);
    {
        size_t phase_expansion = map->ref_size;
        map->max_ref_size = budget;
        parser->refmap = own_map;
        own_map = NULL;
        markdown_core_parser_free(parser);
        parser = NULL;
        if (!root) {
            goto failed;
        }
        if (map->oom) {
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
        // staged root (same id, same changed/bubbled semantics).
        {
            markdown_core_node dummy;
            memset(&dummy, 0, sizeof(dummy));
            dummy.type = (uint16_t)MARKDOWN_CORE_NODE_DOCUMENT;
            dummy.id = doc->id;
            dummy.last_changed_rev = doc->last_changed_rev;
            dummy.first_child = first_stale;
            dummy.last_child = last_stale;
            if (!markdown_core_session_adopt(session, &dummy, root, new_rev, changes)) {
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

        // Footnote refresh over the assembled tree: the last fallible step.
        // The two-phase diff mutates nothing on failure, so undoing the two
        // splices restores the previous revision exactly.
        if (session->options.footnotes) {
            if (!markdown_core_footnote_index_build(mem, doc, &footnotes) ||
                !markdown_core_footnote_index_diff(mem, &session->footnotes, &footnotes, new_rev, changes)) {
                markdown_core_footnote_index_release(mem, &footnotes);
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
        }
        doc->end_line = total_lines - 1;
        doc->end_column = last_line_length;

        // Definitions: the old entries stay (their orders are document
        // order); they take over the staged anchors, and the staged
        // duplicates leave.
        {
            size_t i;
            uint64_t head_owner = prefix_tail ? prefix_tail->id : 0;
            for (i = 0; i < old_defs.count; i++) {
                uint64_t anchor = new_defs.items[i]->entry.owner;
                old_defs.items[i]->entry.owner =
                    anchor == 0 ? head_owner : ((const markdown_core_node *)(uintptr_t)anchor)->id;
            }
            markdown_core_map_remove_until(map, previous_head);
        }

        // Id table: repoint adopted ids at their staged nodes, then drop
        // whatever still points into the graveyard.
        ids_put_chain(session, staged_first, suffix_head);
        ids_remove_stale_chain(session, first_stale);

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

        session->total_lines = total_lines;
        session->last_line_length = last_line_length;
        session->revision = new_rev;
        session->pending.dirty = false;
        session->pending.new_lo = 0;
        session->pending.new_hi = 0;
        session->pending.delta = 0;

        free_child_chain(first_stale);
        markdown_core_node_free(root);
        root = NULL;
    }

    result = MARKDOWN_CORE_INCREMENTAL_COMMITTED;
    goto done;

failed:
    // Pointer-stamped duplicates never survive a failed or abandoned
    // pipeline: later commits must find only id-anchored entries at rest.
    markdown_core_map_remove_until(map, previous_head);
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
