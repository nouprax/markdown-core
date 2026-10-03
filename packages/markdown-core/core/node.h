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
#include "children.h"
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

typedef struct {
    int level;
    bool setext;
} markdown_core_heading;

/* The title has one private inline parsing root throughout its lifetime.
 * It uses paragraph storage but never enters block content or finalization;
 * the facade exposes only its inline children as Callout.title. */
typedef struct {
    markdown_core_optional_chunk variant;
    markdown_core_optional_bool collapsed;
    struct markdown_core_node *title;
} markdown_core_callout;

/* THE RESOURCE a Link or Embedded reads its destination and title from (M2).
 *
 * It is COUNTED and SHARED. A link reference definition's resource is built
 * once, when the block phase reads the definition into the parser's map, and
 * every occurrence that resolves to that definition -- full, collapsed or
 * shortcut -- holds the same one; a direct link, a direct image and an
 * autolink each own one of their own. Sharing is what deletes D9 for good: a
 * definition with a long destination referenced many times costs one copy of
 * the destination however many times it is named, so nothing has to be
 * charged and no budget can make WHETHER A REFERENCE RESOLVES depend on how
 * many resolved before it. `holders` counts the fact that declares it and
 * every node reading through the resource; the last one out frees it, which
 * is how the tree outlives the registries that declared it.
 *
 * A resource is a VALUE: two are equal exactly when their fields are, and the
 * pointer is only its storage. Nodes holding one storage hold equal values;
 * equal values can live in different storage, because a session's document
 * takes Links and Embeddeds whole from the previous one and they keep the
 * storage they were parsed with. `markdown_core_node_resource` hands the
 * storage out, so a consumer can decode each storage once, and one that names
 * each value once keys on the value. */
struct markdown_core_resource {
    /* REQUIRED (Q26). `[a]()`, `[a](<>)` and `[a]: <>` wrote a destination
     * and it was empty; there is no link whose author wrote no destination at
     * all, because a reference resolves to its definition's. */
    markdown_core_chunk url;
    /* OPTIONAL (requirement 14): `[a](/u)` wrote no title and `[a](/u "")`
     * wrote an empty one. */
    markdown_core_optional_chunk title;
    /* Definition metadata is immutable and owned by the same resource. Each
     * occurrence holds only its own normalized attributes on the node. */
    markdown_core_attributes attributes;
    size_t holders;
};
#ifndef MARKDOWN_CORE_RESOURCE_TYPEDEF
#define MARKDOWN_CORE_RESOURCE_TYPEDEF
typedef struct markdown_core_resource markdown_core_resource;
#endif
struct markdown_core_node_pool;

typedef struct {
    bool has_value;
    markdown_core_dimensions value;
} markdown_core_optional_dimensions;

typedef struct {
    /* NEVER NULL on a node the parser finished: the resource is attached in
     * the same step that makes the node a link, and an allocation that could
     * not attach one frees the node. */
    markdown_core_resource *resource;
    markdown_core_optional_dimensions dimensions;
} markdown_core_link;

/* One authored workspace reference. Each occurrence owns its raw strings;
 * unlike resolved links, it has no resource shared with a definition. */
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

/* ONE ITEM of a cite (M4), one of the Cite's children: the referent, and the item's owned fields: the
 * inline note a `footnote(note)` referent owns, then the two affix chains.
 * Each populated affix uses the same private inline root as other owned
 * fields; the facade exposes its children. `value` is the referent's key or
 * label: for a footnote referent that names a definition it is the label
 * under the map's own normalization WITHOUT the caret, which is the
 * `Footnote.label` it names, computed once per occurrence. NORMATIVE: a label
 * is compared with memcmp over its bytes and is never case mapped,
 * renormalized, or re-encoded. `note` is NULL unless the item is an inline
 * note, whose `Footnote` it owns. A chain is NULL when the affix is empty. */
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

typedef struct {
    struct markdown_core_node *term;
    bool compact;
} markdown_core_definition;

typedef struct {
    int continuation;
    /* The next carried nonblank line, shared by its preceding blank run. */
    int continuation_line;
} markdown_core_definition_body_value;

/* THE DOCUMENT's own field: the metadata the properties envelope produced.
 * Footnote and specimen definitions stay in the tree where they were
 * written; the document's tables of them are the session's registries
 * (elements/registry.h). */
