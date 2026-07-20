#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "session_internal.h"

#include "directive.h"

#include <iterator.h>
#include <node.h>
#include <parser.h>

// A session is a purely local object: it owns its text, its committed tree,
// its reference map, and its id table, and shares no state with any other
// session or any global. Commits route through the incremental pipeline in
// incremental.c when the edits allow it and fall back to a full staged reparse
// (this file) otherwise; both produce identical observable results, which the
// equivalence suite enforces.

static void clear_error(markdown_core_error **error) {
    if (error) {
        *error = NULL;
    }
}

// Mirrors set_error in ast.c; sessions and documents share the error type but
// not a translation unit private to either.
void markdown_core_ast_set_error(markdown_core_error **error, markdown_core_error_code code, const char *message);

static uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

// --- id table ---------------------------------------------------------------

static void id_table_release(markdown_core_mem *mem, markdown_core_id_table *table) {
    if (table->slots) {
        mem->free(mem, table->slots);
    }
    table->slots = NULL;
    table->capacity = 0;
    table->count = 0;
}

static void id_table_insert(markdown_core_id_table *table, markdown_core_node_id id, markdown_core_node *node) {
    size_t mask = table->capacity - 1;
    size_t slot = (size_t)mix64(id) & mask;
    while (table->slots[slot].id != 0) {
        if (table->slots[slot].id == id) {
            table->slots[slot].node = node;
            return;
        }
        slot = (slot + 1) & mask;
    }
    table->slots[slot].id = id;
    table->slots[slot].node = node;
    table->count++;
}

// Allocates a table for at least `entries` ids at <= 50% load.
static bool id_table_alloc(markdown_core_mem *mem, size_t entries, markdown_core_id_table *out) {
    size_t capacity = 16;
    while (capacity < entries * 2) {
        capacity *= 2;
    }
    out->slots = (markdown_core_id_slot *)mem->calloc(mem, capacity, sizeof(markdown_core_id_slot));
    out->count = 0;
    if (!out->slots) {
        return false;
    }
    out->capacity = capacity;
    return true;
}

bool markdown_core_session_ids_reserve(markdown_core_session *session, size_t extra) {
    markdown_core_id_table *table = &session->ids;
    markdown_core_id_table grown = {NULL, 0, 0};
    size_t i;

    if (table->capacity && (table->count + extra) * 2 <= table->capacity) {
        return true;
    }
    if (!id_table_alloc(session->mem, table->count + extra, &grown)) {
        return false;
    }
    for (i = 0; i < table->capacity; i++) {
        if (table->slots[i].id != 0) {
            id_table_insert(&grown, table->slots[i].id, table->slots[i].node);
        }
    }
    id_table_release(session->mem, table);
    *table = grown;
    return true;
}

void markdown_core_session_ids_put(markdown_core_session *session, markdown_core_node_id id, markdown_core_node *node) {
    id_table_insert(&session->ids, id, node);
}

void markdown_core_session_ids_remove(markdown_core_session *session, markdown_core_node_id id) {
    markdown_core_id_table *table = &session->ids;
    size_t mask;
    size_t slot;
    size_t gap;
    size_t scan;

    if (table->capacity == 0) {
        return;
    }
    mask = table->capacity - 1;
    slot = (size_t)mix64(id) & mask;
    while (table->slots[slot].id != id) {
        if (table->slots[slot].id == 0) {
            return;
        }
        slot = (slot + 1) & mask;
    }

    // Backward-shift deletion: pull every entry of the collision run whose
    // home slot lies at or before the gap into it. The load-factor bound
    // guarantees an empty slot, so the walk terminates.
    gap = slot;
    scan = (gap + 1) & mask;
    while (table->slots[scan].id != 0) {
        size_t home = (size_t)mix64(table->slots[scan].id) & mask;
        if (((scan - home) & mask) >= ((scan - gap) & mask)) {
            table->slots[gap] = table->slots[scan];
            gap = scan;
        }
        scan = (scan + 1) & mask;
    }
    table->slots[gap].id = 0;
    table->slots[gap].node = NULL;
    table->count--;
}

