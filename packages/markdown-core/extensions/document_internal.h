#ifndef MARKDOWN_CORE_SESSION_INTERNAL_H
#define MARKDOWN_CORE_SESSION_INTERNAL_H

#include "../include/markdown_core.h"
#include "arena.h"
#include "ast_internal.h"

#include <map.h>
#include <markdown-core.h>

#include "source.h"

// AddressSanitizer detection: session pooling is bypassed under ASan so the
// sanitizer keeps seeing individual allocations (see session_open_with_mem).
#ifndef MARKDOWN_CORE_ASAN
#if defined(__SANITIZE_ADDRESS__)
#define MARKDOWN_CORE_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MARKDOWN_CORE_ASAN 1
#else
#define MARKDOWN_CORE_ASAN 0
#endif
#else
#define MARKDOWN_CORE_ASAN 0
#endif
#endif

/** SplitMix64 finalizer shared by the session's open-addressing tables. */
static inline uint64_t markdown_core_mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

// Open-addressing id -> node table. Rebuilt lazily after a commit; keys are
// session-unique node ids (0 marks an empty slot, ids start at 1). Id and
// node share a slot so every probe costs one cache line, not two — the
// table dwarfs the cache at document scale and probes dominate the
// commit's table maintenance.

struct markdown_core_delta {
    uint64_t lineage;
    uint64_t before;
    uint64_t after;
    markdown_core_diff *diffs;
    size_t count;
    size_t capacity;
};

// (hits and misses alike: every lookup is an answer a definition edit can
// change). Labels are owned NUL-terminated strings; `positions` runs
// parallel to `labels` and holds each label's entry position inside its
// posting, so removal fixes the posting in O(1).
typedef struct {
    unsigned char **labels;
    size_t *positions;
    size_t count;
} markdown_core_lookup_record;

// One posting entry: a unit whose record holds the posting's label, plus the
// label's ordinal inside that record so a swap-remove can repoint the moved
// entry's stored position.
typedef struct {
    markdown_core_node_id unit;
    size_t ordinal;
} markdown_core_lookup_posting_entry;

// Every unit that recorded a lookup of one label, unordered. The label key
// is an owned copy with posting lifetime; a posting that empties stays and
// is reused when its label returns, mirroring the footnote label interning
// lifetime rules (slots never move or free for the session's lifetime).
typedef struct {
    unsigned char *label; // owned NUL-terminated copy
    markdown_core_lookup_posting_entry *items;
    size_t count;
    size_t capacity;
    size_t staged; // this commit's pending appends, tallied by the reserve
} markdown_core_lookup_posting;

// Inverted index over the lookup table: label -> the units that looked it
// up. A commit whose definition reconciliation changed per-label winners
// walks the changed labels' postings, so dependent collection costs
// O(affected units), not O(units with lookups).
typedef struct {
    markdown_core_lookup_posting *items;
    size_t count;
    size_t capacity;
    markdown_core_key_index by_label; // label -> posting index + 1
} markdown_core_lookup_postings;

// Open-addressing unit-id -> lookup-record table, persistent across commits
// (0 marks an empty slot, ids start at 1), plus the label->units postings
// maintained by every put and remove.
typedef struct {
    markdown_core_node_id *keys;
    markdown_core_lookup_record *records;
    size_t capacity; // power of two, 0 when unallocated
    size_t count;
    markdown_core_lookup_postings postings;
} markdown_core_lookup_table;

