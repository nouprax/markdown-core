#ifndef MARKDOWN_CORE_NODE_H
#define MARKDOWN_CORE_NODE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdint.h>

#include "node_type.h"
#include "markdown-core-element-api.h"
#include "buffer.h"
#include "chunk.h"
#include "attributes.h"
#include "slab.h"
#include "metadata.h"

typedef struct {
    markdown_core_list_flavor flavor;
    int marker_offset;
    int padding;
    int start;
    markdown_core_ordered_list_variant variant;
    markdown_core_ordered_list_delimiter delimiter;
    unsigned char bullet_char;
    bool tight;
    /* The authored UTF-8 task marker, owned by the item; absent on ordinary
     * items. Completion is derived by consumers, not stored by the tree. */
    markdown_core_optional_chunk task_marker;
} markdown_core_list;

typedef struct {
    /* OPTIONAL, and the type says so (requirement 14). A fence with nothing
     * after it wrote no info string; `` ``` `` and an indented block are both
     * absent, and `js` is present. */
    markdown_core_optional_chunk info;
    markdown_core_chunk literal;
    uint8_t fence_length;
    uint8_t fence_offset;
    unsigned char fence_char;
    int8_t fenced;
    int8_t fence_closed;
} markdown_core_code;

/* A heading's `label` is the normalized reference label its text declares,
 * the one a reference occurrence names when no Reference declares it, in
 * storage the node owns (markdown_core_node_pool_bytes); empty when its text
 * declares none. */
typedef struct {
    int level;
    bool setext;
    markdown_core_chunk label;
} markdown_core_heading;

/* The title has one private inline parsing root throughout its lifetime.
 * It uses paragraph storage but never enters block content or finalization;
 * the facade exposes only its inline children as Callout.title. */
typedef struct {
    markdown_core_optional_chunk variant;
    markdown_core_optional_bool collapsed;
    struct markdown_core_node *title;
} markdown_core_callout;

/* THE RESOURCE a direct Link or Embedded, or a Reference, states: its
 * destination and title, which the node owns out of its slot. */
typedef struct markdown_core_resource {
    /* REQUIRED (Q26). `[a]()`, `[a](<>)` and `[a]: <>` wrote a destination
     * and it was empty. */
    markdown_core_chunk url;
    /* OPTIONAL (requirement 14): `[a](/u)` wrote no title and `[a](/u "")`
     * wrote an empty one. */
    markdown_core_optional_chunk title;
} markdown_core_resource;
struct markdown_core_node_pool;

typedef struct {
    bool has_value;
    markdown_core_dimensions value;
} markdown_core_optional_dimensions;

/* A LINK OR EMBEDDED names its destination one of two ways (4.5): a direct
 * one states it, in `resource`; a reference names the definition it resolves
 * to by its normalized label, in `label`, as a citation names a footnote, and
 * has no resource. The label is in storage the node owns
 * (markdown_core_node_pool_bytes). */
typedef struct {
    markdown_core_resource *resource;
    markdown_core_chunk label;
    markdown_core_optional_dimensions dimensions;
} markdown_core_link;

/* One authored workspace reference. Each occurrence owns its raw strings. */
typedef struct {
    markdown_core_chunk path;
    markdown_core_optional_chunk anchor;
    markdown_core_optional_chunk label;
} markdown_core_cross_reference;

typedef struct {
    markdown_core_cross_reference reference;
    markdown_core_optional_dimensions dimensions;
} markdown_core_cross_embedded;

/* THE REFERENT of one citation (M4): a tagged value. A `bib` referent, which
 * the citations module first produces with P7, carries a key and a mode; a
 * `footnote` referent carries the label of the `Footnote` it names, or owns
 * the inline note it stands for; a `specimen` referent carries a label. */
typedef enum {
    MARKDOWN_CORE_NODE_REFERENT_BIB = 1,
    MARKDOWN_CORE_NODE_REFERENT_FOOTNOTE = 2,
    MARKDOWN_CORE_NODE_REFERENT_SPECIMEN = 3
} markdown_core_node_referent_kind;

