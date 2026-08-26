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

typedef struct {
    markdown_core_list_type list_type;
    int marker_offset;
    int padding;
    int start;
    markdown_core_delim_type delimiter;
    unsigned char bullet_char;
    bool tight;
    bool checked; // For task list extension
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

typedef struct {
    /* REQUIRED (Q26). `[a]()` and `[a](<>)` wrote a destination and it was
     * empty; there is no link whose author wrote no destination at all,
     * because the shortcut and collapsed forms are references and carry none. */
    markdown_core_chunk url;
    /* OPTIONAL (requirement 14): `[a](/u)` wrote no title and `[a](/u "")`
     * wrote an empty one. */
    markdown_core_optional_chunk title;
} markdown_core_link;

/* THE ASSOCIATION every reference and definition carries. TWO values, and
 * neither derives the other in either direction.
 *
 * `label` is the bytes between the delimiters exactly as written: escapes and
 * character references unresolved, whitespace uncollapsed, case unfolded.
 * `identifier` is the match key: full Unicode case fold, trim, collapse
 * internal whitespace -- and for a footnote it KEEPS its leading `^`, so that a
 * link definition and a footnote definition of the same name cannot collide in
 * a consumer's single map. That caret is a correction to mdast, which separates
 * the two namespaces only by node type and so cannot survive being flattened
 * onto a wire.
 *
 * NORMATIVE: `identifier` is compared with memcmp over its bytes. It is never
 * case mapped, never NFC/NFD normalized, never re-encoded, and never used as a
 * key in a language map whose `==` has an opinion about Unicode -- Swift's
 * `String ==` is canonical equivalence, which would collapse the NFC and NFD
 * spellings of `[cafe\u0301]` that this parser deliberately keeps apart.
 *
 * NEITHER derives the other. `raw -> key` needs the case-fold table; `key ->
 * raw` is impossible, because the fold is many-to-one and `[ss]` and
 * `[\u00df]` are two labels with one key. The producer computes the key at zero
 * marginal cost: it already builds one per occurrence for its own map. */
typedef struct {
    markdown_core_chunk label;
    markdown_core_chunk identifier;
} markdown_core_association;

/* A link or image reference: Association + the form it was written in + THE
 * DEFINITION IT RESOLVED TO. IT HOLDS NO DESTINATION -- the destination is
 * stated once, at the definition, which is what D9's budget existed to bound
 * and what deleting the copy removes the reason for.
 *
 * `definition` is the winning definition BLOCK's identity (its full identity
 * is the pair (definition, 0)); a reference node exists only because a lookup
 * succeeded, so the field is never "unresolved". It is a VALUE and never a
 * pointer: a map that owned a node is how a definition came to be freed while
 * the tree still pointed at it (D11), and an identity survives the definition
 * node it names. */
typedef struct {
    markdown_core_association association;
    markdown_core_reference_form form;
    uint32_t definition;
} markdown_core_reference_link;

/* A footnote call: Association + the definition it resolved to, exactly as a
 * link reference names its own. No form -- there is one footnote call syntax
 * (Q3), so a form field would hold one value. */
typedef struct {
    markdown_core_association association;
    uint32_t definition;
} markdown_core_footnote_reference;

/* A link reference definition is a block node at the byte where its opening
 * bracket was written, in the container it was written in, and it stays there.
 * There are two kinds of reference definition and they differ in exactly one
 * thing: what the definition's body is. A link reference definition's body is a
 * resource -- a destination and an optional title -- so it is a leaf. A
 * footnote definition's body is flow content, so it is a container with block
 * children. Everything else is one rule for both: each carries the label
 * exactly as the source spells it, delimiters excluded, character escapes and
 * character references unresolved, whitespace uncollapsed, case unfolded; each
 * exists whether or not anything refers to it; each keeps the outcome of
 * matching off the node, in the reference map; and neither is ever moved,
 * reordered, renumbered, dropped, or given a back-reference by anything that
 * runs after the parse. This is mdast's model, adopted deliberately in
 * preference to cmark-gfm's, which erases a link reference definition into a
 * parser-private map and leaves no node behind.
 *
 * BOXED, and the reason is measured rather than stylistic: `markdown_core_code`
 * is 48 bytes, which is the widest arm `node.as` has, and this is 72. Storing
 * it inline would grow EVERY node in the document by 24 bytes to carry a
 * payload that appears once per definition. A reference is 40 and fits.
 *
 * ~~40~~ and ~~64~~ were true until Step 14: an optional chunk is 24 bytes
 * where a chunk is 16, so `code.info` took the widest arm 40 -> 48 and a node
 * 168 -> 176. Both numbers are re-measured, and section 4.14.14 states what
 * that cost on the benchmark. */
typedef struct {
    markdown_core_association association;
    /* REQUIRED (Q7, Q26): a definition that could not build a destination is
     * not emitted, so an empty one here means the source wrote `<>`. */
    markdown_core_chunk url;
    /* OPTIONAL (requirement 14). */
    markdown_core_optional_chunk title;
} markdown_core_definition;

enum markdown_core_node__internal_flags {
    MARKDOWN_CORE_NODE__OPEN = (1 << 0),
    MARKDOWN_CORE_NODE__LAST_LINE_BLANK = (1 << 1),
    MARKDOWN_CORE_NODE__LAST_LINE_CHECKED = (1 << 2),
    /* What `link` means on this node (docs/STREAMING.md T9). Neither set and
     * `link.holder` non-NULL: BORROWED, the node aliases the holder's list.
     * CACHE_OWNER: a CST block whose `link.holder` keeps its last projection.
     * ORIGIN: a derived block mid-projection whose `link.origin` is the CST
     * block it was cloned from; never set on a node a caller can hold. */
    MARKDOWN_CORE_NODE__CACHE_OWNER = (1 << 3),
    MARKDOWN_CORE_NODE__ORIGIN = (1 << 4),

    // The first bit an extension may claim. Extension flags are compile-time
    // constants owned by the extension that uses them; there is no runtime
    // registration and no allocator to run out of bits.
    MARKDOWN_CORE_NODE__EXTENSION_FIRST = (1 << 5),
};

typedef uint16_t markdown_core_node_internal_flags;

typedef struct markdown_core_holder markdown_core_holder;

struct markdown_core_node {
    markdown_core_strbuf content;

    struct markdown_core_node *next;
    struct markdown_core_node *prev;
    struct markdown_core_node *parent;
    struct markdown_core_node *first_child;
    struct markdown_core_node *last_child;

    void *user_data;
    markdown_core_free_func user_data_free_func;

    int start_line;
    int start_column;
    int end_line;
    int end_column;
    int internal_offset;
    /* This block's run in parser->line_marks -- the content-to-source map.
     * `content_mark_count == 0` means the block took no lines, which is the
     * state of every node that is not a block that accumulates content. */
    int content_mark;
    int content_mark_count;
    /* THE WRITE STAMP (docs/STREAMING.md T3): the parser's write clock as it
     * stood the last time the block phase wrote this block -- content
     * appended, closed, retyped, born. Half of the projection cache's key
     * (T9): two readings that agree say the block's own bytes and shape are
     * what they were. A CLOCK rather than a per-block count, so a block born
     * at an address another block died at can never read as unchanged. Only
     * the CST is stamped; a derived node carries its origin's reading. */
    uint32_t stamp;
    /* THE NODE'S IDENTITY (docs/STREAMING.md D4, T2), in two scopes that
     * together cover every collection a consumer can iterate. A BLOCK's
     * identity is minted once when the block phase opens it, carried onto
     * every derived node by the clone, and never reused within a parse: it is
     * document-unique. An INLINE's identity is its pre-order ordinal among
     * the owning block's inline descendants, assigned by the projection at
     * the end of the block's tail: it is unique within the block, so an
     * inline's full identity is the PAIR (owning block's identity, this
     * ordinal). Zero is "no identity" -- a lost mint or carry -- so a loss
     * fails closed (F11). The identity names the element a consumer is
     * tracking across feeds, not the node's kind: a retype keeps it, and
     * when a block splits or dies the fragment that continues what the
     * consumer already renders inherits it (§4 D4). */
    uint32_t identifier;
    uint16_t type;
    markdown_core_node_internal_flags flags;

    const markdown_core_syntax_extension *extension;

    /* ONE SLOT, and `flags` says what it holds (T19, T9).
     *
     * BORROWED -- neither flag, `holder` non-NULL: `first_child`..`last_child`
     * are the holder's list, aliased here, not this node's own. The free walk
     * detaches them and releases one hold instead of freeing them. Every node
     * in an aliased list has `parent == NULL`: a list can be aliased by
     * several borrowers at once -- the tree a consumer still holds and the
     * one the next feed returned -- so it can carry no single parent, and the
     * iterator climbs out of it to the borrower it entered through. A
     * borrower READS the list and never writes it; an insert beside a shared
     * node fails closed, having no parent to insert under.
     *
     * CACHE_OWNER -- a CST block: `holder` keeps the block's last projection,
     * keyed by the stamp and generations the holder records. The children are
     * the block's own. The hold is released with the block.
     *
     * ORIGIN -- a derived block between its clone and the end of its tail:
     * `origin` is the CST block it was cloned from, so the tail can store
     * what it projected. The free walk ignores the slot; the tail turns it
     * into BORROWED (a store) or clears it. */
    union {
        markdown_core_holder *holder;
        struct markdown_core_node *origin;
    } link;

    union {
        markdown_core_chunk literal;
        markdown_core_list list;
        markdown_core_code code;
        markdown_core_heading heading;
        markdown_core_link link;
        markdown_core_definition *definition;
        markdown_core_association association;
        markdown_core_reference_link reference;
        markdown_core_footnote_reference footnote_reference;
        int html_block_type;
        int cell_index; // For keeping track of TABLE_CELL table alignments
        void *opaque;
    } as;
};

/* A HOLDER owns a child list that borrowers alias (docs/STREAMING.md T19).
 * One count per LIST, not one per node: `refs` is the number of holds,
 * `release` drops one and destroys the list with the last. A fresh holder has
 * no hold, so its first `release` destroys it -- the rule an ordinary node
 * already lives under. The list is one level deep: a node in it is never
 * itself a borrower, which is what lets the iterator remember one borrower
 * rather than a stack of them. */
struct markdown_core_holder {
    markdown_core_mem *mem;
    markdown_core_node *first_child;
    markdown_core_node *last_child;
    uint32_t refs;
    /* THE KEY the list was projected under (T9): the origin's write stamp
     * (T3), both map generations (T4), and the extension set's generation. A
     * reading that agrees on all four says the list is what projecting the
     * block now would produce. */
    uint32_t stamp;
    size_t refgen;
    size_t footgen;
    size_t extgen;
};

/* Does this node alias a holder's list? The one question every walk asks. */
static MARKDOWN_CORE_INLINE bool MARKDOWN_CORE_NODE_BORROWED_P(const markdown_core_node *node) {
    return node->link.holder != NULL &&
           (node->flags & (MARKDOWN_CORE_NODE__CACHE_OWNER | MARKDOWN_CORE_NODE__ORIGIN)) == 0;
}

markdown_core_holder *markdown_core_holder_new(markdown_core_mem *mem);
void markdown_core_holder_hold(markdown_core_holder *holder);
void markdown_core_holder_release(markdown_core_holder *holder);
/* Move `block`'s OWN children into the holder's empty list. `block` is left
 * childless and the children parentless. The list is made SELF-CONTAINED on
 * the way in: an inline chunk that borrowed the block's content buffer would
 * dangle once the block is freed under a holder that outlives it -- F12
 * counted 7.6% of inline chunks borrowed. Returns 0 when a copy could not be
 * allocated; the chunk is then emptied rather than left borrowing. */
int markdown_core_holder_take_children(markdown_core_holder *holder, markdown_core_node *block);
/* Alias the holder's list under a childless `block`, which takes one hold. */
void markdown_core_node_borrow_children(markdown_core_node *block, markdown_core_holder *holder);

static MARKDOWN_CORE_INLINE markdown_core_mem *markdown_core_node_mem(markdown_core_node *node) {
    return node->content.mem;
}
MARKDOWN_CORE_EXPORT int markdown_core_node_check(markdown_core_node *node, FILE *out);

static MARKDOWN_CORE_INLINE bool MARKDOWN_CORE_NODE_TYPE_BLOCK_P(markdown_core_node_type node_type) {
    return (node_type & MARKDOWN_CORE_NODE_TYPE_MASK) == MARKDOWN_CORE_NODE_TYPE_BLOCK;
}

static MARKDOWN_CORE_INLINE bool MARKDOWN_CORE_NODE_BLOCK_P(markdown_core_node *node) {
    return node != NULL && MARKDOWN_CORE_NODE_TYPE_BLOCK_P((markdown_core_node_type)node->type);
}

static MARKDOWN_CORE_INLINE bool MARKDOWN_CORE_NODE_TYPE_INLINE_P(markdown_core_node_type node_type) {
    return (node_type & MARKDOWN_CORE_NODE_TYPE_MASK) == MARKDOWN_CORE_NODE_TYPE_INLINE;
}

static MARKDOWN_CORE_INLINE bool MARKDOWN_CORE_NODE_INLINE_P(markdown_core_node *node) {
    return node != NULL && MARKDOWN_CORE_NODE_TYPE_INLINE_P((markdown_core_node_type)node->type);
}

MARKDOWN_CORE_EXPORT bool markdown_core_node_can_contain_type(
    markdown_core_node *node,
    markdown_core_node_type child_type
);

#ifdef __cplusplus
}
#endif

#endif
