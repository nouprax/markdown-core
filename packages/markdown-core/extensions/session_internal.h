#ifndef MARKDOWN_CORE_SESSION_INTERNAL_H
#define MARKDOWN_CORE_SESSION_INTERNAL_H

#include "../include/markdown_core.h"
#include "ast_internal.h"

#include <map.h>
#include <markdown-core.h>
#include <text.h>

// Open-addressing id -> node table. Rebuilt lazily after a commit; keys are
// session-unique node ids (0 marks an empty slot, ids start at 1).
typedef struct {
    markdown_core_node_id *keys;
    markdown_core_node **values;
    size_t capacity; // power of two, 0 when unallocated
    size_t count;
} markdown_core_id_table;

typedef struct {
    markdown_core_node_id *ids;
    size_t count;
    size_t capacity;
} markdown_core_id_array;

struct markdown_core_delta {
    uint64_t before;
    uint64_t after;
    markdown_core_id_array added;
    markdown_core_id_array removed;
    markdown_core_id_array changed;
    markdown_core_id_array bubbled;
};

// One record per footnote node (reference or definition) of the committed
// tree, sorted by node id. `group` indexes the in-use group of the record's
// label (SIZE_MAX when the label is unused or unresolvable).
typedef struct {
    markdown_core_node *node; // borrowed from the committed tree
    markdown_core_footnote_info info;
    size_t group;
} markdown_core_footnote_record;

// One footnote node with the containment facts the incremental merge
// classifies by: the document child whose subtree holds it, and (for
// references) the inline-owning leaf whose parse produced it.
typedef struct {
    markdown_core_node *node;   // borrowed from the committed tree
    markdown_core_node *anchor; // document child whose subtree holds `node`
    markdown_core_node *unit;   // nearest block ancestor for references, NULL for definitions
} markdown_core_footnote_site;

typedef struct {
    markdown_core_footnote_site *items;
    size_t count;
    size_t capacity;
} markdown_core_footnote_site_list;

// Session-maintained footnote index: numbering, first-use order, resolution
// state, and back-reference ordinals are answered from here; the tree stays
// source-faithful. Rebuilt every commit from the document-ordered site
// lists — collected by a tree walk on the full path, merged in place by
// incremental commits — and diffed against the previous index to bump the
// revisions of nodes whose query answers changed.
typedef struct {
    markdown_core_footnote_site_list defs;  // definition sites in document order
    markdown_core_footnote_site_list refs;  // reference sites in document order
    markdown_core_footnote_record *records; // sorted by node id
    size_t record_count;
    markdown_core_node_id *in_use; // winning definitions in first-use order
    size_t in_use_count;
    markdown_core_node_id *references; // reference ids grouped by in_use entry, document order
    size_t *reference_offsets;         // in_use_count + 1 entries
} markdown_core_footnote_index;

// Per-unit record of the normalized labels the unit's inline parse looked up
// (hits and misses alike: every lookup is an answer a definition edit can
// change). Labels are owned NUL-terminated strings.
typedef struct {
    unsigned char **labels;
    size_t count;
} markdown_core_lookup_record;

// Open-addressing unit-id -> lookup-record table, persistent across commits
// (0 marks an empty slot, ids start at 1). A commit whose definition
// reconciliation changed per-label winners scans it for the dependent units.
typedef struct {
    markdown_core_node_id *keys;
    markdown_core_lookup_record *records;
    size_t capacity; // power of two, 0 when unallocated
    size_t count;
} markdown_core_lookup_table;

// One observed lookup, keyed by the attribution node pointer while ids are
// still unassigned (adoption resolves them later, like definition owners).
typedef struct {
    markdown_core_node *unit;
    unsigned char *label; // owned
} markdown_core_lookup_event;

// Append-only observation list for one staged parse. `lost` poisons the
// recording: a commit that cannot trust its records must not commit
// incrementally.
typedef struct {
    markdown_core_mem *mem;
    markdown_core_lookup_event *items;
    size_t count;
    size_t capacity;
    bool lost;
} markdown_core_lookup_recording;