/* ONE ITEM of a cite (M4), and one of its children: the referent, and the
 * item's owned fields: the inline note a `footnote(note)` referent owns, then
 * the two affixes. Each populated affix is a private group node whose
 * children are the affix's nodes; the facade exposes its children. `value` is the referent's key or
 * label: for a footnote referent that names a definition it is the label
 * under the map's own normalization WITHOUT the caret, which is the
 * `Footnote.label` it names, computed once per occurrence. NORMATIVE: a label
 * is compared with memcmp over its bytes and is never case mapped,
 * renormalized, or re-encoded. `note` is NULL unless the item is an inline
 * note, whose `Footnote` it owns. An affix is NULL when it is empty. */
typedef struct {
    markdown_core_node_referent_kind referent;
    markdown_core_chunk value;
    /* The bib mode as the public `markdown_core_bib_mode` numbers it; 0 for a
     * footnote referent. */
    int mode;
    struct markdown_core_node *note;
    struct markdown_core_node *prefix;
    struct markdown_core_node *suffix;
} markdown_core_citation_item;

/* A FOOTNOTE: content is the node's children, block or inline. A definition
 * `[^x]: body` is a block where it was written and has its normalized
 * authored label; an inline note `^[body]` is owned by its Citation and has
 * no label. */
typedef struct {
    markdown_core_optional_chunk label;
} markdown_core_footnote_value;

/* A specimen owns its optional authored label and effective explicit counter
 * reset. Anonymous definitions and absent resets remain absent; numbering is
 * derived from the document's definition order by consumers. */
typedef struct {
    markdown_core_optional_chunk label;
    int64_t start;
    bool has_start;
} markdown_core_specimen_value;

/* A REFERENCE: a link reference definition where it was written. `label` is
 * its normalized label, the one a reference occurrence names, in storage the
 * node owns (markdown_core_node_pool_bytes); `resource` is the destination
 * and title it states. The attributes it supplies are the
 * node's own. */
typedef struct {
    markdown_core_chunk label;
    markdown_core_resource *resource;
} markdown_core_reference_value;

typedef struct {
    struct markdown_core_node *term;
    bool compact;
} markdown_core_definition;

typedef struct {
    int continuation;
    /* The next carried nonblank line, shared by its preceding blank run. */
    int continuation_line;
} markdown_core_definition_body_value;

/* A DEFINITION TABLE of a published document: every Footnote, every
 * Specimen, or every Reference, in source order, borrowed from the tree. */
typedef struct markdown_core_definitions {
    /* Every definition of the kind, in source order. */
    const struct markdown_core_node **nodes;
    size_t count;
    /* The labeled ones by label, in source order among equal labels: the
     * Footnotes and Specimens a lookup by label answers from. A label a
     * reference occurrence names resolves through the reference targets. */
    const struct markdown_core_node **labeled;
    size_t labeled_count;
} markdown_core_definitions;

/* THE DOCUMENT's own field: the metadata the properties envelope produced.
 * Footnote, specimen and Reference definitions stay in the tree where they
 * were written; publishing the document records its definition tables here,
 * and the nodes reference occurrences resolve to, by label: for each label,
 * the first Reference declaring it, or, when none does, the first Heading
 * whose text declares it. */
typedef struct {
    struct markdown_core_node *metadata;
    markdown_core_definitions footnotes;
    markdown_core_definitions specimens;
    markdown_core_definitions references;
    const struct markdown_core_node **reference_targets;
    size_t reference_target_count;
} markdown_core_document_value;

/* A node's source extent in UTF-8 bytes (the published form of its place). */
#ifndef MARKDOWN_CORE_EXTENT_TYPEDEF
#define MARKDOWN_CORE_EXTENT_TYPEDEF
typedef struct markdown_core_extent {
    int32_t lead;
    uint32_t span;
} markdown_core_extent;
#endif

/* An absolute byte range [start, end) of the document source. */
typedef struct {
    uint32_t start, end;
} markdown_core_place;

/* A run of a node's source (markdown_core.h): `length` content bytes read
 * from the source bytes `span` long, `lead` from the end of the previous run,
 * or from the start of the node that holds the runs. */
