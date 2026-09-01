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
    /* Set by a block's own inline parse when it held a candidate the map
     * COULD answer -- a reference-form label in range of the cap, a footnote
     * call -- whether or not the map answered, or was even non-empty: an
     * insert can change only a projection that had something to ask (#163).
     * Recorded on the derived block as it parses, copied onto the holder at
     * the store, and the reason `S_cache_fresh` may ignore a map's
     * generation for a block that never consulted it. */
    MARKDOWN_CORE_NODE__CONSULTED_REFMAP = (1 << 5),
    MARKDOWN_CORE_NODE__CONSULTED_FOOTNOTES = (1 << 6),
    /* The node's MEMORY is a derivation arena's (#161): the free walk still
     * releases everything the node holds -- frozen content, holder holds,
     * chunk retains, the boxed definition -- but hands the 192 bytes back to
     * the arena (`markdown_core_node_arena_forget`) instead of the general
     * allocator, and the arena's last node out drops the pages. */
    MARKDOWN_CORE_NODE__ARENA = (1 << 7),
    /* The node's CHILDREN ARE A VECTOR (`children.vec/count`), not an
     * intrusive list (#161, D9). Set by the derivation on every skeleton
     * parent it gives children to, because siblinghood is the PARENT's fact:
     * an intrusive `next` writes a per-tree answer into the child, which is
     * exactly what forbids sharing the child (F23), while a vector keeps
     * every per-tree fact in per-tree memory. The CST and every inline list
     * stay intrusive -- an inline list is shared WHOLE behind a holder, so
     * its internal links never lie. */
    MARKDOWN_CORE_NODE__CHILD_ARRAY = (1 << 8),
    /* The node IS the holder's retained projection (#161, D9): one physical
     * node handed into every tree that hits, under one holder hold per
     * tree. Parentless and linkless -- every per-tree fact lives in the
     * referencing tree's vector -- never entered by a projection's walk,
     * never queued for a tail (its tail ran once, before the store), never
     * written. Freed by the holder alone, when the last hold goes. */
    MARKDOWN_CORE_NODE__SHARED = (1 << 9),

    // The first bit an extension may claim. Extension flags are compile-time
    // constants owned by the extension that uses them; there is no runtime
    // registration and no allocator to run out of bits.
    MARKDOWN_CORE_NODE__EXTENSION_FIRST = (1 << 10),
};

typedef uint16_t markdown_core_node_internal_flags;

typedef struct markdown_core_holder markdown_core_holder;
typedef struct markdown_core_node_arena markdown_core_node_arena;

struct markdown_core_node {
    markdown_core_strbuf content;
    /* THE FROZEN CONTENT (#153). NULL while `content` is the block's own
     * mutable accumulator -- which it stays for the whole life of a block
     * that is never shared, so a finish-only parse allocates no headers and
     * touches no counts. Set at FIRST SHARE: the first derivation that
     * clones the closed block freezes the strbuf's allocation into a
     * reference-counted immutable buffer (blocks.c, S_clone_block_node) and
     * repoints `content.ptr/size` at the same bytes with `asize == 0` --
     * readers keep working verbatim, and nothing moved, so every view taken
     * before the freeze stays valid. A derived block retains its origin's
     * buffer instead of copying the bytes; inline literals hold retained
     * slices of it (chunk.h), so the bytes outlive any one tree. When set,
     * the node's free path releases this and must not free `content.ptr`. */
    markdown_core_buf *frozen_content;

    struct markdown_core_node *next;
    struct markdown_core_node *prev;
    struct markdown_core_node *parent;
    /* TWO CHILD SHAPES, and `MARKDOWN_CORE_NODE__CHILD_ARRAY` says which
     * (#161, D9). Intrusive `first_child..last_child` for the CST, for
     * every inline list, and for a leaf's parsed content; `children` -- a
     * vector of child pointers -- for a derived skeleton parent, where the
     * per-tree sibling order must live in per-tree memory so a closed
     * child's NODE can be shared between trees. The vector's memory follows
     * the node's own: an arena node's vector is arena-bumped and dies with
     * the pages, a malloc'd node's is malloc'd and freed with it. */
    /* The anonymous member is C11 (6.7.2.1p13); MSVC accepts it in its C
     * mode but files it under C4201, which /WX promotes. */
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4201)
#endif
    union {
        struct {
            struct markdown_core_node *first_child;
            struct markdown_core_node *last_child;
        };
        struct {
            struct markdown_core_node **vec;
            size_t count;
        } children;
    };
