#ifndef MARKDOWN_CORE_SESSION_INTERNAL_H
#define MARKDOWN_CORE_SESSION_INTERNAL_H

#include "../include/markdown_core.h"
#include "ast_internal.h"

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

struct markdown_core_changeset {
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

// Session-maintained footnote index: numbering, first-use order, resolution
// state, and back-reference ordinals are answered from here; the tree stays
// source-faithful. Rebuilt on every commit and diffed against the previous
// index to bump the revisions of nodes whose query answers changed.
typedef struct {
    markdown_core_footnote_record *records; // sorted by node id
    size_t record_count;
    markdown_core_node_id *in_use; // winning definitions in first-use order
    size_t in_use_count;
    markdown_core_node_id *references; // reference ids grouped by in_use entry, document order
    size_t *reference_offsets;         // in_use_count + 1 entries
} markdown_core_footnote_index;

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
};

/** Internal constructor used by allocation-injection tests; the public
 * markdown_core_session_open uses the default allocator. */
markdown_core_session *markdown_core_session_open_with_mem(const markdown_core_parse_options *options,
                                                           markdown_core_mem *mem, markdown_core_error **error);

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
bool markdown_core_session_adopt(markdown_core_session *session, markdown_core_node *old_root,
                                 markdown_core_node *new_root, uint64_t new_rev, markdown_core_changeset *changes);

/** Appends an id to a changeset array; plain-malloc grow. */
bool markdown_core_id_array_push(markdown_core_id_array *array, markdown_core_node_id id);

/** Builds the footnote index for `root` into `index` (zeroed on entry by the
 * caller). Returns false on allocation failure with `index` fully released. */
bool markdown_core_footnote_index_build(markdown_core_mem *mem, markdown_core_node *root,
                                        markdown_core_footnote_index *index);

/** Releases everything owned by `index` and zeroes it. */
void markdown_core_footnote_index_release(markdown_core_mem *mem, markdown_core_footnote_index *index);

/** Diffs `next` against `previous` by node id and bumps the revision of
 * every node whose query answers changed but whose dump content did not:
 * the node is recorded as `changed`, untouched ancestors as `bubbled`.
 * Returns false when a changeset array could not grow. */
bool markdown_core_footnote_index_diff(const markdown_core_footnote_index *previous,
                                       const markdown_core_footnote_index *next, uint64_t new_rev,
                                       markdown_core_changeset *changes);

#endif