typedef struct {
    struct markdown_core_node *metadata;
} markdown_core_document_value;

/* A link reference definition is not a node (M2). The block phase reads it off
 * the front of the paragraph that held it into the parser's map, which owns
 * its resource once, and every reference that resolves to it is the `Link` or
 * `Embedded` it names, sharing that resource. This is the inherited grammar's
 * model: a definition exists to be referred to, an unreferenced one produces
 * nothing, and the first definition of a label in source order wins. A footnote
 * definition is a node, because its body is flow content, and stays where it
 * was written. */

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

/* WHERE A NODE IS, in bytes of the UTF-8 source, and never in lines or
 * columns. A node holds its absolute `place` until its owner completes:
 * the owner, when the parse publishes it (markdown_core_publish_node),
 * rewrites the place of every node of its relations as its `extent`: `lead`,
 * the signed distance from the end of the previous node in the same relation
 * (or from the owner's start, for the first node), and `span`, the length of
 * its range; the root's extent is measured from 0. Relative extents are what
 * lets a node keep its value when text before it moves. A published tree
 * holds only extents. */
typedef union {
    markdown_core_place place;
    markdown_core_extent extent;
} markdown_core_node_where;

enum markdown_core_node__internal_flags {
    MARKDOWN_CORE_NODE__OPEN = (1 << 0),
    MARKDOWN_CORE_NODE__LAST_LINE_BLANK = (1 << 1),
    // THE BLANK-LINE FACTS (docs/plans/2026-09-29-incremental-parsing.md,
    // E3 and E4), each written once, by the parse that makes the block, and
    // only read after; blocks.c settles them. A completed block ENDS BLANK
    // when its own last line is blank or, for a kind a blank line propagates
    // out of that holds children, when its last child ends blank as the
    // block saw it, counting the blank lines after that child.
    MARKDOWN_CORE_NODE__ENDS_BLANK = (1 << 2),
    MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK = (1 << 3),
    // An HTML block whose own end condition matched on the line being
    // processed. `finalize` reads it to end the block on that line rather
    // than on the line before, and to know that a `-->` line really closed a
    // type-2 block rather than the input or a container running out.
    MARKDOWN_CORE_NODE__CLOSED_BY_END_CONDITION = (1 << 4),
    // A finalized paragraph consumed entirely by reference definitions. It
    // keeps its place as its open parent's last child, and the parent drops
    // it when it takes another child or completes, before list layout and
    // inline parsing observe the semantic children.
    MARKDOWN_CORE_NODE__REFERENCE_DEFINITION_ONLY = (1 << 5),

    // Deferred contextual escape token, decoded when inline ownership is final.
    MARKDOWN_CORE_NODE__ESCAPED_SPACE = (1 << 6),

    /* Reference definitions have advanced this block's semantic beginning.
     * A later arrival may supply its first surviving content line. */
    MARKDOWN_CORE_NODE__REFERENCE_PREFIX = (1 << 7),

    /* A closed block whose completion waits for its content -- an inline
     * root's pass, or a cell's blocks (parser.h, completion) -- so its owner
     * completes, and publishes it, first. */
    MARKDOWN_CORE_NODE__PENDING = (1 << 8),

    /* An edit met this node, its lead or its reach
     * (docs/plans/2026-09-29-incremental-parsing.md, 5.2): a parse against
     * the tree it is in reads it again rather than taking it. */
    MARKDOWN_CORE_NODE__CHANGED = (1 << 9),

    /* A run of taken blocks cannot end at this one (5.3): the parse after a
     * run reads the lines after it with the run closed, and the old parse
     * read one of them from a state in which the block was still open -- a
     * block start was refused on the line that closed it because a paragraph
     * or a lazy line was open, or the block was open over blank lines after
     * its end, which its last descendants saw; or a later line may still
     * write into it (E2, markdown_core_parser_write_closed), which only a
     * block of the parse that makes it takes. */
    MARKDOWN_CORE_NODE__EXIT_FRAGILE = (1 << 10),

    /* The node's `where` holds its extent: its owner published it, in this
     * parse or, for a node the parse took, in the one that made it. */
    MARKDOWN_CORE_NODE__PUBLISHED = (1 << 11),