#ifdef _MSC_VER
#pragma warning(pop)
#endif

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
    /* THE OTHER HALF OF AN INLINE'S PAIR: the owning block's identity,
     * stamped by the same numbering pass that assigns the ordinal above --
     * the one moment anything stands inside the block and beside the inline
     * at once. MEANINGFUL ONLY ON INLINE-CLASS NODES: a block IS its own
     * owner and reads nothing here, which is what spares every mint, retype
     * and identity handoff from maintaining a second field. Zero is "no
     * owner" -- an inline that never passed a tail -- and fails closed with
     * the ordinal it accompanies. */
    uint32_t owner;
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
 * One count per LIST, not one per node: `refs` is the number of holds, a
 * holder is BORN WITH ITS CREATOR'S HOLD (#153: the same create-at-one rule
 * as markdown_core_buf), and `release` drops one and destroys the list with
 * the last. The count is C11-atomic, and that is the THREADING CONTRACT for
 * everything a derivation shares: a derived document may be freed on any
 * thread, concurrently with the session that produced it and with other
 * documents derived from it -- the atomic counts on holders and frozen
 * buffers are the synchronization. Access to ONE document is still the
 * caller's to order (include/markdown_core.h states the facade form of the
 * same rule). The list is one level deep: a node in it is never itself a
 * borrower, which is what lets the iterator remember one borrower rather
 * than a stack of them. */
struct markdown_core_holder {
    markdown_core_mem *mem;
    markdown_core_node *first_child;
    markdown_core_node *last_child;
    markdown_core_atomic_u32 refs;
    /* THE KEY the list was projected under (T9): the origin's write stamp
     * (T3), both map generations (T4), and the extension set's generation. A
     * reading that agrees on all four says the list is what projecting the
     * block now would produce. */
    uint32_t stamp;
    size_t refgen;
    size_t footgen;
    size_t extgen;
    /* The CONSULTED bits of the block the list was projected from
     * (MARKDOWN_CORE_NODE__CONSULTED_*): a map's generation takes part in
     * the key only when the stored projection had something to ask that map
     * (#163) -- a definition arriving cannot change a block that consulted
     * neither, so its hit survives the bump. */
    markdown_core_node_internal_flags consulted;
    /* THE RETAINED PROJECTION (#161, D9): the derived block node the list
     * was projected into, malloc-shelled so it outlives any one tree's
     * arena, flagged SHARED, aliasing this holder's list as its children.
     * A hit hands THIS node into the requesting tree's vector under one
     * hold; the holder frees it with the list when the last hold goes.
     * NULL only on a holder that has not stored since the field landed --
     * every store sets it. */
    markdown_core_node *node;
};

/* Does this node alias a holder's list? The one question every walk asks. */
static MARKDOWN_CORE_INLINE bool MARKDOWN_CORE_NODE_BORROWED_P(const markdown_core_node *node) {
    return node->link.holder != NULL &&
           (node->flags & (MARKDOWN_CORE_NODE__CACHE_OWNER | MARKDOWN_CORE_NODE__ORIGIN)) == 0;
}

static MARKDOWN_CORE_INLINE bool MARKDOWN_CORE_NODE_ARRAY_P(const markdown_core_node *node) {
    return (node->flags & MARKDOWN_CORE_NODE__CHILD_ARRAY) != 0;
}

/* THE CHILD CURSOR: one loop shape over both child representations. `index`
 * is the vector cursor and is meaningless on an intrusive parent; the
 * CALLER owns knowing the parent across the loop, which is what lets an
 * intrusive step stay the child's own `next`. Every walk over children that
 * can be a derived container's goes through these two; a walk that knows
 * its list is intrusive -- an inline list, the CST -- may keep reading
 * `first_child`/`next` directly. */
typedef struct {
    size_t index;
} markdown_core_child_cursor;

static MARKDOWN_CORE_INLINE markdown_core_node *markdown_core_child_first(
    const markdown_core_node *parent,
    markdown_core_child_cursor *cursor
) {
    cursor->index = 0;
    if (MARKDOWN_CORE_NODE_ARRAY_P(parent)) {
        return parent->children.count ? parent->children.vec[0] : NULL;
    }
    return parent->first_child;
}

