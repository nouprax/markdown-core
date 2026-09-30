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
 * many resolved before it. `holders` counts the map record and every node
 * reading through the resource; the last one out frees it, which is how the
 * tree outlives the parser that built the map.
 *
 * Identity is the pointer: `markdown_core_node_resource` hands it out, and a
 * consumer that materializes a destination once per distinct resource keys
 * on it. Nothing else about the pointer is stated. */
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

/* THE CITE (M4): a `Cite` owns its items as a chain of CITATION nodes beside
 * its children, which it never has. The chain is a node-valued field, not
 * content: the items are scoped values, not `Markup`. */
typedef struct {
    struct markdown_core_node *citations;
} markdown_core_cite;

/* THE REFERENT of one citation (M4): a tagged value. A `bib` referent, which
 * the citations module first produces with P7, carries a key and a mode; a
 * `footnote` referent carries the label of the `Footnote` it names, or owns
 * the inline note it stands for; a `specimen` referent carries a label. */
typedef enum {
    MARKDOWN_CORE_NODE_REFERENT_BIB = 1,
    MARKDOWN_CORE_NODE_REFERENT_FOOTNOTE = 2,
    MARKDOWN_CORE_NODE_REFERENT_SPECIMEN = 3
} markdown_core_node_referent_kind;

/* ONE ITEM of a cite (M4): the referent, and the item's owned fields: the
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

/* A DEFINITION TABLE of a published document: every Footnote, or every
 * Specimen, in source order, borrowed from the tree. */
typedef struct markdown_core_definitions {
    /* Every definition of the kind, in source order. */
    const struct markdown_core_node **nodes;
    size_t count;
    /* The labeled ones by label, in source order among equal labels. */
    const struct markdown_core_node **labeled;
    size_t labeled_count;
} markdown_core_definitions;

/* THE DOCUMENT's own field: the metadata the properties envelope produced.
 * Footnote and specimen definitions stay in the tree where they were
 * written; publishing the document records its definition tables here. */
typedef struct {
    struct markdown_core_node *metadata;
    markdown_core_definitions footnotes;
    markdown_core_definitions specimens;
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
    // A finalized paragraph consumed entirely by reference definitions. It
    // retains block adjacency until block parsing ends, then is discarded
    // before list layout and inline parsing observe the semantic children.
    MARKDOWN_CORE_NODE__REFERENCE_DEFINITION_ONLY = (1 << 5),

    // Deferred contextual escape token, decoded when inline ownership is final.
    MARKDOWN_CORE_NODE__ESCAPED_SPACE = (1 << 6),

    /* Reference definitions have advanced this block's semantic beginning.
     * A later arrival may supply its first surviving content line. */
    MARKDOWN_CORE_NODE__REFERENCE_PREFIX = (1 << 7),

    // The first bit an element may claim. Element flags are compile-time
    // constants owned by the element that uses them; there is no runtime
    // registration and no allocator to run out of bits.
    MARKDOWN_CORE_NODE__ELEMENT_FIRST = (1 << 8),
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
    markdown_core_cite *cite;
    markdown_core_citation_item *citation;
    markdown_core_footnote_value *footnote;
    markdown_core_specimen_value *specimen;
    markdown_core_document_value *document;
    markdown_core_metadata_fields *metadata;
    markdown_core_definition *definition;
    markdown_core_definition_body_value *definition_body;
    markdown_core_html_block *html_block;
    markdown_core_table_cell *table_cell;
} markdown_core_node_data;

struct markdown_core_node {
    /* The node's PLACE: its links and its id. Every field after `id` is its
     * value (markdown_core_node_swap_values). */
    struct markdown_core_node *next;
    struct markdown_core_node *prev;
    struct markdown_core_node *parent;
    /* Intrusive list of content children. */
    struct markdown_core_node *first_child;
    struct markdown_core_node *last_child;

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
const markdown_core_chunk *markdown_core_node_anchor_chunk(const markdown_core_node *node);

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

/* Commit an exclusively owned, detached subtree after the caller has proved
 * containment and disjointness. No callbacks, allocation, or rejection occurs
 * after ownership starts to move. Debug/ASan checks the pointer links and
 * pure built-in containment; stateful callbacks are never re-evaluated. */
void markdown_core_node_attach_validated(markdown_core_node *parent, markdown_core_node *child,
                                         markdown_core_node *before);

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

/* Releases `node`, its descendants and every node-valued field under them. */
void markdown_core_node_free(markdown_core_node *node);

/* Detaches `node` from its parent and siblings without releasing it. */
void markdown_core_node_unlink(markdown_core_node *node);

/* Moves `child` to the end of `node`'s children. Returns 0, having moved
 * nothing, when `node` cannot contain `child`. */
int markdown_core_node_append_child(markdown_core_node *node, markdown_core_node *child);

/* The internal type's name, for diagnostics: "<unknown>" for a value no
 * class defines. Internal types the facade folds together (COMMENT_BLOCK and
 * COMMENT) keep their own names here. */
const char *markdown_core_node_get_type_string(markdown_core_node *node);

/* `markdown_core_node_free`, reporting how many nodes it released: the node,
 * its descendants and every owned field root under them. The parse's own
 * free (parser.h) counts this, so removal is observed where it is done. */
size_t markdown_core_node_release(markdown_core_node *node);

/* WHERE A NODE'S STORAGE COMES FROM, and where it goes back to.
 *
 * A node lives in a SLOT (slab.h) holding the node and room for its kind's
 * record, and the resources links read through live in slots of their own.
 * A pool holds the slabs of both. A parse takes every slot from the pool its
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
} markdown_core_node_pool;

/* `markdown_core_node_new_with_ext` from a pool's slots. A NULL pool is the
 * allocator's own slot, which is what the parser-less constructor takes. */
markdown_core_node *markdown_core_node_pool_new(markdown_core_node_pool *pool, markdown_core_node_type type,
                                                const markdown_core_element *element);
/* Exchanges the values of two nodes of one kind with the same node-valued
 * fields: everything but their places, which are their links, their ids and
 * the nodes their fields hold. Each value keeps the storage it borrows from,
 * so a node takes the other's value with the other's storage. */
void markdown_core_node_swap_values(markdown_core_node *a, markdown_core_node *b);
/* `markdown_core_node_release` into a pool: the node slots and the slots of
 * the resources the nodes held last go back to it for reuse rather than
 * dropping their slabs. A NULL pool is the plain release. */
size_t markdown_core_node_pool_release(markdown_core_node_pool *pool, markdown_core_node *node);
/* Drops what the pool holds: its released slots and its current slabs. Slots
 * still in use keep their slabs alive after this. */
void markdown_core_node_pool_dispose(markdown_core_node_pool *pool);

#ifdef __cplusplus
}
#endif

#endif