#ifndef MARKDOWN_CORE_RUN_TYPEDEF
#define MARKDOWN_CORE_RUN_TYPEDEF
typedef struct markdown_core_run {
    int32_t lead;
    uint32_t span;
    uint32_t length;
} markdown_core_run;
#endif

/* WHERE A NODE'S BYTES LIE when its extent alone does not say: the runs of
 * source it read, those its inline content was read from and, with length
 * 0, those that gave no content, with the source that is not its own between
 * them. While a parse builds the tree each holds an absolute source range,
 * as a node's place does; publishing rewrites them relative, as it rewrites
 * the place as the extent. The list is one owned allocation, NULL when the
 * node has none. */
typedef struct {
    uint32_t start, end, length;
} markdown_core_run_place;

typedef union {
    markdown_core_run_place place;
    markdown_core_run run;
} markdown_core_run_where;

typedef struct markdown_core_runs {
    /* How many runs there are, and the content bytes they read: 0 for a
     * node without inline content, whose runs all have length 0. */
    uint32_t count, content;
    markdown_core_run_where items[];
} markdown_core_runs;

/* WHERE A NODE IS, in bytes of the UTF-8 source, and never in lines or
 * columns. While a parse builds the tree every node holds its absolute
 * `place`. Publishing the document (markdown_core_publish_tree) rewrites
 * each node's place as its `extent`: `lead`, the signed distance from the end
 * of the previous node in the same relation (or from its owner's start, for
 * the first node), and `span`, the length of its range. Relative extents are
 * what lets a node keep its value when text before it moves. A published node
 * holds only its extent; nothing reads a place after publishing. */
typedef union {
    markdown_core_place place;
    markdown_core_extent extent;
} markdown_core_node_where;

enum markdown_core_node__internal_flags {
    MARKDOWN_CORE_NODE__OPEN = (1 << 0),
    MARKDOWN_CORE_NODE__LAST_LINE_BLANK = (1 << 1),
    MARKDOWN_CORE_NODE__LAST_LINE_CHECKED = (1 << 2),
    MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK = (1 << 3),
    // An HTML block whose own end condition matched on the line being
    // processed. `finalize` reads it to end the block on that line rather
    // than on the line before, and to know that a `-->` line really closed a
    // type-2 block rather than the input or a container running out.
    MARKDOWN_CORE_NODE__CLOSED_BY_END_CONDITION = (1 << 4),
    // A finalized paragraph consumed entirely by reference definitions,
    // which its finalization made References before it. It retains block
    // adjacency until block parsing ends, then is discarded before list
    // layout and inline parsing observe the semantic children.
    MARKDOWN_CORE_NODE__REFERENCE_DEFINITION_ONLY = (1 << 5),

    // Deferred contextual escape token, decoded when inline ownership is final.
    MARKDOWN_CORE_NODE__ESCAPED_SPACE = (1 << 6),

    /* Reference definitions have advanced this block's semantic beginning.
     * A later arrival may supply its first surviving content line. */
    MARKDOWN_CORE_NODE__REFERENCE_PREFIX = (1 << 7),

    /* A block the blank-line facts read through, as CommonMark reads through
     * a link reference definition: whether a block ends with a blank line,
     * and whether another block follows it, are answered by the blocks
     * around it. */
    MARKDOWN_CORE_NODE__BLANK_TRANSPARENT = (1 << 8),

    /* A private node a group of its owner's relations hangs from -- a
     * callout's title, a definition's term or body, a citation's affix. It is
     * no node of the document: its owner numbers the group's nodes. */
    MARKDOWN_CORE_NODE__GROUP = (1 << 9),

    /* A block that closed while the last block it holds was still open: it
     * settles as that block does (markdown_core_block_finalize). */
    MARKDOWN_CORE_NODE__AWAITS_CHILD = (1 << 10),

    // The first bit an element may claim. Element flags are compile-time
    // constants owned by the element that uses them; there is no runtime
    // registration and no allocator to run out of bits.
    MARKDOWN_CORE_NODE__ELEMENT_FIRST = (1 << 11),
};

