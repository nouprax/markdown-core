#ifndef MARKDOWN_CORE_NODE_H
#define MARKDOWN_CORE_NODE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <assert.h>
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
 * It uses paragraph storage but never enters block content or a block close;
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

/* Where an entry a snapshot's document lists lies in a table it shares
 * with the committed parse (ast.c, A SNAPSHOT'S TABLES): its index among the
 * document's entries, and the index of the committed entry after it. */
typedef struct markdown_core_table_place {
    size_t at, next;
} markdown_core_table_place;

/* A TABLE of a published document, borrowed from the tree: `count` nodes,
 * every one of which `nodes` holds, or, in a snapshot's document, `own` of
 * them, each where `places` says, and the committed table's in between. */
typedef struct markdown_core_node_table {
    const struct markdown_core_node **nodes;
    size_t count, own;
    markdown_core_table_place *places;
} markdown_core_node_table;

/* A DEFINITION TABLE of a published document: every Footnote, every
 * Specimen, or every Reference, in source order. */
typedef struct markdown_core_definitions {
    markdown_core_node_table all;
    /* The labeled ones by label, in source order among equal labels: the
     * Footnotes and Specimens a lookup by label answers from, every one, or
     * a snapshot's own that no committed definition comes before. A label a
     * reference occurrence names resolves through the reference targets. */
    const struct markdown_core_node **labeled;
    size_t labeled_count;
} markdown_core_definitions;

/* THE DOCUMENT's own field: the metadata the properties envelope produced.
 * Footnote, specimen and Reference definitions stay in the tree where they
 * were written; publishing the document records its definition tables here,
 * and the nodes reference occurrences resolve to, by label: for each label,
 * the first Reference declaring it, or, when none does, the first Heading
 * whose text declares it. A snapshot's document shares the committed parse's
 * tables, `committed`, and lists what it adds to them; NULL in any other. */