// A bundled per-unit record ready to install once the unit's id is final.
typedef struct {
    markdown_core_node *unit;
    markdown_core_lookup_record record;
} markdown_core_unit_lookups;

// One CLEAN_START document child of the committed tree, in document order.
// These are the only safe incremental restart and resync points; children
// without the flag are fused to their predecessor and always reparse with it.
typedef struct {
    size_t start_byte;        // byte offset of the child's first line, current text
    int start_line;           // absolute 1-based first line
    markdown_core_node *node; // borrowed from the committed tree
} markdown_core_clean_child;

typedef struct {
    markdown_core_clean_child *items;
    size_t count;
    size_t capacity;
} markdown_core_clean_index;

// Coalesced summary of the edits since the last successful commit: one dirty
// byte range in current-text coordinates plus the net length delta. Bytes
// before `new_lo` and at/after `new_hi` are byte-identical to the committed
// text (the old range is [new_lo, new_hi - delta)).
typedef struct {
    bool dirty;
    size_t new_lo;
    size_t new_hi;
    ptrdiff_t delta;
} markdown_core_edit_summary;

struct markdown_core_session {
    markdown_core_mem *mem;
    markdown_core_parse_options options;
    markdown_core_text text;
    markdown_core_document view; // view.root is the committed tree, owned
    uint64_t next_id;            // monotonic, starts at 1, never reused
    uint64_t lineage;
    uint64_t revision;
    markdown_core_id_table ids;
    markdown_core_footnote_index footnotes;
    // Session-persistent reference map (refmap v2): definitions carry the id
    // of the document child anchoring them (0 = the region before the first
    // child), so a commit retracts exactly the definitions whose bytes it
    // reparses. At rest every entry's `order` stems from the most recent
    // full parse, so per-label winner election sees true document order.
    markdown_core_map *refmap;
    // Persistent unit-id -> looked-up-labels table backing per-unit re-runs
    // when a commit changes per-label winners. Maintained by both commit
    // paths; skipped entirely for the one-shot convenience parse.
    markdown_core_lookup_table lookups;
    bool record_lookups;
    // The incremental pipeline reconciled definitions in place and then could
    // not finish: the map no longer matches the committed tree, so the next
    // commit must take the full path (which rebuilds the map and clears
    // this).
    bool refmap_stale;
    markdown_core_clean_index clean;
    markdown_core_edit_summary pending;
    int total_lines;      // parser line count of the committed text
    int last_line_length; // parser's final-line length of the committed text
    // Upper bound on the reference expansion a one-shot parse of the current
    // text would accumulate. While it stays within the one-shot budget,
    // incremental inline phases can run unlimited and still match the
    // one-shot dump byte for byte; beyond it commits fall back to a full
    // reparse, which resets the bound to the measured value.
    size_t expansion_estimate;
};

/** Internal constructor used by allocation-injection tests; the public
 * markdown_core_session_open uses the default allocator. */
markdown_core_session *markdown_core_session_open_with_mem(
    const markdown_core_parse_options *options,
    markdown_core_mem *mem,
    markdown_core_error **error
);

/** Compares the canonical dump fields of two nodes of the same raw type,
 * excluding scope. Allocation failure reports "not equal" so a revision bump
 * can never be missed. Defined in ast.c next to the dump implementation. */
bool markdown_core_ast_fields_equal(const markdown_core_node *a, const markdown_core_node *b);

/** Adopts ids from `old_root` (may be NULL) onto `new_root`, assigns
 * last_changed_rev = new_rev to every added/changed/bubbled node, carries the
 * old revision over for untouched subtrees, and records facade-visible ids
 * into `changes` when non-NULL. Returns false on allocation failure while
 * recording (the trees are left consistent; the caller discards `new_root`).
 */
bool markdown_core_session_adopt(
    markdown_core_session *session,
    markdown_core_node *old_root,
    markdown_core_node *new_root,
    uint64_t new_rev,
    markdown_core_delta *changes
);

