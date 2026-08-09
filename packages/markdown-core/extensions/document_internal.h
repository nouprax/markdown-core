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
typedef struct {
    markdown_core_node_id id; // 0 marks an empty slot
    markdown_core_node *node;
} markdown_core_id_slot;

typedef struct {
    markdown_core_id_slot *slots;
    size_t capacity; // power of two, 0 when unallocated
    size_t count;
} markdown_core_id_table;

typedef struct {
    markdown_core_node_id *ids;
    size_t count;
    size_t capacity;
} markdown_core_id_array;

struct markdown_core_delta {
    uint64_t lineage;
    uint64_t before;
    uint64_t after;
    markdown_core_id_array added;
    markdown_core_id_array removed;
    markdown_core_id_array changed;
    markdown_core_id_array bubbled;
};

// One record per footnote node (reference or definition) of the committed
// tree, held in the open-addressing id table below. `group` indexes the
// in-use group of the record's label (SIZE_MAX when the label is unused or
// unresolvable).
typedef struct {
    markdown_core_node *node; // borrowed from the committed tree
    markdown_core_footnote_info info;
    size_t group;
} markdown_core_footnote_record;

// Open-addressing node-id -> footnote-record table (0 marks an empty slot,
// UINT64_MAX a tombstone; ids start at 1). Kept in step with the site lists
// by targeted removes/inserts on sequence-preserving commits and rebuilt
// wholesale otherwise; growth and tombstone compaction happen only through
// the fallible reserve step, so applies never allocate.
typedef struct {
    markdown_core_node_id *keys;
    markdown_core_footnote_record *records;
    size_t capacity; // power of two, 0 when unallocated
    size_t count;    // live records
    size_t occupied; // live + tombstones
} markdown_core_footnote_table;

// One footnote node with the containment facts the incremental splice
// classifies by: the document child whose subtree holds it, the
// inline-owning leaf whose parse produced it (references), the node's
// interned label, and its position inside the label's reference group.
typedef struct {
    markdown_core_node *node;   // borrowed from the committed tree
    markdown_core_node *anchor; // document child whose subtree holds `node`
    markdown_core_node *unit;   // nearest block ancestor for references, NULL for definitions
    size_t label;               // slot in the index's label table, SIZE_MAX unresolvable
    size_t group_pos;           // references: index inside the label's group run
} markdown_core_footnote_site;

typedef struct {
    markdown_core_footnote_site *items;
    size_t count;
    size_t capacity;
} markdown_core_footnote_site_list;

// Session-persistent label interning: one owned normalized string per
// distinct label ever seen, folded once when a site first carries it. Slots
// are stable for the session's lifetime — index generations, full rebuilds,
// and failed commits never move or free them — so site label fields stay
// valid across every path. Aggregates (winner, numbering) are derived per
// refresh and live only in the index's structures.
typedef struct {
    unsigned char **normalized; // owned NUL-terminated keys
    size_t count;
    size_t capacity;
    markdown_core_key_index by_label; // normalized label -> slot + 1
} markdown_core_footnote_labels;

// Session-maintained footnote index: numbering, first-use order, resolution
// state, and back-reference ordinals are answered from here; the tree stays
// source-faithful. Maintained across commits: a sequence-preserving commit
// (the stale and staged site runs carry identical label sequences) patches
// node pointers and churned ids in place, any other commit rebuilds the
// derived structures from the spliced site lists — label-slot integers, no
// re-normalization — and bumps the revisions of nodes whose query answers
// changed. The full parse path rebuilds the index wholesale; the session's
// label table persists throughout.
typedef struct {
    markdown_core_footnote_site_list defs; // definition sites in document order
    markdown_core_footnote_site_list refs; // reference sites in document order
    markdown_core_footnote_table records;
    markdown_core_node_id *in_use; // winning definitions in first-use order
    size_t in_use_count;
    markdown_core_node_id *references; // reference ids grouped by in_use entry, document order
    size_t *reference_offsets;         // in_use_count + 1 entries
} markdown_core_footnote_index;

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
    uint64_t next_id;            // monotonic, starts at 1, never reused
    uint64_t lineage;
    uint64_t revision;
    markdown_core_id_table ids;
    markdown_core_footnote_index footnotes;
    markdown_core_footnote_labels footnote_labels;
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
    // The incremental pipeline reconciled definitions in place and then could
    // not finish: a table no longer matches the committed tree, so the next
    // commit must take the full path (which rebuilds both tables and clears
    // this). One flag for both, because the full path is wholesale either way.
    bool definitions_stale;
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
    // Restart-locality inventory (white-box, asserted by fallback_runner):
    // full-reparse commits, incremental restarts, and restarts that
    // reflowed at a boundary. Degraded-to-full cases stay counted instead
    // of accumulating silently.
    size_t full_commits;
    size_t restarted_commits;
    size_t reflowed_commits;
    // Dependent units reparsed because a definition flip selected them
    // through the label postings, summed over successful incremental
    // commits. The definition-flip gate asserts this equals the mention
    // count of the flipped label's own kind, independent of document size.
    size_t dependent_reparses;
    // Stored bytes the substrate copied to apply edits, summed over every
    // successful edit (white-box, asserted by fallback_runner). It used to
    // answer "is an edit O(edit)?", and the gate over it required the
    // per-edit figure to stay FLAT as the document doubled — 11.1's removed
    // work bound, and the one thing a persistent rope was here to satisfy.
    // fallback_runner's edit_locality gate now divides by the document: a
    // splice in place moves a suffix, which is a constant FRACTION of the
    // document (0.500 from the midpoint) and must stay one. That is 11.1's
    // surviving requirement — no commit may be quadratic — measured in bytes
    // rather than in seconds.
    size_t edit_bytes_moved;
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