    /* The rest of the blank-line facts. A completed block CONTAINS A BLANK
     * when a child but its last ends blank as the block saw it. A block's
     * entry records what it starts after: the sibling before it ENDS BLANK
     * as their parent saw it, or that sibling ENDS LOOSE, which a list item
     * separates from the next on: its own last line is blank, a blank line
     * follows it, or it ends blank. */
    MARKDOWN_CORE_NODE__CONTAINS_BLANK = (1 << 12),
    MARKDOWN_CORE_NODE__AFTER_BLANK_END = (1 << 13),
    MARKDOWN_CORE_NODE__AFTER_LOOSE_END = (1 << 14),

    // The first bit an element may claim. Element flags are compile-time
    // constants owned by the element that uses them; there is no runtime
    // registration and no allocator to run out of bits.
    MARKDOWN_CORE_NODE__ELEMENT_FIRST = (1 << 15),
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
} markdown_core_table_cell;

/* Every arm points to the kind's ordinary typed record. Construction places
 * the record after an aligned node allocation header; a kind with no fields
 * has no record. Retyping keeps node identity stable and reuses slot capacity
 * or owns an external record. The common node layout never depends on record size. */
/* A REGISTRY ENTRY's record (facts.h): the fact the entry places. */
struct markdown_core_fact;
typedef struct {
    struct markdown_core_fact *fact;
} markdown_core_fact_place;

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
    markdown_core_document_value *document;
    markdown_core_metadata_fields *metadata;
    markdown_core_definition *definition;
    markdown_core_definition_body_value *definition_body;
    markdown_core_html_block *html_block;
    markdown_core_table_cell *table_cell;
    markdown_core_fact_place *fact_place;
} markdown_core_node_data;

/* THE BYTES A LITERAL READS (docs/plans/2026-09-29-incremental-parsing.md,
 * 5.11): an inline root's content once its inline parse begins. The literals
 * that parse makes point into it rather than copy it, so every node the parse
 * makes holds it, as the parse itself does while it runs: a node shared into
 * another tree keeps the bytes its values read, whichever root they came
 * from. */
typedef struct markdown_core_bytes {
    size_t refs;
    unsigned char *data;
} markdown_core_bytes;

/* A NODE IS A SHARED VALUE (docs/plans/2026-09-29-incremental-parsing.md,
 * 5.11). It has no parent and no siblings: its holders -- the runs of a
 * parent's children, an owner's field, a document, a parse's open spine --
 * count it, and a walk carries its own path. A node held once may change in
 * place; a shared one is copied before it changes. */
struct markdown_core_node {
    /* The node's holders, its children and its id; every field after `id` is
     * its value. */
    union {
        size_t refs;
        /* Once it has no holder: the next node of the release list it is
         * on (node.c). */
        struct markdown_core_node *released;
    } hold;
    /* The content children (children.h), or NULL when there are none. */
    markdown_core_run *children;
    /* The inline input the parser was reading when it made the node, which
     * its literals may point into, or NULL. */
    markdown_core_bytes *bytes;

    /* The node's identifier, unique within its document; 0 until the
     * document is published (markdown_core_publish_tree). */
    uint64_t id;

    markdown_core_attributes attributes;
    markdown_core_strbuf content;
    /* Where the node is in the source; see markdown_core_node_where. */
    markdown_core_node_where where;
    int internal_offset;
    /* This node's slice of parser-owned content-to-source runs. Zero count
     * means there is no mapped content (for example, an empty cell). */
    markdown_core_content_map content_map;
    uint16_t kind;
    markdown_core_node_internal_flags flags;
    /* How far past its end the decisions about this node read, in bytes
     * (5.1): the input's high-water mark when it closed, raised by a later
     * write to it (markdown_core_parser_write_closed). */
    uint32_t reach;
    /* What the node adds to a count its open parent carries over its
     * children (E3), which the children tree sums (children.h): a table
     * row's completed cells. */
    uint32_t tally;

    const markdown_core_element *element;
    /* Element-owned data, allocated by opaque_alloc_func and released by
     * opaque_free_func. It survives kind changes independently of `as`. */
    void *opaque;

    /* Owns a record too large for the slot, whether installed at construction
     * or conversion. `as` is the typed view for either backing. */
    void *node_data_allocation;
    markdown_core_node_data as;
};

/* The effective declaration is occurrence-local, then inherited from its
 * shared definition. All consumers, including synthesis reservation, use it. */
