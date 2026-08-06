#if defined(_WIN32) && !defined(_CRT_RAND_S)
// rand_s is the linkage-free CSPRNG on Windows and must be requested before
// the first stdlib.h include.
#define _CRT_RAND_S
#endif
#if (defined(__EMSCRIPTEN__) || defined(__wasi__)) && !defined(_GNU_SOURCE)
// musl only declares getentropy outside strict-standard mode.
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__EMSCRIPTEN__) || defined(__wasi__)
#include <unistd.h>
#elif !defined(__APPLE__) && !defined(_WIN32)
#include <stdio.h>
#endif

#include "session_internal.h"

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
    size_t slot = (size_t)markdown_core_mix64(id) & mask;
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
    slot = (size_t)markdown_core_mix64(id) & mask;
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
        size_t home = (size_t)markdown_core_mix64(table->slots[scan].id) & mask;
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
    /* Attachment order is priority. `table` is attached last because its row
     * opener accepts any non-blank line inside an open table, so anything with
     * a narrower claim has to be offered the line first — which is exactly
     * what the GFM specification requires of a table: "The table is broken at
     * the first empty line, or beginning of another block-level structure."
     * A construct that matches everything can only ever be the fallback. */
    bool attached = (!options->strikethrough || attach_extension_named(parser, "strikethrough")) &&
                    (!options->autolinks || attach_extension_named(parser, "autolink")) &&
                    (!options->task_lists || attach_extension_named(parser, "tasklist")) &&
                    (!options->formulas || attach_extension_named(parser, "formula")) &&
                    (!options->directives || attach_extension_named(parser, "directive")) &&
                    (!options->cross_links || attach_extension_named(parser, "cross_link")) &&
                    (!options->embeds || attach_extension_named(parser, "embed")) &&
                    (!options->tables || attach_extension_named(parser, "table"));
    if (!attached) {
        bool allocation_failed = parser->oom;
        markdown_core_parser_free(parser);
        markdown_core_ast_set_error(
            error,
            allocation_failed ? MARKDOWN_CORE_ERROR_ALLOCATION_FAILED : MARKDOWN_CORE_ERROR_INTERNAL,
            allocation_failed ? "could not attach the required syntax extensions"
                              : "required syntax extension is unavailable"
        );
        return NULL;
    }
    return parser;
}