typedef uint16_t markdown_core_node_internal_flags;

/* HTML recognition state and the eventual literal have one owner throughout
 * the block lifecycle. They never overlay or replace each other's storage. */
typedef struct {
    markdown_core_chunk literal;
    int block_type;
} markdown_core_html_block;

typedef struct {
    int64_t rowspan, colspan;
    /* Whether the cell's content is read as blocks (a block input) rather
     * than as inline content. */
    bool blocks;
} markdown_core_table_cell;

/* Every arm points to the kind's ordinary typed record. Construction places
 * the record after an aligned node allocation header; a kind with no fields
 * has no record. Retyping keeps node identity stable and reuses slot capacity
 * or owns an external record. The common node layout never depends on record size. */
typedef union {
    void *data;
    markdown_core_chunk *literal;
    markdown_core_list *list;
    markdown_core_code *code;
    markdown_core_heading *heading;
    markdown_core_callout *callout;
    markdown_core_link *link;
    markdown_core_cross_reference *cross_link;
    markdown_core_cross_embedded *cross_embedded;
    markdown_core_citation_item *citation;
    markdown_core_footnote_value *footnote;
    markdown_core_specimen_value *specimen;
    markdown_core_reference_value *reference;
    markdown_core_document_value *document;
    markdown_core_metadata_fields *metadata;
    markdown_core_definition *definition;
    markdown_core_definition_body_value *definition_body;
    markdown_core_html_block *html_block;
    markdown_core_table_cell *table_cell;
} markdown_core_node_data;

/* A NODE'S CHILDREN TREE (docs/plans/2026-09-29-incremental-parsing.md,
 * 5.11): an immutable, balanced tree of the node's children in order, like
 * the children array of a tree-sitter subtree. A stem of height 0 holds
 * nodes; a higher one holds the stems below it, all of one height. Each holds
 * between 1 and MARKDOWN_CORE_STEM_WIDTH entries and counts the nodes under
 * it. A stem is a value shared by reference: every node or stem that holds
 * it counts one reference, and an entry it holds counts one reference of
 * that entry's. */
#define MARKDOWN_CORE_STEM_WIDTH 32
#define MARKDOWN_CORE_STEM_HEIGHT 8

typedef struct markdown_core_stem markdown_core_stem;

typedef union {
    struct markdown_core_node *node;
    markdown_core_stem *stem;
} markdown_core_stem_entry;

struct markdown_core_stem {
    uint32_t refs;
    uint32_t count;
    uint8_t height;
    uint8_t width;
    markdown_core_stem_entry entries[];
};

/* A NODE is a shared immutable value (5.11): one subtree may sit in the old
 * tree and the new one at once, so a node has no parent and no siblings, and
 * its children are its stem. `refs` counts the stems, fields and builders
 * (markdown_core_member) that hold it; a node held once may change in place,
 * and one held more often is copied before it changes. */
struct markdown_core_node {
    uint32_t refs;
    uint16_t kind;
    markdown_core_node_internal_flags flags;
    /* The node's children, or NULL when it has none. */
    markdown_core_stem *children;

    /* The node's identifier, unique within its document; 0 until the node
     * that holds it completes (docs/plans/2026-09-29-incremental-parsing.md,
     * 5.8). */
    uint64_t id;

    markdown_core_attributes attributes;
    markdown_core_strbuf content;
    /* Where the node is in the source; see markdown_core_node_where. */
    markdown_core_node_where where;
    int internal_offset;
    /* This node's slice of parser-owned content-to-source runs. Zero count
     * means there is no mapped content (for example, an empty cell). */
    markdown_core_content_map content_map;
    /* The runs of its source (markdown_core_runs). */
    markdown_core_runs *runs;

    const markdown_core_element *element;
    /* Element-owned data, allocated by opaque_alloc_func and released by
     * opaque_free_func. It survives kind changes independently of `as`. */
    void *opaque;

    /* Owns a record too large for the slot, whether installed at construction
     * or conversion. `as` is the typed view for either backing. */
    void *node_data_allocation;
    markdown_core_node_data as;
};

