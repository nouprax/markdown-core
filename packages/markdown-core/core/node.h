#ifndef MARKDOWN_CORE_NODE_H
#define MARKDOWN_CORE_NODE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdint.h>

#include "markdown-core.h"
#include "markdown-core-extension-api.h"
#include "buffer.h"
#include "chunk.h"
#include "attributes.h"
#include "metadata.h"

typedef struct {
    markdown_core_list_type list_type;
    int marker_offset;
    int padding;
    int start;
    markdown_core_delim_type delimiter;
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

/* THE RESOURCE a Link or Image reads its destination and title from (M2).
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
    size_t holders;
};
#ifndef MARKDOWN_CORE_RESOURCE_TYPEDEF
#define MARKDOWN_CORE_RESOURCE_TYPEDEF
typedef struct markdown_core_resource markdown_core_resource;
#endif

typedef struct {
    /* NEVER NULL on a node the parser finished: the resource is attached in
     * the same step that makes the node a link, and an allocation that could
     * not attach one frees the node. */
    markdown_core_resource *resource;
    markdown_core_optional_i64 width;
    markdown_core_optional_i64 height;
} markdown_core_link;

/* One authored workspace reference. Each occurrence owns its raw strings;
 * unlike resolved links, it has no resource shared with a definition. */
typedef struct {
    markdown_core_chunk path;
    markdown_core_optional_chunk anchor;
    markdown_core_optional_chunk label;
    bool embedded;
} markdown_core_cross_link;

/* THE CITE (M4): a `Cite` owns its items as a chain of CITATION nodes beside
 * its children, which it never has. The chain is a node-valued field, not
 * content: the items are scoped values, not `Markup`. */
typedef struct {
    struct markdown_core_node *citations;
} markdown_core_cite;

/* THE REFERENT of one citation (M4): a tagged value. A `bib` referent, which
 * the citations module first produces with P7, carries a key and a mode; a
 * `footnote` referent carries the id of the `Footnote` it names. */
typedef enum {
    MARKDOWN_CORE_NODE_REFERENT_BIB = 1,
    MARKDOWN_CORE_NODE_REFERENT_FOOTNOTE = 2,
    MARKDOWN_CORE_NODE_REFERENT_SPECIMEN = 3
} markdown_core_node_referent_kind;

/* ONE ITEM of a cite (M4): the referent, and two affix chains the item owns
 * beside its children, which it never has. `value` is the referent's key or
 * id: for a footnote referent it is the label under the map's own
 * normalization WITHOUT the caret, which is the `Footnote.id` it names,
 * computed once per occurrence. NORMATIVE: an id is compared with memcmp over
 * its bytes and is never case mapped, renormalized, or re-encoded. A chain is
 * NULL when the affix is empty. */
typedef struct {
    markdown_core_node_referent_kind referent;
    markdown_core_chunk value;
    /* The bib mode as the public `markdown_core_bib_mode` numbers it; 0 for a
     * footnote referent. */
    int mode;
    struct markdown_core_node *prefix;
    struct markdown_core_node *suffix;
} markdown_core_citation_item;

/* A FOOTNOTE (M4): the id is the definition's label under the map's own
 * normalization, without the caret, the key every call's referent names; the
 * content is the node's children. */
typedef struct {
    markdown_core_chunk id;
} markdown_core_footnote_value;

/* A specimen owns its optional authored label and effective explicit counter
 * reset. Anonymous definitions and absent resets remain absent; numbering is
 * derived from the document's definition order by consumers. */
typedef struct {
    markdown_core_optional_chunk id;
    int64_t start;
    bool has_start;
} markdown_core_specimen_value;

/* THE DOCUMENT's own footnotes (M4): every footnote definition leaves the tree
 * when the document finalizes and is chained here in ascending scope order, a
 * node-valued field the root owns beside its content. */
typedef struct {
    markdown_core_metadata *metadata;
    struct markdown_core_node *footnotes;
    struct markdown_core_node *specimens;
} markdown_core_document_value;

/* A link reference definition is not a node (M2). The block phase reads it off
 * the front of the paragraph that held it into the parser's map, which owns
 * its resource once, and every reference that resolves to it is the `Link` or
 * `Image` it names, sharing that resource. This is the inherited grammar's
 * model: a definition exists to be referred to, an unreferenced one produces
 * nothing, and the first definition of a label in source order wins. A footnote
 * definition stays a node while it is parsed, because its body is flow
 * content, and becomes a document-owned `Footnote` value when the document
 * finalizes (M4). */

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

    // The first bit an extension may claim. Extension flags are compile-time
    // constants owned by the extension that uses them; there is no runtime
    // registration and no allocator to run out of bits.
    MARKDOWN_CORE_NODE__EXTENSION_FIRST = (1 << 5),
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
 * has no record. Retyping keeps node identity stable and installs a separately
 * allocated replacement. The common node layout never depends on record size. */
typedef union {
    void *data;
    markdown_core_chunk *literal;
    markdown_core_list *list;
    markdown_core_code *code;
    markdown_core_heading *heading;
    markdown_core_link *link;
    markdown_core_cross_link *cross_link;
    markdown_core_cite *cite;
    markdown_core_citation_item *citation;
    markdown_core_footnote_value *footnote;
    markdown_core_specimen_value *specimen;
    markdown_core_document_value *document;
    markdown_core_html_block *html_block;
    markdown_core_table_cell *table_cell;
} markdown_core_node_data;

struct markdown_core_node {
    markdown_core_attributes attributes;
    markdown_core_strbuf content;

    struct markdown_core_node *next;
    struct markdown_core_node *prev;
    struct markdown_core_node *parent;
    /* Intrusive list of content children. */
    struct markdown_core_node *first_child;
    struct markdown_core_node *last_child;

    void *user_data;
    markdown_core_free_func user_data_free_func;

    int start_line;
    int start_column;
    int end_line;
    int end_column;
    int internal_offset;
    /* This node's slice of parser-owned content-to-source runs. Zero count
     * means there is no mapped content (for example, an empty cell). */
    int content_mark;
    int content_mark_count;
    /* A slice reads immutable parser-owned marks at this content origin. */
    int content_mark_offset;
    uint16_t kind;
    markdown_core_node_internal_flags flags;

    const markdown_core_extension *extension;
    /* Extension-owned data, allocated by opaque_alloc_func and released by
     * opaque_free_func. It survives kind changes independently of `as`. */
    void *opaque;

    /* Owns a replacement record, when present. The initial record belongs to
     * the node allocation instead. `as` is the typed view in either case. */
    void *node_data_allocation;
    markdown_core_node_data as;
};

static MARKDOWN_CORE_INLINE markdown_core_mem *markdown_core_node_mem(markdown_core_node *node) {
    return node->content.mem;
}

/* Takes ownership of `url` and `title` and answers a resource with one holder,
 * or NULL having taken nothing -- the caller still owns both chunks and frees
 * them. */
markdown_core_resource *markdown_core_resource_new(markdown_core_mem *mem, markdown_core_chunk url,
                                                   markdown_core_optional_chunk title);
void markdown_core_resource_retain(markdown_core_resource *resource);
/* Drops one holder and frees the resource with the last. NULL is a no-op. */
void markdown_core_resource_release(markdown_core_mem *mem, markdown_core_resource *resource);
MARKDOWN_CORE_EXPORT int markdown_core_node_check(markdown_core_node *node, FILE *out);

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

MARKDOWN_CORE_EXPORT bool markdown_core_node_can_contain_type(markdown_core_node *node,
                                                              markdown_core_node_type child_type);

#ifdef __cplusplus
}
#endif

#endif
