#ifndef MARKDOWN_CORE_NODE_H
#define MARKDOWN_CORE_NODE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "node_type.h"
#include "markdown-core-element-api.h"
#include "buffer.h"
#include "chunk.h"
#include "attributes.h"
#include "slab.h"
#include "metadata.h"
#include "registry.h"

typedef struct {
    markdown_core_list_flavor flavor;
    int marker_offset;
    int padding;
    int start;
    markdown_core_ordered_list_variant variant;
    markdown_core_ordered_list_delimiter delimiter;
    unsigned char bullet_char;
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

/* THE DOCUMENT's own field: the metadata the properties envelope produced.
 * Footnote, specimen and Reference definitions stay in the tree where they
 * were written; the published document holds the registry's rosters of them
 * (registry.h) as its parse left them: the Footnotes, Specimens and
 * References in tree order, and for each footnote, specimen and reference
 * label the node it resolves to, in label order -- for a reference label, the
 * first Reference declaring it, or, when none does, the first Heading whose
 * text declares it. */
typedef struct {
    struct markdown_core_node *metadata;
    struct markdown_core_roster *rosters[MARKDOWN_CORE_ROSTER_COUNT];
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

/* A run of a node's source (markdown_core.h): its `source` range, whose lead
 * is from the end of the previous run, or, for the first, from where the
 * node's extent is measured from, in the source. */
#ifndef MARKDOWN_CORE_RUN_TYPEDEF
#define MARKDOWN_CORE_RUN_TYPEDEF
typedef struct markdown_core_run {
    markdown_core_extent source;
} markdown_core_run;
#endif

/* WHERE A NODE'S BYTES LIE, when its extent alone does not say: the runs of
 * source it read, with the source that is not its own between them. An
 * inline root's runs map its content too: each says how many content bytes
 * it decodes, those its content was read from and, decoding none, those that
 * gave no content. While a parse builds the tree each run holds an absolute
 * source range, as a node's place does; publishing rewrites them relative,
 * as it rewrites the place as the extent. The list is one owned allocation,
 * NULL when the node has none. */
typedef union {
    markdown_core_place place;
    markdown_core_run run;
} markdown_core_run_where;

/* A stretch of an inline root's source that one decoding reads: its source
 * bytes, and the content bytes it decodes them to. */
typedef struct markdown_core_run_piece {
    uint32_t span, decoded;
} markdown_core_run_piece;

typedef struct markdown_core_runs {
    /* How many runs there are and how many the list has room for, the
     * content bytes they decode (0 for a node without inline content), and
     * how many pieces follow the list's room when it decodes bytes
     * (markdown_core_runs_pieces). */
    uint32_t count, capacity, decoded, pieces;
    markdown_core_run_where items[];
} markdown_core_runs;

/* THE LINES OF A LEAF BLOCK (E5): what the line machine did with each
 * physical line the leaf took, in order, its opening line first. `span` runs
 * from the line's start to where the next line read begins, `text` to where
 * its content ends before its line ending, and `reach` is
 * how far past there the decisions on the line read. A PLAIN line continued
 * the leaf, which was the current block when the line began, with every
 * container prefix matched, and opened no block: its content begins `offset`
 * bytes into the line at `column`, after `indent` columns of indentation and
 * the rest of a TAB, and the leaf's own source `own` bytes into the line. A
 * BLANK line is blank past its prefixes. `lead` runs from the leaf's start to
 * its opening line's; a leaf whose lines the parse could not follow one by
 * one is BROKEN, and none of its lines is taken. */
enum { MARKDOWN_CORE_LINE_PLAIN = 1, MARKDOWN_CORE_LINE_BLANK = 2, MARKDOWN_CORE_LINE_TAB = 4 };
typedef struct markdown_core_line {
    uint32_t span, text, reach, own, offset;
    int32_t column, indent;
    uint32_t flags;
} markdown_core_line;

typedef struct markdown_core_lines {
    uint32_t count, capacity;
    int32_t lead;
    bool broken;
    markdown_core_line items[];
} markdown_core_lines;

/* THE PIECES OF AN INLINE ROOT'S RUNS, in source order: each stretch one
 * decoding reads (a copy, a tab's columns, a NUL, a cell's `\|`, a line
 * ending that is not LF, or source that gives no content). While the parse
 * holds the runs as places, piece `i` is run `i`'s; published, the runs are
 * the root's own source with touching runs joined, and its pieces fill them
 * in order. */
static inline markdown_core_run_piece *markdown_core_runs_pieces(const markdown_core_runs *runs) {
    return (markdown_core_run_piece *)(runs->items + runs->capacity);
}

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
    /* Part of the block's parse record (5.3): a line after its end, before
     * the next block, wrote into it or was read with it open, so a
     * candidate ends at it only before a block no edit meets. */
    MARKDOWN_CORE_NODE__TRAILED = (1 << 2),
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

    /* Part of the block's parse record (docs/plans/2026-09-29-incremental-
     * parsing.md, 5.3): a candidate does not end at it, because the line
     * after it read it: that line matched it, refused a start it would open
     * with the block closed (markdown_core_block_start_refuses), or split
     * the block (markdown_core_block_close). */
    MARKDOWN_CORE_NODE__HOLDS_NEXT = (1 << 11),

    /* A node of the tree a parse builds that holds old nodes it took whole
     * from an old node it does not continue (5.9): they are kept whole and
     * continue nothing, so a copy of one made to change it continues nothing
     * either (markdown_core_publication_splice). */
    MARKDOWN_CORE_NODE__FOSTERS = (1 << 12),

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
/* Every stem but the root of a tree holds at least half the width, so a tree
 * of n nodes is at most log base 16 of n high. */
#define MARKDOWN_CORE_STEM_FILL (MARKDOWN_CORE_STEM_WIDTH / 2)
#define MARKDOWN_CORE_STEM_HEIGHT 10

typedef struct markdown_core_stem markdown_core_stem;

typedef union {
    struct markdown_core_node *node;
    markdown_core_stem *stem;
} markdown_core_stem_entry;

/* A CHILDREN TREE'S SUMMARY (E4): a word `of` each node, and an associative
 * `combine` of the words of two adjacent runs of nodes, the front one first.
 * A stem built with a summary holds the combined word of the nodes under
 * it; one built without holds 0. */
typedef struct {
    uint64_t (*of)(const struct markdown_core_node *node);
    uint64_t (*combine)(uint64_t front, uint64_t back);
} markdown_core_stem_summary;

/* WHAT A STEM MEASURES of the nodes under it (5.1), from their extents and
 * their parse records once their owner has numbered them: `length`, the sum
 * of their leads and spans, which is how far the end of the last lies from
 * where the first is measured; `far`, how far past that end the furthest
 * of their reaches goes; their first order; and its marks. A stem made of nodes not numbered
 * yet is FRESH until markdown_core_stem_measure measures it. */
enum {
    /* A node under it is a group. */
    MARKDOWN_CORE_STEM_GROUP = 1,
    /* A node under it does not hold the next (MARKDOWN_CORE_NODE__HOLDS_NEXT). */
    MARKDOWN_CORE_STEM_FREE = 2,
    MARKDOWN_CORE_STEM_FRESH = 4,
};

struct markdown_core_stem {
    uint32_t refs;
    uint32_t count;
    uint8_t height;
    uint8_t width;
    uint8_t marks;
    uint64_t summary;
    int64_t length, far;
    /* The first order of the nodes under it (markdown_core_node), or NULL. */
    struct markdown_core_order *first;
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
    /* THE BLOCK'S PARSE RECORD (5.1), which the next parse of its session
     * reads, and no value of the document includes: its `entry`, the state
     * its parent carried where it began (markdown_core_parser_carry), and
     * its `reach`, how far past its end the decisions about it read, in
     * bytes once it is numbered and an absolute offset of the source until
     * then. Both are 0 for a node no line machine made. */
    uint64_t entry;
    uint32_t reach;
    /* The facts the node declares to the document, and the questions an
     * inline root it holds asked of it (registry.h), or NULL. */
    struct markdown_core_fact *facts;
    /* Where it lies in tree order when it declares facts, holds an inline
     * root or holds blocks read from its content (registry.h), and the first
     * order of its subtree, which is its own when it has one; NULL when
     * there is none. Set when its owner numbers it. */
    struct markdown_core_order *order, *first;

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
    /* A leaf block's lines (markdown_core_lines), or NULL. */
    markdown_core_lines *lines;

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
/* Frees the resource, its slot and the strings it holds going back to
 * `pool`, or, when that is NULL, dropping their slab holds. NULL is a
 * no-op. */
void markdown_core_resource_free(struct markdown_core_node_pool *pool, markdown_core_resource *resource);
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
    /* The facts of the nodes the pool's parses made (registry.h). */
    markdown_core_registry registry;
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

/* Makes every byte string `node`'s record holds its own, copying a view of
 * the input a parse read into `pool`'s storage. Completion calls it, so a
 * node that a later revision shares reads nothing another node owns. False
 * when a copy could not be allocated. */
bool markdown_core_node_hold_strings(markdown_core_node_pool *pool, markdown_core_node *node);

/* The bytes of a list of runs with room for `capacity` and `pieces`. */
static inline size_t markdown_core_runs_size(uint32_t capacity, uint32_t pieces) {
    return sizeof(markdown_core_runs) + (size_t)capacity * sizeof(markdown_core_run_where) +
           (size_t)pieces * sizeof(markdown_core_run_piece);
}
/* An empty list of runs from the pool's storage, with room for `capacity`,
 * and for a piece per run when `decoded`; NULL when it could not be had. */
static inline markdown_core_runs *markdown_core_runs_new(markdown_core_node_pool *pool, uint32_t capacity,
                                                         bool decoded) {
    const uint32_t pieces = decoded ? capacity : 0;
    markdown_core_runs *runs =
        (markdown_core_runs *)markdown_core_node_pool_bytes(pool, markdown_core_runs_size(capacity, pieces));
    if (runs) {
        runs->count = runs->decoded = 0;
        runs->capacity = capacity;
        runs->pieces = pieces;
    }
    return runs;
}

/* `markdown_core_node_new_with_ext` from a pool's slots. A NULL pool is the
 * allocator's own slot, which is what the parser-less constructor takes. */
markdown_core_node *markdown_core_node_pool_new(markdown_core_node_pool *pool, markdown_core_node_type type,
                                                const markdown_core_element *element);
/* A new node from a pool's slots equal to `node`, with its id, that shares
 * its children and fields and owns its own copy of everything else. NULL when
 * an allocation failed. */
markdown_core_node *markdown_core_node_copy(markdown_core_node_pool *pool, const markdown_core_node *node);
/* `markdown_core_node_release` into a pool: the node slots and the slots of
 * the resources the nodes held last go back to it for reuse rather than
 * dropping their slabs. A NULL pool is the plain release. */
size_t markdown_core_node_pool_release(markdown_core_node_pool *pool, markdown_core_node *node);
/* Drops what the pool holds: its released slots and its current slabs. Slots
 * still in use keep their slabs alive after this. */
void markdown_core_node_pool_dispose(markdown_core_node_pool *pool);

/* THE CHILDREN TREE'S OPERATIONS. */

/* A stem of the `count` nodes at `nodes`, in order, balanced, with
 * `summary` (which may be NULL): it takes the reference to each node the
 * caller held. NULL, having taken nothing, when `count` is 0 or an
 * allocation failed (`*failed`). */
markdown_core_stem *markdown_core_stem_make(markdown_core_node_pool *pool, markdown_core_node *const *nodes,
                                            size_t count, const markdown_core_stem_summary *summary, bool *failed);

static inline markdown_core_stem *markdown_core_stem_retain(markdown_core_stem *stem) {
    if (stem) {
        stem->refs++;
    }
    return stem;
}

/* Drops a reference to `stem` (which may be NULL), releasing what only it
 * held into `pool`. */
void markdown_core_stem_release(markdown_core_node_pool *pool, markdown_core_stem *stem);

/* The nodes of `front` followed by those of `back`, either of which may be
 * NULL, as one balanced stem with `summary`; it shares their stems and takes
 * the caller's references to both. NULL, having taken nothing, when an
 * allocation failed (`*failed`) or both are NULL. */
markdown_core_stem *markdown_core_stem_join(markdown_core_node_pool *pool, markdown_core_stem *front,
                                            markdown_core_stem *back, const markdown_core_stem_summary *summary,
                                            bool *failed);

/* The `count` nodes of `stem` from `index`, which it holds, as a balanced
 * stem with `summary` that shares the stems of `stem` they fill; a new
 * reference. NULL when `count` is 0 or an allocation failed (`*failed`). */
markdown_core_stem *markdown_core_stem_slice(markdown_core_node_pool *pool, const markdown_core_stem *stem,
                                             size_t index, size_t count, const markdown_core_stem_summary *summary,
                                             bool *failed);

/* The combined `summary` word of the `count` nodes of `stem` from `index`
 * (at least one, all of which it holds), read from the stems they fill. */
uint64_t markdown_core_stem_run_summary(const markdown_core_stem *stem, size_t index, size_t count,
                                        const markdown_core_stem_summary *summary);

static inline size_t markdown_core_stem_count(const markdown_core_stem *stem) { return stem ? stem->count : 0; }

/* Measures the fresh stems of `stem` (which may be NULL), whose nodes are
 * numbered now. */
void markdown_core_stem_measure(markdown_core_stem *stem);

/* The first node of `stem` (which may be NULL) at or after `index` that is
 * a group or whose reach meets `edge`: its end past its reach is at `edge`
 * or after, with the node at `index` measured from `*anchor`. Returns its
 * index, with `*anchor` where it is measured from, or the stem's count, with
 * `*anchor` where its last node ends, when none is. */
size_t markdown_core_stem_meet(const markdown_core_stem *stem, size_t index, int64_t *anchor, int64_t edge);

/* The last node of `stem` from `index` up to `end` that does not hold the
 * next; SIZE_MAX when none. */
size_t markdown_core_stem_last_free(const markdown_core_stem *stem, size_t index, size_t end);

/* How far the end of the last of the `count` nodes of `stem` from `index`
 * lies from where the first is measured: their leads and spans. */
int64_t markdown_core_stem_length(const markdown_core_stem *stem, size_t index, size_t count);

/* Whether a Footnote, Specimen, Reference or Heading has a label, and the
 * label in `label`. */
bool markdown_core_definition_label(const struct markdown_core_node *node, markdown_core_chunk *label);

/* The last node of `stem` whose first order (markdown_core_node) lies at
 * `label` or before it: the node whose subtree holds the order labelled
 * `label` when `stem`'s nodes hold it. SIZE_MAX when none does. */
size_t markdown_core_stem_find(const markdown_core_stem *stem, uint64_t label);

/* The node at `index` of the stem, which holds more than `index`. */
markdown_core_node *markdown_core_stem_at(const markdown_core_stem *stem, size_t index);

/* Puts `node` at `index` of `stem`, which no one else holds, and returns the
 * node it held there; the stem takes the caller's reference to `node` and
 * gives the caller its reference to the one it returns. */
markdown_core_node *markdown_core_stem_put(markdown_core_stem *stem, size_t index, markdown_core_node *node);

/* A new stem equal to `stem` but for `node` at `index`, which `stem` holds,
 * with `summary`: it shares the stems off the path to `index` and takes the
 * caller's reference to `node`; `stem` is unchanged. NULL, having taken
 * nothing, when an allocation failed (`*failed`). */
markdown_core_stem *markdown_core_stem_replace(markdown_core_node_pool *pool, const markdown_core_stem *stem,
                                               size_t index, markdown_core_node *node,
                                               const markdown_core_stem_summary *summary, bool *failed);

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

/* WHAT THE DECISIONS ABOUT AN INLINE NODE READ (docs/plans/2026-09-29-
 * incremental-parsing.md, 5.6), kept by its member while its root's content
 * is parsed and completed. The node lies on the content offsets `start` to
 * `end`. `rules` are the delimiter rules (one bit per rule) whose stack
 * entries a decision about it counted or searched; `state` the rules that had
 * entries on the stack where it begins, and HELD when a token still open
 * there (a bracket, a citation token, an opaque body) can change what
 * follows. The decisions read the content from `low` to `reach`, a range in
 * which -1 is the start of the content and one past its length is its end.
 * `stay` names the delimiter its token pushed (parser.h, `stays`), and
 * `until` is the furthest offset at which a delimiter it holds left the
 * stack. It is RECORDED when every decision about it said what it read; it
 * leaves a whitespace BOUNDARY on the stack; its completion read the nodes
 * around it when it has CONTEXT; and it is LOCAL when its decisions read
 * nothing but the root's content and the parse of it, asking no registry
 * and no element that did not say what it read. */
#define MARKDOWN_CORE_INLINE_HELD (1u << 31)
#define MARKDOWN_CORE_INLINE_RECORDED 1u
#define MARKDOWN_CORE_INLINE_BOUNDARY 2u
#define MARKDOWN_CORE_INLINE_CONTEXT 4u
#define MARKDOWN_CORE_INLINE_LOCAL 8u
typedef struct markdown_core_inline_reads {
    uint32_t rules, state;
    int32_t start, end, low, reach, until;
    uint32_t stay, flags;
} markdown_core_inline_reads;

/* AN INLINE NODE'S ENTRY, as its node keeps it for the next parse (5.6): zero
 * for a node no parse takes whole. A LOCAL node is taken whole where its
 * root's content is all the old one's (inlines.c, S_cursor_open); a TAKE
 * node, which is LOCAL, also where only the bytes its decisions read are,
 * with the rules whose stack must be empty where it is taken and how many
 * content bytes before its start its decisions read. BOUNDARY when taking it
 * leaves a whitespace boundary on the stack. Its `reach` counts the content
 * bytes after its end they read. A rule is one bit of the low sixteen. */
#define MARKDOWN_CORE_INLINE_ENTRY_TAKE (1ull << 16)
#define MARKDOWN_CORE_INLINE_ENTRY_BOUNDARY (1ull << 17)
#define MARKDOWN_CORE_INLINE_ENTRY_LOCAL (1ull << 18)
static inline uint64_t markdown_core_inline_entry(bool take, uint32_t rules, bool boundary, uint32_t back) {
    return MARKDOWN_CORE_INLINE_ENTRY_LOCAL | (take ? MARKDOWN_CORE_INLINE_ENTRY_TAKE : 0) |
           (boundary ? MARKDOWN_CORE_INLINE_ENTRY_BOUNDARY : 0) | ((uint64_t)back << 32) | (rules & 0xffffu);
}
static inline uint32_t markdown_core_inline_entry_rules(uint64_t entry) { return (uint32_t)entry & 0xffffu; }
static inline uint32_t markdown_core_inline_entry_back(uint64_t entry) { return (uint32_t)(entry >> 32); }

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
 * its old coordinates, `passed`, the image up to which its owner's cursor
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
    uint32_t old_start, passed, pair_anchor, pair_name;
    /* The old node an open block reads again (5.3), the old offset where it
     * starts, which of its children the block's cursor is at and the old
     * offset where the child before that one ends; and whether every block
     * from the document to this one carried the state its old node's parent
     * carried where that node began, so that the old children may be taken. */
    const markdown_core_node *scan;
    uint32_t scan_start, scan_at;
    size_t scan_next;
    bool scan_equal;
    /* A CANDIDATE, old children the parse took whole (5.3), held as one stem: the
     * member stands for all of them, its node is the last of them, and
     * `past` is the index after them among the old children they were taken
     * from. NULL for a member of one node. */
    markdown_core_stem *candidate;
    size_t past;
    /* THE ORDERS OF ITS SUBTREE (registry.h), as its completion and its
     * owner's numbering collect them: the new ones in tree order from
     * `order_first` to `order_last`, linked by `next`. An old order the
     * subtree holds is the `before` of the new ones ahead of it: until the
     * next old one comes, those from `order_open` on have none.
     * `order_lead` is the first old one, and `order_head` the first one of
     * either. */
    markdown_core_order *order_first, *order_last, *order_open, *order_lead, *order_head;
    uint32_t asks, index, slot, source, waits, candidates, last_candidate;
    /* What the decisions about a node of an inline root's content read, and
     * whether the node is an old one its parse took whole (5.6). */
    markdown_core_inline_reads reads;
    bool held, field, inner, decided, identified, paired, asked, numbered, counted, taken;
    /* Whether blocks are read from its node's content (markdown_core_parser_queue_block_input). */
    bool queued;
};

/* Where `member`'s node lies: its place, which numbering keeps in the member
 * as the node takes its extent. */
static inline markdown_core_place markdown_core_member_place(const markdown_core_member *member) {
    return member->numbered ? member->place : member->node->where.place;
}

/* Whether `owner`'s node may hold `child`'s: its built-in containment, which
 * is pure and shares its rules with checked construction, or an element's
 * dynamic policy, decided before and never replayed. */
bool markdown_core_member_admits(const markdown_core_member *owner, const markdown_core_member *child);

#define MARKDOWN_CORE_MEMBER_SLAB_BYTES ((size_t)16 * 1024)

/* A member for `node`, linked to nothing; it holds the node's reference when
 * `held`. NULL when it could not be allocated. */
static inline markdown_core_member *markdown_core_member_new(markdown_core_node_pool *pool, markdown_core_node *node,
                                                             bool held) {
    markdown_core_member *member = (markdown_core_member *)markdown_core_slab_take(
        pool ? &pool->members : NULL, sizeof(*member), MARKDOWN_CORE_MEMBER_SLAB_BYTES);
    if (member) {
        memset(member, 0, sizeof(*member));
        member->node = node;
        member->held = held;
    }
    return member;
}

/* Whether `member` has decided to continue no old node: then nothing it holds
 * continues one either, and every member it holds has decided so too (5.9). */
static inline bool markdown_core_member_continues_nothing(const markdown_core_member *member) {
    return member->decided && !member->old;
}

/* The first member `member` holds: its first field root, or its first
 * child. */
static inline markdown_core_member *markdown_core_member_first_held(const markdown_core_member *member) {
    return member->fields ? member->fields : member->first;
}

/* The member `member`'s owner holds after it: the next field root, then the
 * first child, then the next child. */
static inline markdown_core_member *markdown_core_member_held_after(const markdown_core_member *member) {
    return member->next ? member->next : member->field ? member->owner->first : NULL;
}

/* `member` decides to continue nothing, and so does every member it holds
 * that had not decided: they continue nothing whatever they become. */
static inline void markdown_core_member_continue_nothing(markdown_core_member *member) {
    member->decided = true;
    /* A member that had decided continues nothing, and so has everything it
     * holds, or continues an old node, which what it holds searches. */
    markdown_core_member *at = markdown_core_member_first_held(member);
    while (at) {
        markdown_core_member *below = NULL;
        if (!at->decided) {
            at->decided = true;
            below = markdown_core_member_first_held(at);
        }
        if (below) {
            at = below;
            continue;
        }
        while (at != member && !markdown_core_member_held_after(at)) {
            at = at->owner;
        }
        at = at == member ? NULL : markdown_core_member_held_after(at);
    }
}

/* What `member` takes from the `owner` it is linked under: it is inner when
 * `owner` is, and continues nothing when `owner` does. */
static inline void markdown_core_member_inherit(const markdown_core_member *owner, markdown_core_member *member) {
    member->inner = owner->inner;
    if (!member->decided && markdown_core_member_continues_nothing(owner)) {
        markdown_core_member_continue_nothing(member);
    }
}

/* Links the detached `child` under `owner`, before `before` (a child of
 * `owner`) or last, and it inherits from `owner`. The caller has proved
 * containment. */
static inline void markdown_core_member_attach(markdown_core_member *owner, markdown_core_member *child,
                                               markdown_core_member *before) {
    assert(owner && child && owner != child);
    assert(!child->owner && !child->prev && !child->next);
    assert(!before || before->owner == owner);
    assert(markdown_core_member_admits(owner, child));
    markdown_core_member *previous = before ? before->prev : owner->last;
    child->owner = owner;
    markdown_core_member_inherit(owner, child);
    child->prev = previous;
    child->next = before;
    if (previous) {
        previous->next = child;
    } else {
        owner->first = child;
    }
    if (before) {
        before->prev = child;
    } else {
        owner->last = child;
    }
}

/* Links the detached `field` as the last field root `owner` builds, and it
 * inherits from `owner`. */
void markdown_core_member_attach_field(markdown_core_member *owner, markdown_core_member *field);

/* Detaches `member` from its owner and siblings, keeping its subtree. */
static inline void markdown_core_member_unlink(markdown_core_member *member) {
    markdown_core_member *owner = member->owner;
    if (member->field) {
        markdown_core_member **at = &owner->fields;
        while (*at != member) {
            at = &(*at)->next;
        }
        *at = member->next;
        member->field = false;
    } else {
        if (member->prev) {
            member->prev->next = member->next;
        } else if (owner) {
            owner->first = member->next;
        }
        if (member->next) {
            member->next->prev = member->prev;
        } else if (owner) {
            owner->last = member->prev;
        }
    }
    member->owner = member->prev = member->next = NULL;
}

/* COMPLETES A BUILDER'S STRUCTURE: the nodes of `member`'s children become
 * its node's stem, which takes the references their members held and
 * shares the stems of the runs they hold; the
 * members stay, for the node's numbering, and the stem keeps the `summary`
 * of the node's kind (E4), which may be NULL. False, changing nothing, when
 * the stem could not be allocated. */
bool markdown_core_member_freeze(markdown_core_node_pool *pool, markdown_core_member *member,
                                 const markdown_core_stem_summary *summary);

/* Releases `member`, every member below it, and the references they hold;
 * `member` was detached first, or is a root. */
void markdown_core_member_release(markdown_core_node_pool *pool, markdown_core_member *member);

#ifdef __cplusplus
}
#endif

#endif