// Builds a fresh id table for `root` into `out` (owned by the caller on
// success). Runs inside mutating calls only, so concurrent readers never
// observe a table under construction.
static bool id_table_build(markdown_core_mem *mem, markdown_core_node *root, markdown_core_id_table *out) {
    size_t nodes = 0;

    markdown_core_iter *iter = markdown_core_iter_new(root);
    if (!iter) {
        return false;
    }
    markdown_core_event_type ev;
    while ((ev = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        if (ev == MARKDOWN_CORE_EVENT_ENTER) {
            nodes++;
        }
    }
    markdown_core_iter_free(iter);

    if (!id_table_alloc(mem, nodes, out)) {
        return false;
    }

    iter = markdown_core_iter_new(root);
    if (!iter) {
        id_table_release(mem, out);
        return false;
    }
    while ((ev = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        if (ev != MARKDOWN_CORE_EVENT_ENTER) {
            continue;
        }
        markdown_core_node *node = markdown_core_iter_get_node(iter);
        // Directive-label wrappers are facade-invisible and not addressable.
        if (node->type == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL) {
            continue;
        }
        id_table_insert(out, node->id, node);
    }
    markdown_core_iter_free(iter);
    return true;
}

// --- parsing ----------------------------------------------------------------

static int native_options_from(const markdown_core_parse_options *options) {
    int native_options = MARKDOWN_CORE_OPT_VALIDATE_UTF8;
    if (options->smart_punctuation) {
        native_options |= MARKDOWN_CORE_OPT_SMART;
    }
    if (options->footnotes) {
        native_options |= MARKDOWN_CORE_OPT_FOOTNOTES;
    }
    if (options->strip_html_comments) {
        native_options |= MARKDOWN_CORE_OPT_STRIP_HTML_COMMENTS;
    }
    if (options->formulas && options->dollar_formula_delimiters) {
        native_options |= MARKDOWN_CORE_OPT_DOLLAR_FORMULA_DELIMITERS;
    }
    if (options->formulas && options->latex_formula_delimiters) {
        native_options |= MARKDOWN_CORE_OPT_LATEX_FORMULA_DELIMITERS;
    }
    if (options->directives) {
        native_options |= MARKDOWN_CORE_OPT_DIRECTIVE;
    }
    return native_options;
}

static bool attach_extension_named(markdown_core_parser *parser, const char *name) {
    markdown_core_extension *extension = markdown_core_extension_find(name);
    return extension && markdown_core_parser_attach_extension(parser, extension) != 0;
}

markdown_core_parser *markdown_core_session_new_parser(markdown_core_session *session, markdown_core_error **error) {
    markdown_core_parser *parser =
        markdown_core_parser_new_with_mem(native_options_from(&session->options), session->mem);
    if (!parser) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate parser");
        return NULL;
    }

    const markdown_core_parse_options *options = &session->options;
    bool attached = (!options->tables || attach_extension_named(parser, "table")) &&
                    (!options->strikethrough || attach_extension_named(parser, "strikethrough")) &&
                    (!options->autolinks || attach_extension_named(parser, "autolink")) &&
                    (!options->task_lists || attach_extension_named(parser, "tasklist")) &&
                    (!options->formulas || attach_extension_named(parser, "formula")) &&
                    (!options->directives || attach_extension_named(parser, "directive"));
    if (!attached) {
        markdown_core_parser_free(parser);
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_INTERNAL, "required syntax extension is unavailable");
        return NULL;
    }
    return parser;
}

markdown_core_parser *
markdown_core_session_acquire_parser(markdown_core_session *session, markdown_core_error **error) {
    markdown_core_parser *parser = session->warm_parser;
    if (parser) {
        session->warm_parser = NULL;
        return parser;
    }
    return markdown_core_session_new_parser(session, error);
}

void markdown_core_session_release_parser(markdown_core_session *session, markdown_core_parser *parser) {
    if (!parser) {
        return;
    }
    if (!parser->oom && !session->warm_parser) {
        markdown_core_parser_renew(parser);
        if (!parser->oom) {
            session->warm_parser = parser;
            return;
        }
    }
    markdown_core_parser_free(parser);
}