/* Both cross kinds own the same raw reference fields in one payload allocation.
 * Only CrossEmbedded allocates the dimension value beside those fields. */
static inline markdown_core_cross_reference *markdown_core_node_cross_reference(const markdown_core_node *node) {
    return node->kind == MARKDOWN_CORE_NODE_CROSS_LINK ? node->as.cross_link : &node->as.cross_embedded->reference;
}

/* Takes ownership of `url` and `title` and answers a resource, or NULL
 * having taken nothing -- the caller still owns both chunks and frees them.
 * A string that views bytes it does not own is copied: the resource holds
 * what it reads.
 * The resource's slot comes from `pool`'s resource slabs, or from the
 * allocator when it is NULL (slab.h). */
markdown_core_resource *markdown_core_resource_new(struct markdown_core_node_pool *pool, markdown_core_chunk url,
                                                   markdown_core_optional_chunk title);
/* Frees the resource, its slot going back to `resources` (a pool's resource
 * slabs) or, when that is NULL, dropping its slab hold. NULL is a no-op. */
void markdown_core_resource_free(markdown_core_slab_pool *resources, markdown_core_resource *resource);
int markdown_core_node_check(markdown_core_node *node, FILE *out);

static MARKDOWN_CORE_INLINE bool MARKDOWN_CORE_NODE_TYPE_BLOCK_P(markdown_core_node_type node_type) {
    return (node_type & MARKDOWN_CORE_NODE_TYPE_MASK) == MARKDOWN_CORE_NODE_TYPE_BLOCK;
}

static MARKDOWN_CORE_INLINE bool MARKDOWN_CORE_NODE_BLOCK_P(markdown_core_node *node) {
    return node != NULL && MARKDOWN_CORE_NODE_TYPE_BLOCK_P((markdown_core_node_type)node->kind);
}

static MARKDOWN_CORE_INLINE bool MARKDOWN_CORE_NODE_TYPE_INLINE_P(markdown_core_node_type node_type) {
    return (node_type & MARKDOWN_CORE_NODE_TYPE_MASK) == MARKDOWN_CORE_NODE_TYPE_INLINE;
}

static MARKDOWN_CORE_INLINE bool MARKDOWN_CORE_NODE_INLINE_P(markdown_core_node *node) {
    return node != NULL && MARKDOWN_CORE_NODE_TYPE_INLINE_P((markdown_core_node_type)node->kind);
}

/* The fixed grammar and an element's retained kind domain, without callbacks.
 * Checked mutation and construction assertions use this same rule. */
bool markdown_core_node_can_contain_builtin(const markdown_core_node *node, markdown_core_node_type child_type);

bool markdown_core_node_can_contain_type(markdown_core_node *node, markdown_core_node_type child_type);

typedef int (*markdown_core_owned_subtree_visitor)(markdown_core_node **root_slot, void *context);

/* Visits each node-valued field slot of `node`, those of its kind's record
 * and those its element owns (element.h, `visit_owned_subtrees_func`), in
 * canonical field order, whether or not it holds a node; stops, answering 0,
 * when the visitor does. */
int markdown_core_node_visit_fields(markdown_core_node *node, markdown_core_owned_subtree_visitor visitor,
                                    void *context);

/* The bit a BLOCK kind occupies in a container-kind set, or zero for an inline
 * kind or none at all. Block kind values are small and dense, so a set of the
 * kinds open at a point in the parse fits one word and is tested with an AND.
 *
 * It lives with the node type rather than with the code that builds such a set
 * so that the block driver can intersect two sets without naming a kind or
 * knowing how a kind is encoded. */
static MARKDOWN_CORE_INLINE uint32_t markdown_core_node_block_kind_bit(markdown_core_node_type kind) {
    if ((kind & MARKDOWN_CORE_NODE_TYPE_MASK) != MARKDOWN_CORE_NODE_TYPE_BLOCK) {
        return 0;
    }
    unsigned value = (unsigned)kind & MARKDOWN_CORE_NODE_VALUE_MASK;
    /* Every block kind too large for a bit of its own shares the last one.
     * Sharing can only make two different kinds look alike, never make one
     * disappear, so a set built this way OVER-approximates: the cost of an
     * extension kind beyond the word is a hook entered once too often, not a
     * construct silently never recognised. A private bit per kind would be the
     * other way round, and that is the direction that loses documents. */
    return value >= 31 ? 1u << 31 : 1u << value;
}