markdown_core_parser *markdown_core_session_acquire_parser(
    markdown_core_session *session,
    markdown_core_error **error
) {
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
    if (!parser->oom && !parser->internal_error && !session->warm_parser) {
        markdown_core_parser_renew(parser);
        if (!parser->oom && !parser->internal_error) {
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
// Position-free nodes (start_line 0: soft and hard breaks) stay raw and
// unsealed. Their
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

/* Both of the parser's definition maps feed one recording, but each through
 * the sink of its own kind: events carry the kind structurally, and bundling
 * partitions them into per-kind lookup tables, so the two label namespaces
 * share no keys (6.3 step 0) and a flipped `[x]:` selects exactly the units
 * that read `[x]` — never the ones that only read `[^x]`. The
 * definition-flip gate holds each flip to its own kind's exact mention
 * count.
 *
 * Both or neither, never one. Half the dependencies recorded is worse than
 * none, because the next commit would treat the half it has as the whole. A
 * map is absent only when its allocation failed, which poisons the parser and
 * makes it report failure rather than a tree — and the parser relies on this
 * pairing: with the reference map watched, it attributes lookups on the
 * footnote map without testing it again. */
static void watch_definition_lookups(markdown_core_parser *parser, markdown_core_lookup_recording *recording) {
    if (parser->refmap && parser->footnote_defs) {
        parser->refmap->lookup_sink = markdown_core_lookup_recording_sink_references;
        parser->refmap->lookup_context = recording;
        parser->footnote_defs->lookup_sink = markdown_core_lookup_recording_sink_footnotes;
        parser->footnote_defs->lookup_context = recording;
    }
}

void markdown_core_session_resolve_definition_owners(markdown_core_map *map) {
    markdown_core_map_entry *entry;
    for (entry = map->refs; entry; entry = entry->next) {
        if (entry->owner != 0) {
            entry->owner = ((const markdown_core_node *)(uintptr_t)entry->owner)->id;
        }
        /* Unguarded, unlike `owner`: 0 is a real owner (the region before the
         * first document child), while every entry that reaches adoption was
         * stamped with the node it was written as. A definition whose node
         * was lost to allocation failure fails the parse instead, so its
         * entry never gets here. */
        entry->definition_node = ((const markdown_core_node *)(uintptr_t)entry->definition_node)->id;
    }
}

/* Frees a set of lookup tables — the staged ones a failed full commit
 * abandons, or the committed ones it replaces. One instance per definition
 * kind, like the definition tables. */
static void release_lookup_tables(markdown_core_mem *mem, markdown_core_lookup_table *tables) {
    size_t s;
    for (s = 0; s < MARKDOWN_CORE_DEFINITION_TABLE_COUNT; s++) {
        markdown_core_lookup_table_release(mem, &tables[s]);
    }
}

/* Frees a set of definition tables — the staged ones a failed full commit
 * abandons, or the committed ones it replaces. */
static void release_definition_tables(markdown_core_mem *mem, markdown_core_definition_table *tables) {
    size_t s;
    for (s = 0; s < MARKDOWN_CORE_DEFINITION_TABLE_COUNT; s++) {
        markdown_core_map_free(tables[s].map);
        if (tables[s].index) {
            mem->free(mem, tables[s].index);
        }
        memset(&tables[s], 0, sizeof(tables[s]));
    }
}

// Full staged reparse: parses the whole stored text with a fresh parser and
// fresh definition maps, adopts ids from the previous tree, and replaces every
// piece of session state at once. The staging never touches the committed
// state, so any failure leaves the session valid at its previous revision.
static bool commit_full(
    markdown_core_session *session,
    bool initial,
    markdown_core_delta *changes,
    markdown_core_error **error
) {
    markdown_core_parser *parser;
    markdown_core_node *root;
    markdown_core_definition_table staged[MARKDOWN_CORE_DEFINITION_TABLE_COUNT];
    markdown_core_map *map;
    markdown_core_lookup_recording recording;
    int total_lines;
    int last_line_length;
    size_t s;
    uint64_t new_rev = initial ? 0 : session->revision + 1;

    parser = markdown_core_session_acquire_parser(session, error);
    if (!parser) {
        return false;
    }

    markdown_core_lookup_recording_init(&recording, session->mem);
    if (session->record_lookups) {
        watch_definition_lookups(parser, &recording);
    }

    // POC-1 SPIKE: feed leaf run by leaf run. markdown_core_parser_feed is a
    // streaming interface (core/blocks.c S_parser_feed buffers a partial line
    // in parser->linebuf), so the chunking is free to follow the rope.
    size_t length = markdown_core_source_length(session->source);
    {
        size_t pos = 0;
        while (pos < length) {
            size_t run = 0;
            const uint8_t *bytes = markdown_core_source_run_at(session->source, pos, &run);
            markdown_core_parser_feed(parser, (const char *)bytes, run);
            pos += run;
        }
    }
    markdown_core_parser_finalize_blocks(parser);
    total_lines = parser->line_number;
    last_line_length = parser->last_line_length;
    root = markdown_core_parser_refine_blocks(parser);
    if (!root) {
        bool allocation_failed = parser->oom;
        bool internal_error = parser->internal_error;
        markdown_core_parser_free(parser); // frees the staged maps with it
        markdown_core_lookup_recording_release(&recording);
        markdown_core_ast_set_error(
            error,
            allocation_failed || !internal_error ? MARKDOWN_CORE_ERROR_ALLOCATION_FAILED : MARKDOWN_CORE_ERROR_INTERNAL,
            allocation_failed || !internal_error ? "could not parse the session text"
                                                 : "parser refinement invariant failed"
        );
        return false;
    }
    memset(staged, 0, sizeof(staged));
    staged[MARKDOWN_CORE_DEFINITIONS_REFERENCES].map = parser->refmap;
    staged[MARKDOWN_CORE_DEFINITIONS_FOOTNOTES].map = parser->footnote_defs;
    parser->refmap = NULL;
    parser->footnote_defs = NULL;
    markdown_core_session_release_parser(session, parser);
    for (s = 0; s < MARKDOWN_CORE_DEFINITION_TABLE_COUNT; s++) {
        // The sink's context is this call's stack frame; the maps outlive it.
        // Both are present: a parse that lost one is poisoned and returns no
        // root, so this line is only reached with the tree in hand.
        staged[s].map->lookup_sink = NULL;
        staged[s].map->lookup_context = NULL;
        staged[s].map->lookup_unit = NULL;
    }
    map = staged[MARKDOWN_CORE_DEFINITIONS_REFERENCES].map;
    if (recording.lost) {
        release_definition_tables(session->mem, staged);
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
        release_definition_tables(session->mem, staged);
        markdown_core_node_free(root);
        markdown_core_lookup_recording_release(&recording);
        markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not record the delta");
        return false;
    }
    // Ids exist now; definitions recorded against anchor node pointers can
    // take their retraction ids, and lookup records can bind to unit ids.
    for (s = 0; s < MARKDOWN_CORE_DEFINITION_TABLE_COUNT; s++) {
        markdown_core_session_resolve_definition_owners(staged[s].map);
    }

    markdown_core_lookup_table lookups[MARKDOWN_CORE_DEFINITION_TABLE_COUNT];
    memset(lookups, 0, sizeof(lookups));
    {
        markdown_core_unit_lookups *bundles[MARKDOWN_CORE_DEFINITION_TABLE_COUNT];
        size_t bundle_count[MARKDOWN_CORE_DEFINITION_TABLE_COUNT];
        size_t i;
        // One bundling pass partitions the recording by kind; each kind's
        // bundles install into its own table instance.
        bool bound = markdown_core_lookup_recording_bundle(&recording, bundles, bundle_count);
        for (s = 0; bound && s < MARKDOWN_CORE_DEFINITION_TABLE_COUNT; s++) {
            bound = markdown_core_lookup_table_reserve(session->mem, &lookups[s], bundle_count[s]) &&
                    markdown_core_lookup_postings_reserve(session->mem, &lookups[s], bundles[s], bundle_count[s]);
        }
        if (!bound) {
            for (s = 0; s < MARKDOWN_CORE_DEFINITION_TABLE_COUNT; s++) {
                markdown_core_unit_lookups_free(session->mem, bundles[s], bundle_count[s]);
            }
            markdown_core_lookup_recording_release(&recording);
            release_lookup_tables(session->mem, lookups);
            release_definition_tables(session->mem, staged);
            markdown_core_node_free(root);
            markdown_core_ast_set_error(
                error,
                MARKDOWN_CORE_ERROR_ALLOCATION_FAILED,
                "could not index the document's reference lookups"
            );
            return false;
        }
        for (s = 0; s < MARKDOWN_CORE_DEFINITION_TABLE_COUNT; s++) {
            for (i = 0; i < bundle_count[s]; i++) {
                markdown_core_lookup_table_put(session->mem, &lookups[s], bundles[s][i].unit->id, bundles[s][i].record);
                bundles[s][i].record.labels = NULL; // moved into the table
                bundles[s][i].record.positions = NULL;
                bundles[s][i].record.count = 0;
            }
            markdown_core_unit_lookups_free(session->mem, bundles[s], bundle_count[s]);
        }
        markdown_core_lookup_recording_release(&recording);
    }

    // Footnote numbering, resolution state, and back-reference ordinals are
    // index-backed queries; a commit that changes only those answers bumps
    // the affected revisions without any dump-visible change.
    markdown_core_footnote_index footnotes;
    memset(&footnotes, 0, sizeof(footnotes));
    if (!session->one_shot && session->options.footnotes &&
        (!markdown_core_footnote_index_build(session, root, &footnotes) ||
         !markdown_core_footnote_index_diff(session->mem, &session->footnotes, &footnotes, new_rev, changes))) {
        markdown_core_footnote_index_release(session->mem, &footnotes);
        release_lookup_tables(session->mem, lookups);
        release_definition_tables(session->mem, staged);
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
    bool indexed = session->one_shot;
    if (!indexed) {
        // Clean children are indexed against the reference table alone: only a
        // vanished definition paragraph leaves a restart anchor the tree no
        // longer holds, and a footnote definition is a block that never
        // vanishes.
        indexed = id_table_build(session->mem, root, &ids) &&
                  markdown_core_session_index_clean_children(session, root, map, &clean);
        for (s = 0; indexed && s < MARKDOWN_CORE_DEFINITION_TABLE_COUNT; s++) {
            indexed =
                markdown_core_session_index_definitions(session, staged[s].map, &staged[s].index, &staged[s].count);
            staged[s].capacity = staged[s].count ? staged[s].count : 1;
        }
    }
    if (!indexed) {
        id_table_release(session->mem, &ids);
        if (clean.items) {
            session->mem->free(session->mem, clean.items);
        }
        markdown_core_footnote_index_release(session->mem, &footnotes);
        release_lookup_tables(session->mem, lookups);
        release_definition_tables(session->mem, staged);
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
    release_lookup_tables(session->mem, session->lookups);
    release_definition_tables(session->mem, session->definitions);
    if (session->clean.items) {
        session->mem->free(session->mem, session->clean.items);
    }
    memcpy(session->definitions, staged, sizeof(staged));
    session->definitions_stale = false;
    session->view.root = root;
    session->ids = ids;
    session->footnotes = footnotes;
    memcpy(session->lookups, lookups, sizeof(lookups));
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
        changes->lineage = session->lineage;
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

    // Sticky allocation loss in either committed table forces the full path,
    // which rebuilds both. The maps themselves are not checked for presence: a
    // committed `view.root` exists only where a full commit installed both,
    // and a parse that lost a map is poisoned and never produces a root.
    if (!initial && session->view.root && !session->definitions_stale &&
        !session->definitions[MARKDOWN_CORE_DEFINITIONS_REFERENCES].map->oom &&
        !session->definitions[MARKDOWN_CORE_DEFINITIONS_FOOTNOTES].map->oom) {
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

// One 64-bit read from the host CSPRNG. Sessions stay free of any library-
// owned RNG state: every source below is the platform's own, shared-nothing
// entropy service.
static bool session_host_entropy(uint64_t *value) {
#if defined(__EMSCRIPTEN__) || defined(__wasi__)
    // Standalone WASM lowers getentropy to the WASI random_get import; hosts
    // without the import report failure here instead of trapping.
    return getentropy(value, sizeof(*value)) == 0;
#elif defined(__APPLE__)
    arc4random_buf(value, sizeof(*value));
    return true;
#elif defined(_WIN32)
    unsigned int low = 0;
    unsigned int high = 0;
    if (rand_s(&low) != 0 || rand_s(&high) != 0) {
        return false;
    }
    *value = ((uint64_t)high << 32) | (uint64_t)low;
    return true;
#else
    FILE *source = fopen("/dev/urandom", "rb");
    bool complete = false;
    if (source) {
        complete = fread(value, sizeof(*value), 1, source) == 1;
        fclose(source);
    }
    return complete;
#endif
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
    {
        // PERMISSIVE_BYTES, and not as a default: the store must hold NUL and
        // invalid UTF-8 verbatim, because the parser replaces them per line
        // exactly as the one-shot path does, and because a streamed append
        // completes a multi-byte character whose first bytes arrived earlier.
        // A validating profile would reject those intermediate states at the
        // substrate, which is the wrong layer to decide them.
        markdown_core_source_stats scratch;
        markdown_core_source_status status;
        memset(&scratch, 0, sizeof(scratch));
        session->source =
            markdown_core_source_new(mem, MARKDOWN_CORE_SOURCE_PERMISSIVE_BYTES, NULL, 0, &scratch, &status);
        if (!session->source) {
            markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate session");
            return NULL;
        }
    }
    session->next_id = 1;
    session->revision = 0;
    session->record_lookups = true;

    // The address/time/clock mix alone is deterministic for the first
    // session of lockstep-started isolated runtimes (one WASM instance per
    // worker reproduces the same allocator state and coarse clocks), so the
    // host CSPRNG carries the cross-runtime uniqueness contract. The local
    // mix stays folded in as a best-effort fallback when the host read
    // fails.
    uint64_t entropy = (uint64_t)(uintptr_t)session;
    uint64_t host_entropy = 0;
    entropy ^= markdown_core_mix64((uint64_t)time(NULL));
    entropy ^= markdown_core_mix64((uint64_t)clock()) << 1;
    if (session_host_entropy(&host_entropy)) {
        entropy ^= host_entropy;
    }
    session->lineage = markdown_core_mix64(entropy);

    if (!commit_internal(session, true, NULL, error)) {
        markdown_core_session_free(session);
        return NULL;
    }
    return session;
}

markdown_core_session *markdown_core_session_open(
    const markdown_core_parse_options *options,
    markdown_core_error **error
) {
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
    markdown_core_source_release(session->source);
    id_table_release(session->mem, &session->ids);
    markdown_core_footnote_index_release(session->mem, &session->footnotes);
    release_lookup_tables(session->mem, session->lookups);
    release_definition_tables(session->mem, session->definitions);
    if (session->clean.items) {
        session->mem->free(session->mem, session->clean.items);
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
    if (byte_start > byte_end || byte_end > markdown_core_source_length(session->source)) {
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
    {
        // One edit per call, because that is what the convenience API takes.
        // The substrate accepts a batch, so a caller with several edits pays
        // one path copy for all of them; a keystroke pays one for itself.
        markdown_core_source_edit edit;
        markdown_core_source_stats stats;
        markdown_core_source_status status;
        markdown_core_source *next;

        edit.span.start = byte_start;
        edit.span.end = byte_end;
        edit.replacement = bytes;
        edit.replacement_length = length;
        memset(&stats, 0, sizeof(stats));
        next = markdown_core_source_apply(session->source, &edit, 1, &stats, &status);
        if (!next) {
            // The span was validated above and the profile is permissive, so
            // the substrate's remaining refusals are all "this edit cannot be
            // stored" — a failed allocation or a length that size_t cannot
            // represent. Both are the allocation failure this call has always
            // reported, and both leave the previous source retained.
            markdown_core_ast_set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not apply the edit");
            return false;
        }
        markdown_core_source_release(session->source);
        session->source = next;
        session->edit_bytes_moved += stats.bytes_copied;
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
    return session ? markdown_core_source_length(session->source) : 0;
}

const markdown_core_node *markdown_core_session_node_by_id(
    const markdown_core_session *session,
    markdown_core_node_id id
) {
    if (!session || id == 0) {
        return NULL;
    }
    if (session->ids.capacity == 0) {
        return NULL;
    }
    size_t mask = session->ids.capacity - 1;
    size_t slot = (size_t)markdown_core_mix64(id) & mask;
    while (session->ids.slots[slot].id != 0) {
        if (session->ids.slots[slot].id == id) {
            return session->ids.slots[slot].node;
        }
        slot = (slot + 1) & mask;
    }
    return NULL;
}