static MARKDOWN_CORE_INLINE markdown_core_node *markdown_core_child_after(
    const markdown_core_node *parent,
    const markdown_core_node *child,
    markdown_core_child_cursor *cursor
) {
    if (MARKDOWN_CORE_NODE_ARRAY_P(parent)) {
        cursor->index++;
        return cursor->index < parent->children.count ? parent->children.vec[cursor->index] : NULL;
    }
    return child->next;
}

static MARKDOWN_CORE_INLINE markdown_core_node *markdown_core_child_back(const markdown_core_node *parent) {
    if (MARKDOWN_CORE_NODE_ARRAY_P(parent)) {
        return parent->children.count ? parent->children.vec[parent->children.count - 1] : NULL;
    }
    return parent->last_child;
}

/* THE DERIVATION ARENA (#161). A feed's derived skeleton is one node per CST
 * block, all born in `derive_tree` and almost all dying with the tree -- a
 * lifetime the general allocator re-proves node by node, at 25-39% of a
 * hit-dominated feed. The arena states it once: `derive_tree` sizes one page
 * off the parser's block mint, the clone bumps nodes out of it (zeroed, like
 * the calloc they replace, flag `ARENA`), and the pages go back in one
 * motion when the last node is forgotten.
 *
 * The tree stays SELF-CONTAINED: no root carries a handle -- a root is a
 * node like any other, free to be borrowed against, replaced by a hook, or
 * freed from inside a projection -- and no node carries a pointer, because
 * pages are 64 KiB-ALIGNED and never outgrow their alignment window:
 * masking a node's address recovers its page header, and the header names
 * the arena (`forget`). Nothing else changes shape -- `content.mem` stays
 * the parser's own mem, so no buffer, iterator, or chunk a projection
 * builds can capture an address that dies with the arena. (The first build
 * hung the handle on the returned root's link slot, which a borrow
 * clobbers; the second hid the arena behind a mem wrapper in `content.mem`,
 * which a frozen open block's buffer carried into a cache holder that
 * outlived it. The masking has no address to lose and nothing to clobber.)
 * The arena keeps a live count, born at ONE for the deriving call's own
 * hold (#153's create-at-one rule) and one more per node handed out;
 * `release` drops the creator's, `forget` a node's, and whoever reaches
 * zero frees the pages. The count is NOT atomic, deliberately: an arena
 * belongs to ONE derived document, access to one document is the caller's
 * to order (include/markdown_core.h), and the creator's hold is gone
 * before the caller ever sees the tree.
 *
 * Only skeleton nodes live here; everything a projection allocates that can
 * OUTLIVE the tree -- an inline list moved into a cache holder, a hook's
 * replacement node -- keeps the general allocator, so the arena never owns
 * a byte another tree can reach. Under AddressSanitizer the unhanded
 * remainder of a page and every forgotten node are poisoned, so the
 * checking a per-node free() gave up is kept. */
markdown_core_node_arena *markdown_core_node_arena_new(markdown_core_mem *mem, size_t node_hint);
markdown_core_node *markdown_core_node_arena_calloc(markdown_core_node_arena *arena);
void *markdown_core_node_arena_bytes(markdown_core_node_arena *arena, size_t size);
markdown_core_node_arena *markdown_core_node_arena_of(markdown_core_node *node);
void markdown_core_node_arena_release(markdown_core_node_arena *arena);
void markdown_core_node_arena_forget(markdown_core_node *node);

markdown_core_holder *markdown_core_holder_new(markdown_core_mem *mem);
void markdown_core_holder_hold(markdown_core_holder *holder);
void markdown_core_holder_release(markdown_core_holder *holder);
/* Move `block`'s OWN children into the holder's empty list. `block` is left
 * childless and the children parentless. The list is made SELF-CONTAINED on
 * the way in: an inline chunk that borrowed the block's content buffer would
 * dangle once the block is freed under a holder that outlives it -- F12
 * counted 7.6% of inline chunks borrowed. Returns 0 when a copy could not be
 * allocated; the chunk is then emptied rather than left borrowing. */
void markdown_core_holder_take_children(markdown_core_holder *holder, markdown_core_node *block);
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