/* The same for an INLINE kind. Block and inline values overlap once masked, so
 * a set that must tell a List from a LineBreak keeps the two apart. Both are
 * in the header: every node a parse makes records its kind through them, and
 * a call across a translation unit for four instructions was 18 Ir per node. */
static MARKDOWN_CORE_INLINE uint32_t markdown_core_node_inline_kind_bit(markdown_core_node_type kind) {
    if ((kind & MARKDOWN_CORE_NODE_TYPE_MASK) != MARKDOWN_CORE_NODE_TYPE_INLINE) {
        return 0;
    }
    unsigned value = (unsigned)kind & MARKDOWN_CORE_NODE_VALUE_MASK;
    return value >= 31 ? 1u << 31 : 1u << value;
}

/* A set of node kinds, used both for what an element's phase acts on and for
 * what a parse actually produced. Intersecting the two is how a phase that
 * cannot possibly do anything is skipped instead of walking the tree. */
typedef struct markdown_core_node_kind_set {
    uint32_t blocks;
    uint32_t inlines;
} markdown_core_node_kind_set;

/* Add `kind` to `set`. */
static MARKDOWN_CORE_INLINE void markdown_core_node_kind_set_add(markdown_core_node_kind_set *set,
                                                                 markdown_core_node_type kind) {
    set->blocks |= markdown_core_node_block_kind_bit(kind);
    set->inlines |= markdown_core_node_inline_kind_bit(kind);
}

/* Whether the two sets name any kind in common. */
bool markdown_core_node_kind_set_intersects(const markdown_core_node_kind_set *a, const markdown_core_node_kind_set *b);

/* A node of `type` built outside any parse, from the allocator's own slot.
 * `_with_ext` also names the element whose descriptor owns its behaviour. The
 * node may have required fields the caller must still assign. */
markdown_core_node *markdown_core_node_new(markdown_core_node_type type);
markdown_core_node *markdown_core_node_new_with_ext(markdown_core_node_type type, const markdown_core_element *element);

/* Drops one reference to `node`; at the last, releases it and drops its
 * references to its children and fields in turn (markdown_core_node_release). */
void markdown_core_node_free(markdown_core_node *node);

/* Makes every byte string `node`'s record holds its own, copying a view of
 * the input a parse read. Completion calls it, so a node that a later
 * revision shares reads nothing another node owns. False when a copy could
 * not be allocated. */
bool markdown_core_node_hold_strings(markdown_core_node *node);

/* Takes one more reference to `node`, and returns it. */
static inline markdown_core_node *markdown_core_node_retain(markdown_core_node *node) {
    node->refs++;
    return node;
}

/* The internal type's name, for diagnostics: "<unknown>" for a value no
 * class defines. Internal types the facade folds together (COMMENT_BLOCK and
 * COMMENT) keep their own names here. */
const char *markdown_core_node_get_type_string(markdown_core_node *node);

/* `markdown_core_node_free`, reporting how many nodes it released: those
 * whose last reference it dropped. RELEASE IS ITERATIVE (5.11): a released
 * node or stem is linked into the pending list through storage it no longer
 * needs, so nothing recurses and nothing allocates. */
size_t markdown_core_node_release(markdown_core_node *node);