/** Runs the canonical adoption machine over one owner's complete inline
 * ownership domain while keeping the semantic owner stable. Every child of
 * `old_owner` and `staged_owner` belongs to the corresponding domain. On
 * success the staged owner and descendants carry their final ids/revisions,
 * and `owner_revision` reports the stable owner's
 * changed/bubbled/unchanged verdict. */
bool markdown_core_diff_trees_inline_domain(
    markdown_core_document *session,
    markdown_core_node *old_owner,
    markdown_core_node *staged_owner,
    uint64_t new_rev,
    markdown_core_delta *changes,
    uint64_t *owner_revision
);

/** Records every facade-visible node of `root`'s subtree as removed in
 * `changes` (NULL changes: a no-op). Returns false on allocation failure. */
bool markdown_core_document_record_removed(
    markdown_core_document *session,
    const markdown_core_node *root,
    markdown_core_delta *changes
);

/** Appends an id to a delta array; plain-malloc grow. */
bool markdown_core_id_array_push(markdown_core_id_array *array, markdown_core_node_id id);

/** Grows a delta array so the next `extra` pushes cannot fail. */
bool markdown_core_id_array_reserve(markdown_core_id_array *array, size_t extra);

/** Builds the footnote index for `root` into `index` (zeroed on entry by the
 * caller), interning labels into the session's persistent table. Returns
 * false on allocation failure with `index` fully released. */
bool markdown_core_footnote_index_build(
    markdown_core_document *session,
    markdown_core_node *root,
    markdown_core_footnote_index *index
);

/** Interns `label` into the session's footnote-label table, returning its
 * slot; SIZE_MAX with *failed clear when the label normalizes to nothing
 * (it can never participate), SIZE_MAX with *failed set on allocation
 * loss. Idempotent — a failed commit leaves at worst unused slots. */
size_t markdown_core_document_footnote_label(
    markdown_core_document *session,
    const markdown_core_chunk *label,
    bool *failed
);

/** Stamps interned label slots on every site of both lists (used for
 * freshly collected sites, whose labels are not yet resolved). Returns
 * false on allocation failure. */
bool markdown_core_document_footnote_label_sites(
    markdown_core_document *session,
    markdown_core_footnote_site_list *defs,
    markdown_core_footnote_site_list *refs
);

/** Frees the session's footnote-label table. */
void markdown_core_footnote_labels_release(markdown_core_mem *mem, markdown_core_footnote_labels *labels);

/** Looks up a record by node id, NULL when absent. */
markdown_core_footnote_record *markdown_core_footnote_table_find(
    const markdown_core_footnote_table *table,
    markdown_core_node_id id
);

/** Grows/compacts the record table so the next `extra` inserts cannot fail
 * (fallible; run before the point of no return). */
bool markdown_core_footnote_table_reserve(markdown_core_mem *mem, markdown_core_footnote_table *table, size_t extra);

/** Inserts a record within a reserved budget (never fails). The id must not
 * be present. */
void markdown_core_footnote_table_put(markdown_core_footnote_table *table, markdown_core_footnote_record record);

/** Tombstones a record (missing ids are a no-op). */
void markdown_core_footnote_table_remove(markdown_core_footnote_table *table, markdown_core_node_id id);

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

/** Builds the index's derived structures from document-ordered site lists
 * whose label slots are already interned, taking ownership of both lists'
 * storage (they are zeroed; on failure the storage is freed with the rest
 * of the index). Label-slot integer work only — nothing here folds or
 * hashes label text. Node ids must be final. */
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
    markdown_core_delta *changes,
    size_t owed_bubbled
);

/** Creates a parser configured with the session's options and extensions.
 * Returns NULL on allocation or extension-registry failure with *error set
 * when non-NULL. Defined in session.c. */
markdown_core_parser *markdown_core_document_new_parser(markdown_core_document *session, markdown_core_error **error);

/** Takes the session's warm parser when one is held, else creates one like
 * markdown_core_document_new_parser. Defined in session.c. */
markdown_core_parser *markdown_core_document_acquire_parser(markdown_core_document *session, markdown_core_error **error);

/** Hands a parser back after its parse ended: a healthy one is renewed and
 * held warm for the next commit, a poisoned one (or a second hand-back) is
 * freed. The parser's definition maps must be its own or NULL — never the
 * session's. Defined in session.c. */
void markdown_core_document_release_parser(markdown_core_document *session, markdown_core_parser *parser);


/** Grows the id table so the next `extra` markdown_core_document_ids_put
 * calls cannot fail. */
bool markdown_core_document_ids_reserve(markdown_core_document *session, size_t extra);

/** Points `id` at `node`, inserting or repointing. Never fails within a
 * reserved budget. */
void markdown_core_document_ids_put(markdown_core_document *session, markdown_core_node_id id, markdown_core_node *node);

/** Drops `id` from the table (backward-shift deletion; missing ids are a
 * no-op). */
void markdown_core_document_ids_remove(markdown_core_document *session, markdown_core_node_id id);

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
markdown_core_incremental_result markdown_core_document_commit_incremental(
    markdown_core_document *session,
    uint64_t new_rev,
    markdown_core_delta *changes,
    markdown_core_error **error
);

#endif