/** Records every facade-visible node of `root`'s subtree as removed in
 * `changes` (NULL changes: a no-op). Returns false on allocation failure. */
bool markdown_core_session_record_removed(
    markdown_core_session *session,
    const markdown_core_node *root,
    markdown_core_delta *changes
);

/** Appends an id to a delta array; plain-malloc grow. */
bool markdown_core_id_array_push(markdown_core_id_array *array, markdown_core_node_id id);

/** Grows a delta array so the next `extra` pushes cannot fail. */
bool markdown_core_id_array_reserve(markdown_core_id_array *array, size_t extra);

/** Builds the footnote index for `root` into `index` (zeroed on entry by the
 * caller). Returns false on allocation failure with `index` fully released. */
bool markdown_core_footnote_index_build(
    markdown_core_mem *mem,
    markdown_core_node *root,
    markdown_core_footnote_index *index
);

/** Appends a site; plain doubling grow. */
bool markdown_core_footnote_site_push(
    markdown_core_mem *mem,
    markdown_core_footnote_site_list *list,
    markdown_core_footnote_site site
);

/** Frees the list's storage and zeroes it (the nodes are borrowed). */
void markdown_core_footnote_site_list_release(markdown_core_mem *mem, markdown_core_footnote_site_list *list);

/** Appends every footnote node of `root`'s subtree to `defs`/`refs` in
 * document order. Sites take `anchor` when non-NULL (a subtree that will sit
 * under one document child), or their own top-level ancestor below `root`.
 * Returns false on allocation failure; the lists stay releasable. */
bool markdown_core_footnote_collect_sites(
    markdown_core_mem *mem,
    markdown_core_node *root,
    markdown_core_node *anchor,
    markdown_core_footnote_site_list *defs,
    markdown_core_footnote_site_list *refs
);

/** Builds the index from document-ordered site lists, taking ownership of
 * both lists' storage (they are zeroed; on failure the storage is freed with
 * the rest of the index). Node ids must be final. */
bool markdown_core_footnote_index_build_sites(
    markdown_core_mem *mem,
    markdown_core_footnote_site_list *defs,
    markdown_core_footnote_site_list *refs,
    markdown_core_footnote_index *index
);

/** Releases everything owned by `index` and zeroes it. */
void markdown_core_footnote_index_release(markdown_core_mem *mem, markdown_core_footnote_index *index);

/** Diffs `next` against `previous` by node id and bumps the revision of
 * every node whose query answers changed but whose dump content did not:
 * the node is recorded as `changed`, untouched ancestors as `bubbled`.
 * Two-phase and transactional: on false (an allocation could not grow) no
 * node has been touched, so the diff may run against the live committed
 * tree. */
bool markdown_core_footnote_index_diff(
    markdown_core_mem *mem,
    const markdown_core_footnote_index *previous,
    const markdown_core_footnote_index *next,
    uint64_t new_rev,
    markdown_core_delta *changes
);

/** Creates a parser configured with the session's options and extensions.
 * Returns NULL on allocation or extension-registry failure with *error set
 * when non-NULL. Defined in session.c. */
markdown_core_parser *markdown_core_session_new_parser(markdown_core_session *session, markdown_core_error **error);

/** Seals a freshly parsed tree: positions become parent-relative deltas and
 * every node gains MARKDOWN_CORE_NODE__SEALED_RELATIVE. Defined in
 * session.c. */
void markdown_core_session_seal_positions(markdown_core_node *root);

/** Grows the id table so the next `extra` markdown_core_session_ids_put
 * calls cannot fail. */
bool markdown_core_session_ids_reserve(markdown_core_session *session, size_t extra);

/** Points `id` at `node`, inserting or repointing. Never fails within a
 * reserved budget. Directive-label wrappers are not addressable and must not
 * be put. */
void markdown_core_session_ids_put(markdown_core_session *session, markdown_core_node_id id, markdown_core_node *node);