/* WHERE A NODE'S STORAGE COMES FROM, and where it goes back to.
 *
 * A node lives in a SLOT (slab.h) holding the node and room for its kind's
 * record, the resources links read through live in slots of their own, and
 * what else a node owns of its own size -- its runs, a label it
 * declares -- in storage of that size. A pool holds the slabs of all
 * three. A parse takes every slot from the pool its
 * caller lends it -- a session's, which outlives each of its edits, or one
 * the caller makes for a single parse -- and a caller with no pool takes one
 * slot from the allocator. A slot released into a pool goes back to it for
 * reuse, so a session's edits reuse the slots of the nodes they retire
 * instead of pinning a slab per edit; a slot released with no pool drops its
 * slab hold. So a subtree unlinked from a parsed document is as good as one
 * built by hand: it outlives the pool it came from and is released by
 * `markdown_core_node_free` like any other. The size of what it keeps alive
 * is the slab, not the node.
 *
 * Why slabs: a node's chunk was larger than the C library's fast-path size
 * classes, so every release of one walked the allocator's merge path, and the
 * document's teardown cost more than a third of its parse. */
typedef struct markdown_core_node_pool {
    markdown_core_slab_pool nodes;
    markdown_core_slab_pool resources;
    markdown_core_slab_pool members;
    markdown_core_bytes_pool bytes;
} markdown_core_node_pool;

/* Uninitialized storage of `bytes` a node owns -- its runs, a
 * label it declares -- from the pool's slabs, or from the allocator with no
 * pool; NULL when none can be had. The node releases it with itself. */
#define MARKDOWN_CORE_NODE_BYTES_SLAB_BYTES ((size_t)16 * 1024)
static inline void *markdown_core_node_pool_bytes(markdown_core_node_pool *pool, size_t bytes) {
    return markdown_core_bytes_take(pool ? &pool->bytes : NULL, bytes, MARKDOWN_CORE_NODE_BYTES_SLAB_BYTES);
}
/* Gives back storage `markdown_core_node_pool_bytes` took, into `pool` for
 * reuse; a NULL pool is the plain release. */
void markdown_core_node_pool_bytes_free(markdown_core_node_pool *pool, void *storage);

/* `markdown_core_node_new_with_ext` from a pool's slots. A NULL pool is the
 * allocator's own slot, which is what the parser-less constructor takes. */
markdown_core_node *markdown_core_node_pool_new(markdown_core_node_pool *pool, markdown_core_node_type type,
                                                const markdown_core_element *element);
/* `markdown_core_node_release` into a pool: the node slots and the slots of
 * the resources the nodes held last go back to it for reuse rather than
 * dropping their slabs. A NULL pool is the plain release. */
size_t markdown_core_node_pool_release(markdown_core_node_pool *pool, markdown_core_node *node);
/* Drops what the pool holds: its released slots and its current slabs. Slots
 * still in use keep their slabs alive after this. */
void markdown_core_node_pool_dispose(markdown_core_node_pool *pool);

/* THE CHILDREN TREE'S OPERATIONS. */

/* A stem of the `count` nodes at `nodes`, in order, balanced: it takes the
 * reference to each node the caller held. NULL, having taken nothing, when
 * `count` is 0 or an allocation failed (`*failed`). */
markdown_core_stem *markdown_core_stem_make(markdown_core_node_pool *pool, markdown_core_node *const *nodes,
                                            size_t count, bool *failed);

static inline size_t markdown_core_stem_count(const markdown_core_stem *stem) { return stem ? stem->count : 0; }

/* The node at `index` of the stem, which holds more than `index`. */
markdown_core_node *markdown_core_stem_at(const markdown_core_stem *stem, size_t index);

/* Puts `node` at `index` of `stem`, which no one else holds, and returns the
 * node it held there; the stem takes the caller's reference to `node` and
 * gives the caller its reference to the one it returns. */
markdown_core_node *markdown_core_stem_put(markdown_core_stem *stem, size_t index, markdown_core_node *node);

/* A WALK OVER A RUN OF A STEM'S NODES, in order: the path from the stem to
 * the node it is at. */
typedef struct {
    const markdown_core_stem *path[MARKDOWN_CORE_STEM_HEIGHT];
    uint8_t at[MARKDOWN_CORE_STEM_HEIGHT];
    /* How many nodes are left, the one it is at included. */
    size_t left;
} markdown_core_stem_walk;

/* Begins at the node at `index` of `stem` (which may be NULL), to read
 * `count` nodes. */
void markdown_core_stem_walk_begin(markdown_core_stem_walk *walk, const markdown_core_stem *stem, size_t index,
                                   size_t count);