typedef struct {
    struct markdown_core_node *metadata;
    markdown_core_definitions footnotes;
    markdown_core_definitions specimens;
    markdown_core_definitions references;
    markdown_core_node_table reference_targets;
    const struct markdown_core_committed_tables *committed;
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

/* A run of a node's own source (markdown_core.h): a source range whose lead
 * is from the end of the previous run, or, for the first, from where the
 * source of the node before it in its relation ends, or where its owner's
 * begins. */
#ifndef MARKDOWN_CORE_RUN_TYPEDEF
#define MARKDOWN_CORE_RUN_TYPEDEF
typedef struct markdown_core_run {
    int32_t lead;
    uint32_t span;
} markdown_core_run;
#endif

/* WHERE A NODE'S BYTES LIE. While a parse builds the tree, a node whose
 * extent alone does not say holds places: the source it read without
 * content, `length` 0, with the source that is not its own between them,
 * and, for an inline root, the source its content was read from, `length`
 * content bytes each. A place whose source is as long as its content reads
 * each content byte from one source byte; any other reads all of its content
 * from all of its source. Each holds an absolute source range, as a node's
 * place does. Publishing gives every node its runs, its own source with the
 * runs that touch joined and an empty place an empty run, relative as the
 * extent is: a block's from its places, written over them in their list, or
 * its range when it has none, and an inline node's from the places of its
 * root (ast.c). Each list is storage the node owns
 * (markdown_core_node_pool_bytes). */
typedef struct {
    uint32_t start, end, length;
} markdown_core_source_place;

typedef struct markdown_core_places {
    /* How many there are, and the content bytes they read: 0 for a node
     * without inline content. */
    uint32_t count, content;
    markdown_core_source_place items[];
} markdown_core_places;

typedef struct markdown_core_runs {
    uint32_t count;
    markdown_core_run items[];
} markdown_core_runs;

/* A node's places while the parse builds the tree, and its runs once it is
 * published, as its place becomes its extent. */
typedef union {
    markdown_core_places *places;
    markdown_core_runs *runs;
} markdown_core_node_source;

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
    /* Whether the last child the blank-line facts see that settled into a
     * block of a kind whose blank-line answer is its children's ends with a
     * blank line; the block's answer once it has closed, with BLANK_FOLDED
     * (markdown_core_block_ends_with_blank_line). */
    MARKDOWN_CORE_NODE__ENDS_BLANK = (1 << 2),
    /* A blank line separates two children the blank-line facts see in such
     * a block: one that settled into it ended with a blank line, and a later
     * one settled after it. */
    MARKDOWN_CORE_NODE__BLANK_SPACED = (1 << 3),
    // An HTML block whose own end condition matched on the line being
    // processed. `markdown_core_block_close` reads it to end the block on that line rather
    // than on the line before, and to know that a `-->` line really closed a
    // type-2 block rather than the input or a container running out.
    MARKDOWN_CORE_NODE__CLOSED_BY_END_CONDITION = (1 << 4),
    /* A block that settled in a commit (parser.h, THE BUILDERS): it is in
     * the committed tree, and a snapshot reads it and never writes it. */
    MARKDOWN_CORE_NODE__COMMITTED = (1 << 5),
    // Deferred contextual escape token, decoded when inline ownership is final.
    MARKDOWN_CORE_NODE__ESCAPED_SPACE = (1 << 6),
    /* A copy a snapshot made of a builder or of the pending block, which
     * borrows the original's children array, or its content buffer: it
     * writes only past the original's count, and releasing it gives back
     * neither (blocks.c, THE COPIES). */
    MARKDOWN_CORE_NODE__BORROWS_CHILDREN = (1 << 7),

    /* A block the blank-line facts read through, as CommonMark reads through
     * a link reference definition: whether a block ends with a blank line,
     * and whether another block follows it, are answered by the blocks
     * around it. */
    MARKDOWN_CORE_NODE__BLANK_TRANSPARENT = (1 << 8),
    /* A child the blank-line facts see has settled into the block, and
     * ENDS_BLANK holds that child's answer. */
    MARKDOWN_CORE_NODE__BLANK_FOLDED = (1 << 9),
    MARKDOWN_CORE_NODE__BORROWS_CONTENT = (1 << 10),
    /* A heading's anchor that the headings computed rather than one its
     * attributes wrote: it reserves no name, so releasing the heading
     * withdraws none (heading.c). */
    MARKDOWN_CORE_NODE__ANCHOR_COMPUTED = (1 << 11),
    /* A copy one of whose fields holds a COMMITTED node, which it shares
     * with the original -- the metadata of the document, which holds it once
     * its prefix is read: releasing the copy gives back none of them
     * (markdown_core_node_copy). */
    MARKDOWN_CORE_NODE__SHARES_FIELDS = (1 << 12),

    // The first bit an element may claim. Element flags are compile-time
    // constants owned by the element that uses them; there is no runtime
    // registration and no allocator to run out of bits.
    MARKDOWN_CORE_NODE__ELEMENT_FIRST = (1 << 13),
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

struct markdown_core_node {
    /* The node's PLACE: its children and its id. Every field after `id` is
     * its value. */
    /* The node's children in order, `child_count` of them, in storage the
     * node owns whose capacity the count determines
     * (markdown_core_node_push_child). A node holds no pointer to its parent
     * or its siblings: whatever holds a node holds it in an array or a
     * field. */
    struct markdown_core_node **children;
    uint32_t child_count;

    /* The node's identifier, unique within its document; 0 until the
     * document is published (markdown_core_publish_tree). */
    uint64_t id;

    markdown_core_attributes attributes;
    /* What the block read, only ever appended to: no close, line or step
     * writes a byte of it again. What a block makes of it is a view of it --
     * a literal block's literal, an inline root's text (markdown_core_node_text). */
    markdown_core_strbuf content;
    /* The bytes at the front and at the back of `content` that the node's
     * text leaves out, because its block read them as something else: a
     * paragraph's leading reference definitions and its trailing block
     * identifier. */
    bufsize_t cut_front, cut_back;
    /* Where the node is in the source; see markdown_core_node_where. */
    markdown_core_node_where where;
    int internal_offset;
    /* This node's slice of parser-owned content-to-source runs. Zero count
     * means there is no mapped content (for example, an empty cell). */
    markdown_core_content_map content_map;
    /* Its places, then its runs (markdown_core_node_source). */
    markdown_core_node_source source;
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

/* A PATH: where a block of a streaming parse's committed tree is, as the
 * path of the block that holds it and its index in that block's children
 * (blocks.c, THE PATHS), and where the block starts in the source. The
 * root's path holds nothing. A path names a place of the tree, which keeps
 * its meaning while the objects there change. */
typedef struct markdown_core_path {
    const struct markdown_core_path *parent;
    uint32_t index, start;
} markdown_core_path;

/* THE SCAFFOLD: a node's links while something is built of it, kept in the
 * node's slot beside the node.
 *
 * An inline tree is built with them while the inline parse that makes it
 * runs: a node's parent, its siblings and its first and last child. The
 * parse links and relinks nodes through them as it reduces delimiters and
 * brackets; completing the tree's root turns each container's scaffold
 * children into its children array, in order, and empties the chain
 * (blocks.c). A node outside an inline tree under construction has no
 * scaffold children.
 *
 * An open block is linked into the builder stack (parser.h, THE BUILDERS)
 * through its own: `parent` is the block it is open inside and `last` the
 * block open inside it, and a block's `last` empties as that block closes. A
 * block on the completion queue (parser.h) is linked into it through its
 * own: `next` is the block that joined after it and `parent` the block it
 * settled into, which completion asks its finish steps with; `next` empties
 * as the block leaves the queue, and `parent` stays, with the block's index
 * in the children of the block it settled into, as no block has a sibling
 * link (blocks.c, THE PATHS). A node the release walk takes is on the walk's
 * stack through its `next` (node.c), and a node whose state moved names the
 * node it moved to through its `next` (markdown_core_node_move). */
typedef struct markdown_core_scaffold {
    struct markdown_core_node *parent;
    /* An inline node's previous sibling; a settled block's index in the
     * children of the block it settled into; the path of an open block of a
     * streaming commit, once asked for (blocks.c, THE PATHS). */
    union {
        struct markdown_core_node *node;
        const struct markdown_core_path *path;
        uint32_t index;
    } prev;
    struct markdown_core_node *next, *first, *last;
} markdown_core_scaffold;

/* A NODE'S SLOT STORAGE (slab.h): the node's scaffold, the node, then room
 * for its kind's record. A C99 union aligns the node and the record space
 * after it for ordinary scalar fields, as the slot header before them is;
 * the node sits at one fixed offset in every slot whichever storage the slot
 * came from, and so does its scaffold. */
typedef union {
    struct markdown_core_node node;
    long double alignment;
    int64_t integer_alignment;
} markdown_core_node_allocation;

/* Room for a kind's record inside the slot. The bound is a property of the
 * slot, not of any kind: a record that fits is placed here, and one that does
 * not is owned through `node_data_allocation`. Construction and conversion
 * use this same capacity and ownership rule. Sixty-four bytes hold every
 * record but the metadata fields and a cross transclusion's, which are one
 * per document and rare. */
#define MARKDOWN_CORE_NODE_SLOT_RECORD_BYTES 64

typedef struct {
    markdown_core_scaffold scaffold;
    markdown_core_node_allocation node;
    unsigned char record[MARKDOWN_CORE_NODE_SLOT_RECORD_BYTES];
} markdown_core_node_slot;

/* A node's TEXT, the view of its content its inlines are parsed from: all of
 * it but its cuts. */
static inline markdown_core_chunk markdown_core_node_text(const markdown_core_node *node) {
    markdown_core_chunk text = {node->content.ptr + node->cut_front,
                                node->content.size - node->cut_front - node->cut_back, 0};
    return text;
}

static inline markdown_core_node_slot *markdown_core_node_slot_of(const markdown_core_node *node) {
    return (markdown_core_node_slot *)((unsigned char *)node - offsetof(markdown_core_node_slot, node));
}

static inline markdown_core_scaffold *markdown_core_scaffold_of(const markdown_core_node *node) {
    return &markdown_core_node_slot_of(node)->scaffold;
}

/* The fixed grammar and an element's retained kind domain, without callbacks.
 * The containment decision and the construction assertion use this same rule. */
bool markdown_core_node_can_contain_builtin(const markdown_core_node *node, markdown_core_node_type child_type);

/* THE CONSTRUCTION ASSERTION: the engine attaches a child -- to a children
 * array or to the scaffold -- only where its parent may hold it, by the
 * fixed grammar or by a dynamic policy decided before the child was made,
 * which is never asked again. */
#ifndef NDEBUG
bool markdown_core_node_may_attach(const markdown_core_node *parent, markdown_core_node_type child_type);
#endif

/* `child` at the end of `parent`'s scaffold children. */
static inline void markdown_core_scaffold_append(markdown_core_node *parent, markdown_core_node *child) {
    assert(markdown_core_node_may_attach(parent, (markdown_core_node_type)child->kind));
    markdown_core_scaffold *holder = markdown_core_scaffold_of(parent), *link = markdown_core_scaffold_of(child);
    link->parent = parent;
    link->prev.node = holder->last;
    link->next = NULL;
    if (holder->last) {
        markdown_core_scaffold_of(holder->last)->next = child;
    } else {
        holder->first = child;
    }
    holder->last = child;
}

/* `child` before `anchor` among `anchor`'s scaffold siblings. */
static inline void markdown_core_scaffold_insert_before(markdown_core_node *anchor, markdown_core_node *child) {
    markdown_core_scaffold *at = markdown_core_scaffold_of(anchor), *link = markdown_core_scaffold_of(child);
    assert(markdown_core_node_may_attach(at->parent, (markdown_core_node_type)child->kind));
    link->parent = at->parent;
    link->prev.node = at->prev.node;
    link->next = anchor;
    if (at->prev.node) {
        markdown_core_scaffold_of(at->prev.node)->next = child;
    } else {
        markdown_core_scaffold_of(at->parent)->first = child;
    }
    at->prev.node = child;
}

/* `node` out of its scaffold parent's children; its own children stay. */
static inline void markdown_core_scaffold_unlink(markdown_core_node *node) {
    markdown_core_scaffold *link = markdown_core_scaffold_of(node);
    if (!link->parent) {
        return;
    }
    markdown_core_scaffold *holder = markdown_core_scaffold_of(link->parent);
    if (link->prev.node) {
        markdown_core_scaffold_of(link->prev.node)->next = link->next;
    } else {
        holder->first = link->next;
    }
    if (link->next) {
        markdown_core_scaffold_of(link->next)->prev.node = link->prev.node;
    } else {
        holder->last = link->prev.node;
    }
    link->parent = link->next = NULL;
    link->prev.node = NULL;
}

static inline markdown_core_node *markdown_core_scaffold_parent(const markdown_core_node *node) {
    return markdown_core_scaffold_of(node)->parent;
}
static inline markdown_core_node *markdown_core_scaffold_next(const markdown_core_node *node) {
    return markdown_core_scaffold_of(node)->next;
}
static inline markdown_core_node *markdown_core_scaffold_prev(const markdown_core_node *node) {
    return markdown_core_scaffold_of(node)->prev.node;
}
static inline markdown_core_node *markdown_core_scaffold_first(const markdown_core_node *node) {
    return markdown_core_scaffold_of(node)->first;
}
static inline markdown_core_node *markdown_core_scaffold_last(const markdown_core_node *node) {
    return markdown_core_scaffold_of(node)->last;
}

/* Both cross kinds own the same raw reference fields in one payload allocation.
 * Only CrossEmbedded allocates the dimension value beside those fields. */
static inline markdown_core_cross_reference *markdown_core_node_cross_reference(const markdown_core_node *node) {
    return node->kind == MARKDOWN_CORE_NODE_CROSS_LINK ? node->as.cross_link : &node->as.cross_embedded->reference;
}

/* Takes ownership of `url` and `title` and answers a resource, or NULL
 * having taken nothing -- the caller still owns both chunks and frees them.
 * The resource's slot comes from `pool`'s resource slabs, or from the
 * allocator when it is NULL (slab.h). */
markdown_core_resource *markdown_core_resource_new(struct markdown_core_node_pool *pool, markdown_core_chunk url,
                                                   markdown_core_optional_chunk title);
/* Frees the resource, its slot going back to `resources` (a pool's resource
 * slabs) or, when that is NULL, dropping its slab hold. NULL is a no-op. */
void markdown_core_resource_free(markdown_core_slab_pool *resources, markdown_core_resource *resource);

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

bool markdown_core_node_can_contain_type(markdown_core_node *node, markdown_core_node_type child_type);

typedef int (*markdown_core_owned_subtree_visitor)(markdown_core_node **root_slot, void *context);

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

/* Releases `node`, its descendants and every node-valued field under them.
 * An inline node leaves the inline tree under construction that holds it
 * (THE SCAFFOLD); a block is held by an array or a field, never through its
 * scaffold, whose links name the builders and the queue. */
void markdown_core_node_free(markdown_core_node *node);

/* THE CAPACITY OF A NODE ARRAY -- a node's children, a cite's items -- is
 * the smallest power of two that holds its count, and at least two, the
 * pointers one class of a pool's storage holds: the count alone says when
 * the array is full. */
static inline uint32_t markdown_core_node_array_capacity(uint32_t count) {
    uint32_t capacity = 2;
    while (capacity < count) {
        capacity *= 2;
    }
    return capacity;
}

/* `node` as the first of the array `*items`, which holds nothing: the array
 * takes storage of two from `pool` (markdown_core_node_pool_bytes; NULL is
 * the allocator). False, having appended nothing, when no storage can be
 * had. */
bool markdown_core_node_array_start(struct markdown_core_node_pool *pool, markdown_core_node ***items, uint32_t *count,
                                    markdown_core_node *node);

/* Appends `node` to the full array `*items` holds `*count` of, at least two,
 * moving the array to storage from `pool` twice its capacity. False, having
 * appended nothing, when no storage can be had or the count is at its
 * bound. */
bool markdown_core_node_array_push_full(struct markdown_core_node_pool *pool, markdown_core_node ***items,
                                        uint32_t *count, markdown_core_node *node);

/* Appends `node` to the array `*items` holds `*count` of: an empty array
 * takes its storage (markdown_core_node_array_start), and one that is full --
 * at each power of two from two -- moves first
 * (markdown_core_node_array_push_full). */
static inline bool markdown_core_node_array_push(struct markdown_core_node_pool *pool, markdown_core_node ***items,
                                                 uint32_t *count, markdown_core_node *node) {
    const uint32_t held = *count;
    if (!held) {
        return markdown_core_node_array_start(pool, items, count, node);
    }
    if (held >= 2 && !(held & (held - 1))) {
        return markdown_core_node_array_push_full(pool, items, count, node);
    }
    (*items)[held] = node;
    *count = held + 1;
    return true;
}

/* `child` at the end of `node`'s children, with no containment check: the
 * caller decided that `node` holds it. False, having changed nothing, when
 * no storage can be had. */
static inline bool markdown_core_node_push_child(struct markdown_core_node_pool *pool, markdown_core_node *node,
                                                 markdown_core_node *child) {
    assert(markdown_core_node_may_attach(node, (markdown_core_node_type)child->kind));
    return markdown_core_node_array_push(pool, &node->children, &node->child_count, child);
}

/* The internal type's name, for diagnostics: "<unknown>" for a value no
 * class defines. Internal types the facade folds together (COMMENT_BLOCK and
 * COMMENT) keep their own names here. */
const char *markdown_core_node_get_type_string(markdown_core_node *node);

/* WHERE A NODE'S STORAGE COMES FROM, and where it goes back to.
 *
 * A node lives in a SLOT (slab.h) holding the node and room for its kind's
 * record, the resources links read through live in slots of their own, and
 * what else a node owns of its own size -- the lists of its source, a label
 * it declares -- in storage of that size, cut from the slabs its nodes are.
 * A pool holds the slabs of all three. A parse takes every slot from the
 * pool its caller lends it -- a session's, which outlives each of its
 * appends, or one the caller makes for a single parse -- and a caller with no
 * pool takes one slot from the allocator. A slot released into a pool goes
 * back to it for reuse, so a session's appends reuse the slots of the nodes
 * they retire instead of pinning a slab per append; a slot released with no
 * pool drops its slab hold. So a subtree unlinked from a parsed document is
 * as good as one built by hand: it outlives the pool it came from and is
 * released by `markdown_core_node_free` like any other. The size of what it
 * keeps alive is the slab, not the node.
 *
 * Why slabs: a node's chunk was larger than the C library's fast-path size
 * classes, so every release of one walked the allocator's merge path, and the
 * document's teardown cost more than a third of its parse. */
typedef struct markdown_core_node_pool {
    markdown_core_slab_pool nodes;
    markdown_core_slab_pool resources;
    markdown_core_bytes_pool bytes;
} markdown_core_node_pool;

#define MARKDOWN_CORE_NODE_SLAB_BYTES ((size_t)64 * 1024)

/* Uninitialized storage of `bytes` a node owns -- a list of its source, a
 * label it declares -- cut from the slabs of the pool's nodes, which a parse
 * always lends; NULL when none can be had. The node releases it with
 * itself. */
static inline void *markdown_core_node_pool_bytes(markdown_core_node_pool *pool, size_t bytes) {
    return markdown_core_bytes_take(&pool->bytes, &pool->nodes, bytes, MARKDOWN_CORE_NODE_SLAB_BYTES);
}
/* Gives back storage `markdown_core_node_pool_bytes` took, into `pool` for
 * reuse; a NULL pool is the plain release. */
void markdown_core_node_pool_bytes_free(markdown_core_node_pool *pool, void *storage);

/* `markdown_core_node_new_with_ext` from a pool's slots. A NULL pool is the
 * allocator's own slot, which is what the parser-less constructor takes. */
markdown_core_node *markdown_core_node_pool_new(markdown_core_node_pool *pool, markdown_core_node_type type,
                                                const markdown_core_element *element);
/* Exchanges everything two nodes' slots hold -- each node, its scaffold and
 * its record -- so each node takes the other's whole state, with what it
 * owns and borrows, and only the two addresses stay where they were
 * (ast.c, PUBLISHING CONTINUES THE PREVIOUS TREE). */
void markdown_core_node_swap(markdown_core_node *a, markdown_core_node *b);
/* A NODE MOVED TO A SLOT OF ITS OWN: a node in a new slot of `pool` holding
 * `node`'s whole state, which it takes over with what it owns; `node`'s slot
 * keeps its bytes, for whatever still reads it, names the node it moved to
 * (markdown_core_node_moved), and goes back to the pool with
 * markdown_core_node_slot_release. NULL when no slot can be had. */
markdown_core_node *markdown_core_node_move(markdown_core_node_pool *pool, markdown_core_node *node);
/* The node `node`'s state is in now: `node`, or the node it moved to, as
 * often as it moved. A node that never moved names none: no queue holds a
 * placed node (THE SCAFFOLD). */
static inline const markdown_core_node *markdown_core_node_moved(const markdown_core_node *node) {
    while (markdown_core_scaffold_next(node)) {
        node = markdown_core_scaffold_next(node);
    }
    return node;
}
/* Gives back the slot of a node whose state moved (markdown_core_node_move),
 * and nothing it held. */
void markdown_core_node_slot_release(markdown_core_node_pool *pool, markdown_core_node *node);
/* A COPY OF A BLOCK THE BLOCK PHASE STILL WRITES (blocks.c, THE COPIES):
 * a node of `node`'s kind in a slot of `pool`'s, holding a copy of each
 * thing `node` owns -- its attributes, its record and what the record owns,
 * its element's payload, its places -- and of each node its fields hold that
 * is not COMMITTED; it shares those that are (SHARES_FIELDS). Its children
 * array, when `node` has
 * children, and its content buffer, when `node` holds content storage, are
 * `node`'s, which it borrows (BORROWS_CHILDREN, BORROWS_CONTENT); its
 * scaffold is empty. NULL, having taken nothing, when storage could not be
 * had. */
markdown_core_node *markdown_core_node_copy(markdown_core_node_pool *pool, const markdown_core_node *node);
/* A CANDIDATE FOR A BLOCK COMPLETED AGAIN (blocks.c, A LOOKUP THAT FOUND
 * NOTHING): a node of `node`'s kind in a slot of `pool`'s, holding what the
 * block phase made of `node` and nothing its completion or its publish made:
 * a copy of each thing `node` owns -- its attributes, its record and what
 * the record owns, its element's payload -- its content buffer, which it
 * borrows (BORROWS_CONTENT), and, of each field root, a candidate of its
 * own. Its children are inline content its completion makes when `inlines`
 * says so, and `node`'s children array, which it borrows
 * (BORROWS_CHILDREN), otherwise. It has no id, and no place or places
 * until the caller gives them; its scaffold is empty. NULL, having taken
 * nothing, when storage could not be had. */
markdown_core_node *markdown_core_node_candidate(markdown_core_node_pool *pool, const markdown_core_node *node,
                                                 bool inlines);
/* The COMMITTED nodes the fields of `copy`, a copy of `original` that a
 * publish placed, hold, taken into the same fields of `original`: a field
 * node the two share stays one object when the publish exchanges it with
 * the node it continues (blocks.c, AFTER A PUBLISH). */
void markdown_core_node_share_fields(markdown_core_node *original, markdown_core_node *copy);
/* The storage `taker` and each of its field roots borrow, made theirs, and
 * lent by `lender` and its field roots in the same places, which borrow it
 * from them: `taker` is a candidate (markdown_core_node_candidate) and
 * `lender` the node whose state it was made from, released after it. */
void markdown_core_node_take_borrowed(markdown_core_node *taker, markdown_core_node *lender);
/* `markdown_core_node_free` into a pool: the node slots and the slots of
 * the resources the nodes held last go back to it for reuse rather than
 * dropping their slabs. A NULL pool is the plain release. */
void markdown_core_node_pool_release(markdown_core_node_pool *pool, markdown_core_node *node);
/* Drops what the pool holds: its released slots and its current slabs. Slots
 * still in use keep their slabs alive after this. */
void markdown_core_node_pool_dispose(markdown_core_node_pool *pool);

#ifdef __cplusplus
}
#endif

#endif