// One observed lookup, keyed by the attribution node pointer while ids are
// still unassigned (adoption resolves them later, like definition owners).
// `kind` is the definition table the lookup ran against
// (markdown_core_definition_kind): the label text is raw normalized bytes,
// and bundling partitions events into that kind's per-table output.
typedef struct {
    markdown_core_node *unit;
    unsigned char *label; // owned
    size_t kind;          // markdown_core_definition_kind of the queried map
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
// These are the only safe incremental restart and reflow points; children
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

// One kind of document-scoped definition, as the session keeps it: the map
// plus the line-ordered index over that map. Link reference definitions and
// footnote definitions get one table each.
//
// Two tables rather than one map with a kind discriminator on the entry. A
// shared map puts `[x]:` and `[^x]:` in the same label bucket, and the commit
// reconciler decides what to re-refine by comparing that bucket's winner
// before and after: a footnote definition appearing while a link definition
// of the same label stays put leaves the winner "still present", so every
// unit that read `[^x]` keeps a tree that is now wrong. That is an
// under-invalidation — the failure mode a kind-filtered lookup would only
// paper over, because the two definitions would still have to share one
// winner election. In separate tables they never meet and the state cannot be
// written down.
//
// Nothing is duplicated to buy that: the coordination chain takes the table it
// operates on and runs once per table. One mechanism, two instances.
typedef struct {
    // Session-persistent definitions. Each entry carries the id of the
    // document child anchoring it (0 = the region before the first child), so
    // a commit retracts exactly the definitions whose bytes it reparses. At
    // rest every entry's `order` stems from the most recent full parse, so
    // per-label winner election sees true document order.
    markdown_core_map *map;
    // At-rest entries ordered by start line (equal to document order):
    // staleness and prefix/suffix classification are line-interval range
    // queries, and a commit's stale range splices in place. Rebuilt by the
    // full path; an aborted reconciliation is covered by definitions_stale.
    markdown_core_map_entry **index;
    size_t count;
    size_t capacity;
} markdown_core_definition_table;

// The instances. Indices, not named fields, because every step of the
// coordination chain is written once and run per table.
typedef enum {
    MARKDOWN_CORE_DEFINITIONS_REFERENCES = 0,
    MARKDOWN_CORE_DEFINITIONS_FOOTNOTES = 1,
    MARKDOWN_CORE_DEFINITION_TABLE_COUNT = 2,
} markdown_core_definition_kind;

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

struct markdown_core_document {
    markdown_core_mem *mem;
    markdown_core_parse_options options;
    // The session's bytes: one growable buffer, mutable and singly owned, and
    // an edit splices it in place (source.h). It was a persistent rope, and
    // both reasons it gave for being one are gone — the bounded-neighbourhood
    // copy was 11.1's removed work bound, and the readable predecessor was
    // 4.2's removed clause. A session hands out one document, reused in place,
    // so nothing can hold a predecessor to read.
    markdown_core_source *source;
    markdown_core_node *root; // the committed tree, owned
    // What an editor underlines, in source order, owned. Taken from the
    // parser when this document takes its tree, so it describes exactly the
    // committed text and is replaced wholesale by the next edit.
    markdown_core_diagnostic *diagnostics;
    size_t diagnostic_count;
    uint64_t next_id; // monotonic, starts at 1, never reused
    uint64_t lineage;
    uint64_t revision;
    // The definition tables (see markdown_core_definition_table).
    markdown_core_definition_table definitions[MARKDOWN_CORE_DEFINITION_TABLE_COUNT];
    // Persistent unit-id -> looked-up-labels tables backing per-unit re-runs
    // when a commit changes per-label winners. One instance per definition
    // kind, mirroring the definition tables: the two label key spaces are
    // disjoint by construction (incremental-canonical-ast.md 6.3 step 0), so
    // labels inside each table are raw normalized bytes. Maintained by both
    // commit paths; skipped entirely for the one-shot convenience parse.
    markdown_core_lookup_table lookups[MARKDOWN_CORE_DEFINITION_TABLE_COUNT];
    bool record_lookups;
    markdown_core_clean_index clean;
    markdown_core_edit_summary pending;
    int total_lines;      // parser line count of the committed text
    int last_line_length; // parser's final-line length of the committed text
    // One warm parser held between commits: staged parses are
    // per-commit, but the parser shell (struct, line buffers, empty
    // reference map, extension attachments) is commit-invariant, so
    // commits skip rebuilding it. NULL when no healthy parser came back.
    markdown_core_parser *warm_parser;
    // When pooled, every session-owned allocation flows through this arena
    // (session->mem is its allocator face) and teardown is a wholesale
    // release. NULL for unpooled sessions: the one-shot parse (its detached
    // tree must outlive the session and Document.parse keeps its v1 memory
    // profile) and the ASan suites.
    markdown_core_arena *arena;
};

/** Internal constructor used by allocation-injection tests and the one-shot
 * parse; the public markdown_core_document_open uses the default allocator
 * with pooling. `pooled` routes every session-owned allocation through a
 * session arena over `mem` — pass false when detached nodes must outlive
 * the session or when injection needs to see individual allocations. */
markdown_core_document *markdown_core_document_open_with_mem(
    const markdown_core_parse_options *options,
    markdown_core_mem *mem,
    bool pooled,
    markdown_core_error **error
);

/** Which parts of two same-raw-type nodes' projections differ: VALUE for
 * kind and scalar fields, TEXT for canonical text bytes (9.1). Allocation
 * failure reports "differs" so a revision bump can never be missed. Defined
 * in ast.c next to the dump implementation, which reads the same fields. */
uint32_t markdown_core_ast_parts_changed(const markdown_core_node *a, const markdown_core_node *b);

/** Which parts a node HAS, which is what a created node carries: every node
 * has VALUE; TEXT belongs to the kinds with canonical text bytes; CHILDREN
 * and DESCENDANT to a node that has children; ANSWERS to the kinds that are
 * addressed by a parser answer (4.1). */
uint32_t markdown_core_ast_parts_present(const markdown_core_node *node);

/** Adopts ids from `old_root` (may be NULL) onto `new_root`, assigns
 * last_changed_rev = new_rev to every added/changed/bubbled node, carries the
 * old revision over for untouched subtrees, and records facade-visible ids
 * into `changes` when non-NULL. Returns false on allocation failure while
 * recording (the trees are left consistent; the caller discards `new_root`).
 */
/** DIFF: assigns `nw`'s identities from `old` (which may be NULL) and reports
 * what changed. Reads no text; reparses nothing. A pure function of two trees,
 * which is what lets the parse be a pure function of (bytes, options). */
bool markdown_core_document_diff(
    const markdown_core_document *old,
    markdown_core_document *nw,
    markdown_core_delta *changes,
    markdown_core_error **error
);

bool markdown_core_diff_trees(
    markdown_core_document *session,
    markdown_core_node *old_root,
    markdown_core_node *new_root,
    uint64_t new_rev,
    markdown_core_delta *changes
);

/** Appends one row to a delta's `diffs`; plain-malloc grow. */
bool markdown_core_delta_push(markdown_core_delta *changes, markdown_core_node_id id, uint32_t parts);

/** Creates a parser configured with the session's options and extensions.
 * Returns NULL on allocation or extension-registry failure with *error set
 * when non-NULL. Defined in session.c. */
markdown_core_parser *markdown_core_document_new_parser(markdown_core_document *session, markdown_core_error **error);

/** Takes the session's warm parser when one is held, else creates one like
 * markdown_core_document_new_parser. Defined in session.c. */
markdown_core_parser *markdown_core_document_acquire_parser(
    markdown_core_document *session,
    markdown_core_error **error
);

/** Hands a parser back after its parse ended: a healthy one is renewed and
 * held warm for the next commit, a poisoned one (or a second hand-back) is
 * freed. The parser's definition maps must be its own or NULL — never the
 * session's. Defined in session.c. */
void markdown_core_document_release_parser(markdown_core_document *session, markdown_core_parser *parser);

/** Rewrites every definition owner stamped as a node pointer during the
 * just-adopted parse to that node's session id (owner 0 stays 0: the region
 * before the first document child). Owners already holding ids are never
 * present when this runs — full parses replace the whole map, incremental
 * commits remove pointer-stamped duplicates instead. Runs per definition
 * table. */
void markdown_core_document_resolve_definition_owners(markdown_core_map *map);

/** Rebuilds the session's line-ordered at-rest definition index from `map`
 * into caller-provided storage (swapped in by the full commit path). */
bool markdown_core_document_index_definitions(
    markdown_core_document *session,
    markdown_core_map *map,
    markdown_core_map_entry ***out_items,
    size_t *out_count
);

/** Builds the clean-child index for a freshly sealed tree into `out` (zeroed
 * by the caller) by scanning the session's stored text for line starts.
 * O(text); used by full commits only — incremental commits update the index
 * from their own restart bookkeeping. Returns false on allocation failure
 * with `out` released. Defined in incremental.c. */
bool markdown_core_document_index_clean_children(
    markdown_core_document *session,
    markdown_core_node *root,
    const markdown_core_map *map,
    markdown_core_clean_index *out
);

/** Prepares an empty recording bound to `mem`. */
void markdown_core_lookup_recording_init(markdown_core_lookup_recording *recording, markdown_core_mem *mem);

/** Frees the recording's events and any labels not yet moved out. */
void markdown_core_lookup_recording_release(markdown_core_lookup_recording *recording);

/** The map lookup sinks (markdown_core_map_lookup_sink); `context` is the
 * recording, `unit` the attribution node. Each parser map is armed with the
 * sink of its own definition kind, which the shared worker stores on the
 * event structurally — labels stay raw normalized bytes. Consecutive
 * same-unit duplicates of one kind are dropped; allocation loss sets `lost`
 * instead of failing the parse. */
void markdown_core_lookup_recording_sink_references(void *context, void *unit, const unsigned char *label);
void markdown_core_lookup_recording_sink_footnotes(void *context, void *unit, const unsigned char *label);

/** Groups the recording into per-unit bundles partitioned by definition
 * kind in one pass, moving label ownership. A unit appears in a kind's
 * array only if it recorded events of that kind. Returns false on
 * allocation failure; releasing the recording and any partial bundles
 * stays safe either way. */
bool markdown_core_lookup_recording_bundle(
    markdown_core_lookup_recording *recording,
    markdown_core_unit_lookups *bundles[MARKDOWN_CORE_DEFINITION_TABLE_COUNT],
    size_t bundle_count[MARKDOWN_CORE_DEFINITION_TABLE_COUNT]
);

/** Frees `count` bundles and every label still owned by them. */
void markdown_core_unit_lookups_free(markdown_core_mem *mem, markdown_core_unit_lookups *bundles, size_t count);

/** Frees every record and the table's storage; zeroes the table. */
void markdown_core_lookup_table_release(markdown_core_mem *mem, markdown_core_lookup_table *table);

/** Grows the table so the next `extra` puts cannot fail. */
bool markdown_core_lookup_table_reserve(markdown_core_mem *mem, markdown_core_lookup_table *table, size_t extra);

/** Ensures the postings can absorb every label of `bundles` without
 * allocating, so the puts inside the transactional splice cannot fail.
 * Creates empty postings (and interns their label keys) for labels the
 * table has never seen; those persist even if the commit later fails,
 * which is harmless — an empty posting carries no dependency answer. */
bool markdown_core_lookup_postings_reserve(
    markdown_core_mem *mem,
    markdown_core_lookup_table *table,
    const markdown_core_unit_lookups *bundles,
    size_t bundle_count
);

/** The posting for `label`, or NULL when no unit ever recorded it. */
const markdown_core_lookup_posting *markdown_core_lookup_postings_find(
    const markdown_core_lookup_table *table,
    const unsigned char *label
);

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
 * with reflow, suffix transplant, id adoption, footnote refresh, seal).
 * Transactional: on FAILED or FALLBACK the committed tree, id table, refmap,
 * footnote index, and geometry are exactly as before the call. Defined in
 * incremental.c. */
markdown_core_incremental_result markdown_core_document_edit_incremental(
    markdown_core_document *session,
    uint64_t new_rev,
    markdown_core_delta *changes,
    markdown_core_error **error
);

#endif