/* The node the walk is at, and moves past it; NULL once it has read them all. */
markdown_core_node *markdown_core_stem_walk_next(markdown_core_stem_walk *walk);

/* A NODE BEING BUILT (5.11, open blocks are builders): while the parser
 * builds a node, the node's place among the nodes being built is this
 * record's, not the node's. Its owner, its siblings and its children are the
 * members of the nodes beside it. `fields` are the members of the node's
 * field roots that are being built with it, in canonical field order and
 * linked through `next`: a citation's note and affixes, which the inline parse
 * fills as it makes the citation. `held` says whether the member holds the
 * node's reference: a child member does until its owner's freeze hands the
 * reference to the owner's stem; a root, whose node a stem or the parser
 * holds, and a field root, whose node its owner's field holds, do not. A
 * builder lives for one parse and its storage is the pool's. A member is
 * `inner` when the members it holds lie in the content of an inline root:
 * the builder of the root's content is, and so is every member under it.
 *
 * A member also carries what the node continues
 * (docs/plans/2026-09-29-incremental-parsing.md, 5.9): `old`, the node of
 * the previous tree it continues once `decided`, whether its node has taken
 * its id (`identified`), where that node starts in
 * its old coordinates, `reach`, the image up to which its owner's cursor
 * has passed for it, and its `candidates` to `last_candidate`, the old nodes
 * whose images lie in its range (ast_internal.h), among which
 * it decides. As an owner it pairs the members it holds with the old node's
 * relation `pair_name`: the old nodes `pair_next` to `pair_end`
 * of `pair_stem` not yet passed, and the end of the one passed last. `asks`
 * counts the members it holds that have `asked` for their old node, each of
 * which took its `index` among them as it asked.
 *
 * A node is final when everything it waits on is: `waits` counts the inline
 * root, block input or late value it waits on and the members it holds that
 * were numbered while they still waited (`counted`). Once numbered and with
 * nothing to wait on, it settles (ast.c): its `slot` is where its owner's
 * stem holds it, `place` where the node lay, its own `where` now holding its
 * extent, and `source` where a definition was written. */
struct markdown_core_member {
    markdown_core_node *node;
    struct markdown_core_member *owner;
    struct markdown_core_member *prev, *next;
    struct markdown_core_member *first, *last;
    struct markdown_core_member *fields;
    const markdown_core_node *old;
    const markdown_core_stem *pair_stem;
    size_t pair_next, pair_end;
    markdown_core_place place;
    uint32_t old_start, reach, pair_anchor, pair_name;
    uint32_t asks, index, slot, source, waits, candidates, last_candidate;
    bool held, field, inner, decided, identified, paired, asked, numbered, counted;
};

/* A member for `node`, linked to nothing; it holds the node's reference when
 * `held`. NULL when it could not be allocated. */
markdown_core_member *markdown_core_member_new(markdown_core_node_pool *pool, markdown_core_node *node, bool held);

/* Links the detached `child` under `owner`, before `before` (a child of
 * `owner`) or last; it is inner when `owner` is. The caller has proved
 * containment. */
void markdown_core_member_attach(markdown_core_member *owner, markdown_core_member *child,
                                 markdown_core_member *before);

/* Links the detached `field` as the last field root `owner` builds; it is
 * inner when `owner` is. */
void markdown_core_member_attach_field(markdown_core_member *owner, markdown_core_member *field);

/* Detaches `member` from its owner and siblings, keeping its subtree. */
void markdown_core_member_unlink(markdown_core_member *member);

/* COMPLETES A BUILDER'S STRUCTURE: the nodes of `member`'s children become
 * its node's stem, which takes the references their members held; the
 * members stay, for the node's numbering. False, changing nothing, when the
 * stem could not be allocated. */
bool markdown_core_member_freeze(markdown_core_node_pool *pool, markdown_core_member *member);

/* Releases `member`, every member below it, and the references they hold;
 * `member` was detached first, or is a root. */
void markdown_core_member_release(markdown_core_node_pool *pool, markdown_core_member *member);

#ifdef __cplusplus
}
#endif

#endif