// Seals a freshly parsed tree: line positions convert from absolute to
// parent-relative deltas (columns are line-local already) and every node
// gains MARKDOWN_CORE_NODE__SEALED_RELATIVE. Post-order, so each conversion
// still reads its parent's absolute start; pointer-walk iterative, because
// adversarial inputs nest too deep for native recursion.
//
// Position-free nodes (start_line 0: soft/hard breaks and synthesized blocks
// like a table's split-off header paragraph) stay raw and unsealed. Their
// zeros are markers, not places: relativizing them would make them move when
// an incremental commit line-shifts a transplanted ancestor. Resolution
// treats an unsealed node's fields as final, so the markers stay zero and
// their descendants' deltas (computed against the raw zero here) still
// resolve exactly as a fresh parse would.
size_t markdown_core_session_seal_positions(markdown_core_node *root) {
    markdown_core_node *node = root;
    size_t sealed = 0;
    for (;;) {
        if (node->first_child) {
            node = node->first_child;
            continue;
        }
        for (;;) {
            int start_line = node->start_line;
            sealed++;
            if (start_line != 0) {
                if (node->parent) {
                    node->start_line = start_line - node->parent->start_line;
                }
                node->end_line -= start_line;
                node->flags |= MARKDOWN_CORE_NODE__SEALED_RELATIVE;
            }
            if (node == root) {
                return sealed;
            }
            if (node->next) {
                node = node->next;
                break;
            }
            node = node->parent;
        }
    }
}

void markdown_core_session_resolve_definition_owners(markdown_core_map *map) {
    markdown_core_map_entry *entry;
    for (entry = map->refs; entry; entry = entry->next) {
        if (entry->owner != 0) {
            entry->owner = ((const markdown_core_node *)(uintptr_t)entry->owner)->id;
        }
    }
}