static inline const markdown_core_chunk *markdown_core_node_anchor_chunk(const markdown_core_node *node) {
    if (!node->attributes.anchor.len &&
        (node->kind == MARKDOWN_CORE_NODE_LINK || node->kind == MARKDOWN_CORE_NODE_EMBEDDED) &&
        node->as.link->resource) {
        return &node->as.link->resource->attributes.anchor;
    }
    return &node->attributes.anchor;
}

/* Both cross kinds own the same raw reference fields in one payload allocation.
 * Only CrossEmbedded allocates the dimension value beside those fields. */
static inline markdown_core_cross_reference *markdown_core_node_cross_reference(const markdown_core_node *node) {
    return node->kind == MARKDOWN_CORE_NODE_CROSS_LINK ? node->as.cross_link : &node->as.cross_embedded->reference;
}

/* Takes ownership of `url` and `title` and answers a resource with one holder,
 * or NULL having taken nothing -- the caller still owns both chunks and frees
 * them. The resource's slot comes from `pool`'s resource slabs, or from the
 * allocator when it is NULL (slab.h). */
markdown_core_resource *markdown_core_resource_new(struct markdown_core_node_pool *pool, markdown_core_chunk url,
                                                   markdown_core_optional_chunk title);
void markdown_core_resource_retain(markdown_core_resource *resource);
/* Drops one holder and frees the resource with the last, its slot going back
 * to `resources` (a pool's resource slabs) or, when that is NULL, dropping its
 * slab hold. NULL is a no-op. */
void markdown_core_resource_release(markdown_core_slab_pool *resources, markdown_core_resource *resource);

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

/* Puts `child` among `parent`'s children at `index` after the caller has
 * proved containment: stateful containment callbacks are never re-evaluated.
 * False, with nothing changed and the hold still the caller's, when storage
 * runs out. */
bool markdown_core_node_attach_validated(struct markdown_core_node_pool *pool, markdown_core_node *parent, size_t index,
                                         markdown_core_node *child);
/* `markdown_core_node_attach_validated` at the end of the children. */
bool markdown_core_node_append_validated(struct markdown_core_node_pool *pool, markdown_core_node *parent,
                                         markdown_core_node *child);

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

/* Drops the caller's hold on `node`: with its last holder it releases the
 * node, its children and every node-valued field it holds. */
void markdown_core_node_free(markdown_core_node *node);

/* Bytes another holder now holds too. */
static inline markdown_core_bytes *markdown_core_bytes_retain(markdown_core_bytes *bytes) {
    if (bytes) {
        bytes->refs++;
    }
    return bytes;
}

/* A node another holder now holds too. */
static inline markdown_core_node *markdown_core_node_retain(markdown_core_node *node) {
    if (node) {
        node->hold.refs++;
    }
    return node;
}

/* The node's children, in order. */
static inline size_t markdown_core_node_children_count(const markdown_core_node *node) {
    return markdown_core_children_count(node->children);
}
static inline markdown_core_node *markdown_core_node_child(const markdown_core_node *node, size_t index) {
    return markdown_core_children_at(node->children, index);
}
static inline markdown_core_node *markdown_core_node_first_child(const markdown_core_node *node) {
    return node->children ? markdown_core_children_at(node->children, 0) : NULL;
}
static inline markdown_core_node *markdown_core_node_last_child(const markdown_core_node *node) {
    return node->children ? markdown_core_children_last(node->children) : NULL;
}

/* Puts `child` among `node`'s children at `index`, taking the caller's hold
 * on it. False, with nothing changed and the hold still the caller's, when
 * `node` cannot contain it or storage runs out. */
bool markdown_core_node_insert_child(struct markdown_core_node_pool *pool, markdown_core_node *node, size_t index,
                                     markdown_core_node *child);
static inline bool markdown_core_node_append_child(struct markdown_core_node_pool *pool, markdown_core_node *node,
                                                   markdown_core_node *child) {
    return markdown_core_node_insert_child(pool, node, markdown_core_node_children_count(node), child);
}
/* Takes the child at `index` out of `node`, the caller then holding it.
 * NULL, with nothing changed, when storage runs out. */
markdown_core_node *markdown_core_node_take_child(struct markdown_core_node_pool *pool, markdown_core_node *node,
                                                  size_t index);

/* THE STRUCTURAL SELF-CHECK: the number of broken children trees under
 * `node`, each reported on `out` when it is given (children.h,
 * markdown_core_children_check), or -1 when the walk runs out of path
 * storage. */