/** Drops `id` from the table (backward-shift deletion; missing ids are a
 * no-op). */
void markdown_core_session_ids_remove(markdown_core_session *session, markdown_core_node_id id);

/** Rewrites every definition owner stamped as a node pointer during the
 * just-adopted parse to that node's session id (owner 0 stays 0: the region
 * before the first document child). Owners already holding ids are never
 * present when this runs — full parses replace the whole map, incremental
 * commits remove pointer-stamped duplicates instead. */
void markdown_core_session_resolve_definition_owners(markdown_core_map *map);

/** Builds the clean-child index for a freshly sealed tree into `out` (zeroed
 * by the caller) by scanning the session's stored text for line starts.
 * O(text); used by full commits only — incremental commits update the index
 * from their own restart bookkeeping. Returns false on allocation failure
 * with `out` released. Defined in incremental.c. */
bool markdown_core_session_index_clean_children(
    markdown_core_session *session,
    markdown_core_node *root,
    markdown_core_clean_index *out
);

// --- lookup records (lookups.c) ---------------------------------------------

/** Prepares an empty recording bound to `mem`. */
void markdown_core_lookup_recording_init(markdown_core_lookup_recording *recording, markdown_core_mem *mem);

/** Frees the recording's events and any labels not yet moved out. */
void markdown_core_lookup_recording_release(markdown_core_lookup_recording *recording);

/** The map lookup sink (markdown_core_map_lookup_sink); `context` is the
 * recording, `unit` the attribution node. Consecutive same-unit duplicates
 * are dropped; allocation loss sets `lost` instead of failing the parse. */
void markdown_core_lookup_recording_sink(void *context, void *unit, const unsigned char *label);

/** Groups the recording into per-unit bundles, moving label ownership.
 * Returns false on allocation failure; releasing the recording and any
 * partial bundles stays safe either way. */
bool markdown_core_lookup_recording_bundle(
    markdown_core_lookup_recording *recording,
    markdown_core_unit_lookups **out,
    size_t *out_count
);

/** Frees `count` bundles and every label still owned by them. */
void markdown_core_unit_lookups_free(markdown_core_mem *mem, markdown_core_unit_lookups *bundles, size_t count);

/** Frees every record and the table's storage; zeroes the table. */
void markdown_core_lookup_table_release(markdown_core_mem *mem, markdown_core_lookup_table *table);

/** Grows the table so the next `extra` puts cannot fail. */
bool markdown_core_lookup_table_reserve(markdown_core_mem *mem, markdown_core_lookup_table *table, size_t extra);

/** Installs `record` for `id`, replacing (and freeing) any previous record.
 * Never fails within a reserved budget; the table takes ownership. */
void markdown_core_lookup_table_put(
    markdown_core_mem *mem,
    markdown_core_lookup_table *table,
    markdown_core_node_id id,
    markdown_core_lookup_record record
);

/** Drops `id`'s record (backward-shift deletion; missing ids are a no-op). */
void markdown_core_lookup_table_remove(
    markdown_core_mem *mem,
    markdown_core_lookup_table *table,
    markdown_core_node_id id
);

typedef enum {
    MARKDOWN_CORE_INCREMENTAL_COMMITTED, // committed; *changes filled when requested
    MARKDOWN_CORE_INCREMENTAL_FALLBACK,  // not applicable; run the full path
    MARKDOWN_CORE_INCREMENTAL_FAILED     // allocation loss; session intact at the previous revision
} markdown_core_incremental_result;

/** Attempts the incremental commit pipeline (restart plan, staged reparse
 * with resync, suffix transplant, id adoption, footnote refresh, seal).
 * Transactional: on FAILED or FALLBACK the committed tree, id table, refmap,
 * footnote index, and geometry are exactly as before the call. Defined in
 * incremental.c. */
markdown_core_incremental_result markdown_core_session_commit_incremental(
    markdown_core_session *session,
    uint64_t new_rev,
    markdown_core_delta *changes,
    markdown_core_error **error
);

#endif