// Full staged reparse: parses the whole stored text with a fresh parser and
// reference map, adopts ids from the previous tree, and replaces every piece
// of session state at once. The staging never touches the committed state,
// so any failure leaves the session valid at its previous revision.
static bool
commit_full(markdown_core_session *session, bool initial, markdown_core_delta *changes, markdown_core_error **error) {
    markdown_core_parser *parser;
    markdown_core_node *root;
    markdown_core_map *map;
    markdown_core_lookup_recording recording;
    int total_lines;
    int last_line_length;
    uint64_t new_rev = initial ? 0 : session->revision + 1;

    parser = markdown_core_session_acquire_parser(session, error);
    if (!parser) {
        return false;
    }

    markdown_core_lookup_recording_init(&recording, session->mem);
    if (session->record_lookups && parser->refmap) {
        parser->refmap->lookup_sink = markdown_core_lookup_recording_sink;
        parser->refmap->lookup_context = &recording;
    }

    size_t length = markdown_core_text_length(&session->text);
    if (length) {
        markdown_core_parser_feed(parser, (const char *)markdown_core_text_bytes(&session->text), length);
    }
    markdown_core_parser_finalize_blocks(parser);
    total_lines = parser->line_number;
    last_line_length = parser->last_line_length;
    root = markdown_core_parser_refine_blocks(parser);
    if (!root) {
        markdown_core_parser_free(parser); // frees the staged map with it
        markdown_core_lookup_recording_release(&recording);
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not parse the session text");
        return false;
    }
    map = parser->refmap;
    parser->refmap = NULL;
    markdown_core_session_release_parser(session, parser);
    // The sink's context is this call's stack frame; the map outlives it.
    map->lookup_sink = NULL;
    map->lookup_context = NULL;
    map->lookup_unit = NULL;
    if (recording.lost) {
        markdown_core_map_free(map);
        markdown_core_node_free(root);
        markdown_core_lookup_recording_release(&recording);
        markdown_core_ast_set_error(
            error,
            MARKDOWN_CORE_ERROR_ALLOCATION_FAILED,
            "could not record the document's reference lookups"
        );
        return false;
    }

    markdown_core_session_seal_positions(root);

    if (changes) {
        changes->before = session->revision;
        changes->after = new_rev;
    }

    if (!markdown_core_session_adopt(session, session->view.root, root, new_rev, changes)) {
        markdown_core_map_free(map);
        markdown_core_node_free(root);
        markdown_core_lookup_recording_release(&recording);
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not record the delta");
        return false;
    }
    // Ids exist now; definitions recorded against anchor node pointers can
    // take their retraction ids, and lookup records can bind to unit ids.
    markdown_core_session_resolve_definition_owners(map);

    markdown_core_lookup_table lookups = {NULL, NULL, 0, 0, {NULL, 0, 0, {NULL, NULL, 0, 0}}};
    {
        markdown_core_unit_lookups *bundles = NULL;
        size_t bundle_count = 0;
        size_t i;
        bool bound = markdown_core_lookup_recording_bundle(&recording, &bundles, &bundle_count) &&
                     markdown_core_lookup_table_reserve(session->mem, &lookups, bundle_count) &&
                     markdown_core_lookup_postings_reserve(session->mem, &lookups, bundles, bundle_count);
        if (!bound) {
            markdown_core_unit_lookups_free(session->mem, bundles, bundle_count);
            markdown_core_lookup_recording_release(&recording);
            markdown_core_lookup_table_release(session->mem, &lookups);
            markdown_core_map_free(map);
            markdown_core_node_free(root);
            markdown_core_ast_set_error(
                error,
                MARKDOWN_CORE_ERROR_ALLOCATION_FAILED,
                "could not index the document's reference lookups"
            );
            return false;
        }
        for (i = 0; i < bundle_count; i++) {
            markdown_core_lookup_table_put(session->mem, &lookups, bundles[i].unit->id, bundles[i].record);
            bundles[i].record.labels = NULL; // moved into the table
            bundles[i].record.positions = NULL;
            bundles[i].record.count = 0;
        }
        markdown_core_unit_lookups_free(session->mem, bundles, bundle_count);
        markdown_core_lookup_recording_release(&recording);
    }

    // Footnote numbering, resolution state, and back-reference ordinals are
    // index-backed queries; a commit that changes only those answers bumps
    // the affected revisions without any dump-visible change.
    markdown_core_footnote_index footnotes;
    memset(&footnotes, 0, sizeof(footnotes));
    if (!markdown_core_footnote_index_build(session, root, &footnotes) ||
        !markdown_core_footnote_index_diff(session->mem, &session->footnotes, &footnotes, new_rev, changes)) {
        markdown_core_footnote_index_release(session->mem, &footnotes);
        markdown_core_lookup_table_release(session->mem, &lookups);
        markdown_core_map_free(map);
        markdown_core_node_free(root);
        markdown_core_ast_set_error(
            error,
            MARKDOWN_CORE_ERROR_ALLOCATION_FAILED,
            "could not index the document's footnotes"
        );
        return false;
    }

    // The lookup table is maintained here, inside the mutating call, so
    // markdown_core_session_node_by_id stays a pure concurrent-safe read.
    markdown_core_id_table ids = {NULL, 0, 0};
    markdown_core_clean_index clean = {NULL, 0, 0};
    markdown_core_map_entry **def_index = NULL;
    size_t def_count = 0;
    if (!id_table_build(session->mem, root, &ids) ||
        !markdown_core_session_index_clean_children(session, root, map, &clean) ||
        !markdown_core_session_index_definitions(session, map, &def_index, &def_count)) {
        id_table_release(session->mem, &ids);
        if (clean.items) {
            session->mem->free(session->mem, clean.items);
        }
        markdown_core_footnote_index_release(session->mem, &footnotes);
        markdown_core_lookup_table_release(session->mem, &lookups);
        markdown_core_map_free(map);
        markdown_core_node_free(root);
        markdown_core_ast_set_error(
            error,
            MARKDOWN_CORE_ERROR_ALLOCATION_FAILED,
            "could not index the committed document"
        );
        return false;
    }

    if (session->view.root) {
        markdown_core_node_free(session->view.root);
    }
    id_table_release(session->mem, &session->ids);
    markdown_core_footnote_index_release(session->mem, &session->footnotes);
    markdown_core_lookup_table_release(session->mem, &session->lookups);
    markdown_core_map_free(session->refmap);
    if (session->clean.items) {
        session->mem->free(session->mem, session->clean.items);
    }
    if (session->def_index) {
        session->mem->free(session->mem, session->def_index);
    }
    session->def_index = def_index;
    session->def_count = def_count;
    session->def_capacity = def_count ? def_count : 1;
    session->view.root = root;
    session->ids = ids;
    session->footnotes = footnotes;
    session->lookups = lookups;
    session->refmap = map;
    session->refmap_stale = false;
    session->clean = clean;
    session->total_lines = total_lines;
    session->last_line_length = last_line_length;
    session->expansion_estimate = map->ref_size;
    session->revision = new_rev;
    session->full_commits++;
    session->pending.dirty = false;
    session->pending.new_lo = 0;
    session->pending.new_hi = 0;
    session->pending.delta = 0;
    return true;
}