int markdown_core_node_check(markdown_core_node *node, FILE *out);

/* The internal type's name, for diagnostics: "<unknown>" for a value no
 * class defines. Internal types the facade folds together (COMMENT_BLOCK and
 * COMMENT) keep their own names here. */
const char *markdown_core_node_get_type_string(markdown_core_node *node);

/* `markdown_core_node_free`, reporting how many nodes it released: the node,
 * its descendants and every owned field root under them that no other holder
 * held. The parse's own free (parser.h) counts this, so removal is observed
 * where it is done. */
size_t markdown_core_node_release(markdown_core_node *node);

/* Takes `buffer`'s storage as new bytes from `pool`'s slots, held once,
 * leaving the buffer empty; NULL when the bytes cannot be allocated. */
static inline markdown_core_bytes *markdown_core_bytes_take(markdown_core_node_pool *pool,
                                                            markdown_core_strbuf *buffer) {
    markdown_core_bytes *bytes = (markdown_core_bytes *)markdown_core_slab_take(
        pool ? &pool->slabs : NULL, pool ? &pool->bytes : NULL, sizeof(*bytes));
    if (bytes) {
        bytes->refs = 1;
        bytes->data = buffer->ptr;
        markdown_core_strbuf empty = MARKDOWN_CORE_BUF_INIT();
        *buffer = empty;
    }
    return bytes;
}

/* A holder's release of bytes, which frees them with their last holder. */
static inline void markdown_core_bytes_release(markdown_core_node_pool *pool, markdown_core_bytes *bytes) {
    if (bytes && !--bytes->refs) {
        markdown_core_free(bytes->data);
        markdown_core_slab_release(pool ? &pool->bytes : NULL, bytes);
    }
}

/* `markdown_core_node_new_with_ext` from a pool's slots. A NULL pool is the
 * allocator's own slot, which is what the parser-less constructor takes. */
markdown_core_node *markdown_core_node_pool_new(markdown_core_node_pool *pool, markdown_core_node_type type,
                                                const markdown_core_element *element);
/* `markdown_core_node_release` into a pool: the node slots and the slots of
 * the resources and runs the nodes held last go back to it for reuse rather
 * than dropping their slabs. A NULL pool is the plain release. */
size_t markdown_core_node_pool_release(markdown_core_node_pool *pool, markdown_core_node *node);
/* The same for a children tree's hold on its root run. */
size_t markdown_core_node_pool_release_children(markdown_core_node_pool *pool, markdown_core_run *run);
/* Seals the sums (children.h) of every unsealed run of the tree `root`,
 * whose children hold their extents; a sealed run's are already right. */
void markdown_core_children_seal(markdown_core_run *root);
/* Seals again the runs on the path to the child at `index` of `root`, whose
 * extent or record changed. */
void markdown_core_children_reseal(markdown_core_run *root, size_t index);
/* THE CHILD AT AN OFFSET of the sealed tree `root`, whose first child's lead
 * runs from `origin`: the first child that ends after `offset`, its index,
 * and where its lead starts. False when none does. */
bool markdown_core_children_find(const markdown_core_run *root, int64_t origin, int64_t offset, size_t *index,
                                 int64_t *lead);
/* The bytes the children of the sealed tree `root` before child `index`
 * cover, their leads and spans. */
int64_t markdown_core_children_length_before(const markdown_core_run *root, size_t index);
/* THE RUN A PARSE CAN TAKE FROM CHILD `first` of the sealed tree `root`
 * (docs/plans/2026-09-29-incremental-parsing.md, 5.3): the children from
 * `first` before `end` and before the first one an edit met, through the
 * last of them that can end a run. Its count, 0 for none, and its sums in
 * `sums`. */
size_t markdown_core_children_take_run(const markdown_core_run *root, size_t first, size_t end,
                                       markdown_core_run_sums *sums);
/* A run's storage, its entries having moved elsewhere or been released. */
void markdown_core_run_free_slot(markdown_core_node_pool *pool, markdown_core_run *run);
/* Drops what the pool holds: its released slots and its current slabs. Slots
 * still in use keep their slabs alive after this. */
void markdown_core_node_pool_dispose(markdown_core_node_pool *pool);

#ifdef __cplusplus
}
#endif

#endif