static bool commit_internal(
    markdown_core_session *session,
    bool initial,
    markdown_core_delta **changes_out,
    markdown_core_error **error
) {
    markdown_core_delta *changes = NULL;

    if (changes_out) {
        changes = (markdown_core_delta *)calloc(1, sizeof(*changes));
        if (!changes) {
            markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate delta");
            return false;
        }
        changes->before = session->revision;
        changes->after = initial ? 0 : session->revision + 1;
    }

    // A commit with no pending edits advances the revision with an empty
    // delta; nothing else can have changed.
    if (!initial && session->view.root && !session->pending.dirty) {
        session->revision++;
        if (changes_out) {
            *changes_out = changes;
        }
        return true;
    }

    if (!initial && session->view.root && session->refmap && !session->refmap->oom && !session->refmap_stale) {
        switch (markdown_core_session_commit_incremental(session, session->revision + 1, changes, error)) {
        case MARKDOWN_CORE_INCREMENTAL_COMMITTED:
            if (changes_out) {
                *changes_out = changes;
            }
            return true;
        case MARKDOWN_CORE_INCREMENTAL_FAILED:
            markdown_core_delta_free(changes);
            return false;
        case MARKDOWN_CORE_INCREMENTAL_FALLBACK:
            // The delta may hold nothing yet (the pipeline records only
            // after its point of no return), so it is reusable as-is.
            break;
        }
    }

    if (!commit_full(session, initial, changes, error)) {
        markdown_core_delta_free(changes);
        return false;
    }
    if (changes_out) {
        *changes_out = changes;
    }
    return true;
}

// --- public API -------------------------------------------------------------

markdown_core_session *markdown_core_session_open_with_mem(
    const markdown_core_parse_options *options,
    markdown_core_mem *mem,
    bool pooled,
    markdown_core_error **error
) {
    clear_error(error);

    markdown_core_session *session = (markdown_core_session *)calloc(1, sizeof(*session));
    if (!session) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate session");
        return NULL;
    }

    if (options) {
        session->options = *options;
    } else {
        markdown_core_parse_options_init(&session->options);
    }
#if MARKDOWN_CORE_ASAN
    // Slab-carved and freelist-reused blocks are invisible to
    // AddressSanitizer, so pooling would blind the ASan suites to
    // use-after-free and overflow inside session memory. The sanitizer
    // build exercises the exact same allocation paths against the base
    // allocator instead.
    pooled = false;
#endif
    if (pooled) {
        session->arena = markdown_core_arena_new(mem);
        if (!session->arena) {
            free(session);
            markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate session");
            return NULL;
        }
        mem = markdown_core_arena_mem(session->arena);
    }
    session->mem = mem;
    markdown_core_text_init(&session->text, mem);
    session->next_id = 1;
    session->revision = 0;
    session->record_lookups = true;

    // Purely local entropy: no global RNG state. The lineage only has to make
    // accidental cross-session id equality vanishingly unlikely.
    uint64_t entropy = (uint64_t)(uintptr_t)session;
    entropy ^= mix64((uint64_t)time(NULL));
    entropy ^= mix64((uint64_t)clock()) << 1;
    session->lineage = mix64(entropy);

    if (!commit_internal(session, true, NULL, error)) {
        markdown_core_session_free(session);
        return NULL;
    }
    return session;
}

markdown_core_session *
markdown_core_session_open(const markdown_core_parse_options *options, markdown_core_error **error) {
    return markdown_core_session_open_with_mem(options, markdown_core_mem_default(), true, error);
}

void markdown_core_session_free(markdown_core_session *session) {
    if (!session) {
        return;
    }
    if (session->arena) {
        // Everything session-owned came from the arena (deltas and errors
        // are caller-owned system allocations); one release replaces the
        // per-structure teardown below.
        markdown_core_arena_release(session->arena);
        free(session);
        return;
    }
    if (session->view.root) {
        markdown_core_node_free(session->view.root);
    }
    markdown_core_text_release(&session->text);
    id_table_release(session->mem, &session->ids);
    markdown_core_footnote_index_release(session->mem, &session->footnotes);
    markdown_core_lookup_table_release(session->mem, &session->lookups);
    markdown_core_map_free(session->refmap);
    if (session->clean.items) {
        session->mem->free(session->mem, session->clean.items);
    }
    if (session->def_index) {
        session->mem->free(session->mem, session->def_index);
    }
    markdown_core_footnote_labels_release(session->mem, &session->footnote_labels);
    if (session->warm_parser) {
        markdown_core_parser_free(session->warm_parser);
    }
    free(session);
}

bool markdown_core_session_edit(
    markdown_core_session *session,
    size_t byte_start,
    size_t byte_end,
    const uint8_t *bytes,
    size_t length,
    markdown_core_error **error
) {
    clear_error(error);
    if (!session) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, "session must not be null");
        return false;
    }
    if (byte_start > byte_end || byte_end > markdown_core_text_length(&session->text)) {
        markdown_core_ast_set_error(
            error,
            MARKDOWN_CORE_ERROR_INVALID_ARGUMENT,
            "edit range is outside the current text"
        );
        return false;
    }
    if (!bytes && length != 0) {
        markdown_core_ast_set_error(
            error,
            MARKDOWN_CORE_ERROR_INVALID_ARGUMENT,
            "bytes must not be null when length is nonzero"
        );
        return false;
    }
    if (!markdown_core_text_edit(&session->text, byte_start, byte_end, bytes, length)) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not apply the edit");
        return false;
    }

    // Coalesce into the pending summary. The stored range lives in
    // current-text coordinates, so first map the existing range through this
    // edit, then union in the freshly written bytes.
    {
        markdown_core_edit_summary *pending = &session->pending;
        size_t removed = byte_end - byte_start;
        if (pending->dirty) {
            size_t hi = pending->new_hi;
            if (hi > byte_start) {
                hi = hi <= byte_end ? byte_start + length : hi + length - removed;
            }
            if (pending->new_lo > byte_start) {
                pending->new_lo = byte_start;
            }
            pending->new_hi = hi > byte_start + length ? hi : byte_start + length;
        } else {
            pending->dirty = true;
            pending->new_lo = byte_start;
            pending->new_hi = byte_start + length;
        }
        pending->delta += (ptrdiff_t)length - (ptrdiff_t)removed;
    }
    return true;
}

bool markdown_core_session_commit(
    markdown_core_session *session,
    markdown_core_delta **changes,
    markdown_core_error **error
) {
    clear_error(error);
    if (changes) {
        *changes = NULL;
    }
    if (!session) {
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, "session must not be null");
        return false;
    }
    return commit_internal(session, false, changes, error);
}

const markdown_core_document *markdown_core_session_document(const markdown_core_session *session) {
    return session ? &session->view : NULL;
}

uint64_t markdown_core_session_revision(const markdown_core_session *session) {
    return session ? session->revision : 0;
}

uint64_t markdown_core_session_lineage(const markdown_core_session *session) { return session ? session->lineage : 0; }

size_t markdown_core_session_length(const markdown_core_session *session) {
    return session ? markdown_core_text_length(&session->text) : 0;
}

const markdown_core_node *
markdown_core_session_node_by_id(const markdown_core_session *session, markdown_core_node_id id) {
    if (!session || id == 0) {
        return NULL;
    }
    if (session->ids.capacity == 0) {
        return NULL;
    }
    size_t mask = session->ids.capacity - 1;
    size_t slot = (size_t)mix64(id) & mask;
    while (session->ids.slots[slot].id != 0) {
        if (session->ids.slots[slot].id == id) {
            return session->ids.slots[slot].node;
        }
        slot = (slot + 1) & mask;
    }
    return NULL;
}
