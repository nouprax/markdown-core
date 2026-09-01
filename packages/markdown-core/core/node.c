#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "node.h"
#include "iterator.h"
#include "references.h"
#include "syntax_extension.h"

static void S_node_unlink(markdown_core_node *node);

#define NODE_MEM(node) markdown_core_node_mem(node)

/* The arena keeps ASan's checking (#161): a page's unhanded remainder and
 * every forgotten node are poisoned, so a read past the bump or a use after
 * node_free reports exactly as it did when every node was its own
 * allocation. */
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MARKDOWN_CORE_ARENA_POISON 1
#endif
#elif defined(__SANITIZE_ADDRESS__)
#define MARKDOWN_CORE_ARENA_POISON 1
#endif
#ifdef MARKDOWN_CORE_ARENA_POISON
#include <sanitizer/asan_interface.h>
#define S_arena_poison(ptr, len) __asan_poison_memory_region((ptr), (len))
#define S_arena_unpoison(ptr, len) __asan_unpoison_memory_region((ptr), (len))
#else
#define S_arena_poison(ptr, len) ((void)0)
#define S_arena_unpoison(ptr, len) ((void)0)
#endif

/* THE PAGE IS FOUND BY MASKING (node.h): every page lives inside one
 * 64 KiB-aligned window and never outgrows it, so `forget` recovers the
 * page header -- and the arena behind it -- from a node's address alone,
 * and nothing else in the system carries arena identity. The allocation is
 * over-sized by the alignment and the aligned window placed inside it
 * (`raw` keeps what free() needs); the slop is never written, so it costs
 * address space, not resident memory. */
#define MARKDOWN_CORE_ARENA_ALIGN ((uintptr_t)65536)

typedef struct markdown_core_node_arena_page {
    markdown_core_node_arena *arena;
    void *raw;
    struct markdown_core_node_arena_page *next;
    size_t used;
    size_t cap;
    markdown_core_node nodes[];
} markdown_core_node_arena_page;

/* Byte pages carry the derivation's VECTORS (#161, D9): no alignment and no
 * masking, because a vector is never handed back one by one -- it dies with
 * the pages -- and an oversize request simply sizes its own page. */
typedef struct markdown_core_node_arena_bpage {
    struct markdown_core_node_arena_bpage *next;
    size_t used;
    size_t cap;
    unsigned char bytes[];
} markdown_core_node_arena_bpage;

struct markdown_core_node_arena {
    markdown_core_mem *mem;
    markdown_core_node_arena_page *pages;
    markdown_core_node_arena_bpage *bpages;
    size_t live;
};

#define MARKDOWN_CORE_ARENA_BPAGE_BYTES ((size_t)32768)

#define MARKDOWN_CORE_ARENA_MAX_NODES                                                                                  \
    (((size_t)MARKDOWN_CORE_ARENA_ALIGN - sizeof(markdown_core_node_arena_page)) / sizeof(markdown_core_node))
#define MARKDOWN_CORE_ARENA_MIN_NODES 8

static markdown_core_node_arena_page *S_arena_page_new(markdown_core_node_arena *arena, size_t cap) {
    markdown_core_node_arena_page *page;
    void *raw;
    uintptr_t base;
    if (cap > MARKDOWN_CORE_ARENA_MAX_NODES) {
        cap = MARKDOWN_CORE_ARENA_MAX_NODES;
    }
    /* realloc(NULL), not calloc: calloc would WRITE every byte of the
     * window and the slop -- 43% of a feed went to that memset when this
     * was measured -- while the nodes are zeroed one by one as they are
     * handed out, and the header's five fields are all assigned below. */
    raw = arena->mem->realloc(
        NULL,
        sizeof(markdown_core_node_arena_page) + cap * sizeof(markdown_core_node) + MARKDOWN_CORE_ARENA_ALIGN - 1
    );
    if (!raw) {
        return NULL;
    }
    base = ((uintptr_t)raw + MARKDOWN_CORE_ARENA_ALIGN - 1) & ~(MARKDOWN_CORE_ARENA_ALIGN - 1);
    page = (markdown_core_node_arena_page *)base;
    page->arena = arena;
    page->raw = raw;
    page->next = arena->pages;
    page->used = 0;
    page->cap = cap;
    arena->pages = page;
    S_arena_poison(page->nodes, cap * sizeof(markdown_core_node));
    return page;
}

markdown_core_node_arena *markdown_core_node_arena_new(markdown_core_mem *mem, size_t node_hint) {
    markdown_core_node_arena *arena = (markdown_core_node_arena *)mem->calloc(1, sizeof(*arena));
    if (!arena) {
        return NULL;
    }
    arena->mem = mem;
    /* Born at one: the creator's hold (#153), released by `derive_tree` once
     * the nodes carry their own. */
    arena->live = 1;
    if (!S_arena_page_new(
            arena,
            node_hint < MARKDOWN_CORE_ARENA_MIN_NODES ? MARKDOWN_CORE_ARENA_MIN_NODES : node_hint
        )) {
        mem->free(arena);
        return NULL;
    }
    return arena;
}

/* Zeroed, exactly like the calloc it replaces -- per node, at hand-out. */
markdown_core_node *markdown_core_node_arena_calloc(markdown_core_node_arena *arena) {
    markdown_core_node_arena_page *page = arena->pages;
    markdown_core_node *node;
    if (page->used == page->cap) {
        page = S_arena_page_new(arena, MARKDOWN_CORE_ARENA_MAX_NODES);
        if (!page) {
            return NULL;
        }
    }
    node = &page->nodes[page->used++];
    S_arena_unpoison(node, sizeof(*node));
    memset(node, 0, sizeof(*node));
    arena->live++;
    return node;
}

static void S_arena_drop(markdown_core_node_arena *arena) {
    markdown_core_mem *mem = arena->mem;
    markdown_core_node_arena_page *page = arena->pages;
    markdown_core_node_arena_bpage *bpage = arena->bpages;
    while (page) {
        markdown_core_node_arena_page *next = page->next;
        void *raw = page->raw;
        S_arena_unpoison(page->nodes, page->cap * sizeof(markdown_core_node));
        mem->free(raw);
        page = next;
    }
    while (bpage) {
        markdown_core_node_arena_bpage *next = bpage->next;
        mem->free(bpage);
        bpage = next;
    }
    mem->free(arena);
}

/* Bump `size` bytes with the vectors' lifetime -- the arena's own. 16-aligned;
 * NULL when the allocator refuses a page. */
void *markdown_core_node_arena_bytes(markdown_core_node_arena *arena, size_t size) {
    markdown_core_node_arena_bpage *bpage = arena->bpages;
    void *out;
    size = (size + 15) & ~(size_t)15;
    if (!bpage || bpage->cap - bpage->used < size) {
        size_t cap = size > MARKDOWN_CORE_ARENA_BPAGE_BYTES ? size : MARKDOWN_CORE_ARENA_BPAGE_BYTES;
        bpage = (markdown_core_node_arena_bpage *)arena->mem->realloc(NULL, sizeof(*bpage) + cap);
        if (!bpage) {
            return NULL;
        }
        bpage->next = arena->bpages;
        bpage->used = 0;
        bpage->cap = cap;
        arena->bpages = bpage;
    }
    out = bpage->bytes + bpage->used;
    bpage->used += size;
    return out;
}

/* The arena an ARENA-flagged node lives in, by the same masking `forget`
 * uses: for the one caller (vector growth under a hook) that must allocate
 * beside a node it did not derive. */
markdown_core_node_arena *markdown_core_node_arena_of(markdown_core_node *node) {
    markdown_core_node_arena_page *page =
        (markdown_core_node_arena_page *)((uintptr_t)node & ~(MARKDOWN_CORE_ARENA_ALIGN - 1));
    return page->arena;
}

void markdown_core_node_arena_release(markdown_core_node_arena *arena) {
    if (--arena->live == 0) {
        S_arena_drop(arena);
    }
}

/* Hand one node back. Safe to be the drop that frees the pages mid-walk: a
 * walk's saved `next` can only point at a node it has not visited, and an
 * unvisited arena node means the count is still positive. */
void markdown_core_node_arena_forget(markdown_core_node *node) {
    markdown_core_node_arena_page *page =
        (markdown_core_node_arena_page *)((uintptr_t)node & ~(MARKDOWN_CORE_ARENA_ALIGN - 1));
    markdown_core_node_arena *arena = page->arena;
    S_arena_poison(node, sizeof(*node));
    if (--arena->live == 0) {
        S_arena_drop(arena);
    }
}

bool markdown_core_node_can_contain_type(markdown_core_node *node, markdown_core_node_type child_type) {
    if (child_type == MARKDOWN_CORE_NODE_DOCUMENT) {
        return false;
    }

    if (node->extension && node->extension->can_contain_func) {
        return node->extension->can_contain_func(node->extension, node, child_type) != 0;
    }

    switch (node->type) {
    case MARKDOWN_CORE_NODE_DOCUMENT:
    case MARKDOWN_CORE_NODE_BLOCK_QUOTE:
    case MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION:
    case MARKDOWN_CORE_NODE_LIST_ITEM:
        return MARKDOWN_CORE_NODE_TYPE_BLOCK_P(child_type) && child_type != MARKDOWN_CORE_NODE_LIST_ITEM;

    case MARKDOWN_CORE_NODE_LIST:
        return child_type == MARKDOWN_CORE_NODE_LIST_ITEM;

    case MARKDOWN_CORE_NODE_PARAGRAPH:
    case MARKDOWN_CORE_NODE_HEADING:
    case MARKDOWN_CORE_NODE_EMPHASIS:
    case MARKDOWN_CORE_NODE_STRONG:
    case MARKDOWN_CORE_NODE_LINK:
    case MARKDOWN_CORE_NODE_IMAGE:
    case MARKDOWN_CORE_NODE_LINK_REFERENCE:
    case MARKDOWN_CORE_NODE_IMAGE_REFERENCE:
        return MARKDOWN_CORE_NODE_TYPE_INLINE_P(child_type);

    default:
        break;
    }

    return false;
}

static bool S_can_contain(markdown_core_node *node, markdown_core_node *child) {
    if (node == NULL || child == NULL) {
        return false;
    }
    if (NODE_MEM(node) != NODE_MEM(child)) {
        return 0;
    }

    /* `child` must not be `node` and must not be one of its ancestors.
     *
     * This used to sit behind `markdown_core_enable_safety_checks`, a
     * process-global flag that defaulted to OFF and that only the test suite
     * ever set -- so the shipped library answered `append_child(q, q)` with
     * SUCCESS and left `q->parent == q`, and a two-node cycle took two calls.
     * Measured before it was made unconditional, with the flag in its shipped
     * position:
     *
     *     append_child(q, q)   returned 1, parent == self
     *     prepend_child(r, r)  returned 1, parent == self
     *     append_child(a, b) then append_child(b, a)  ->  a->parent == b
     *
     * A library that makes a cycle on request while its own tests deny it is
     * not testing the library. The walk is O(depth) per link and the parse's
     * depth is the document's nesting; §4.14.3b has the cost. */
    {
        markdown_core_node *cur = node;
        do {
            if (cur == child) {
                return false;
            }
            cur = cur->parent;
        } while (cur != NULL);
    }

    return markdown_core_node_can_contain_type(node, (markdown_core_node_type)child->type);
}

markdown_core_node *markdown_core_node_new_with_mem_and_ext(
    markdown_core_node_type type,
    markdown_core_mem *mem,
    const markdown_core_syntax_extension *extension
) {
    markdown_core_node *node = (markdown_core_node *)mem->calloc(1, sizeof(*node));
    if (!node) {
        return NULL;
    }
    markdown_core_strbuf_init(mem, &node->content, 0);
    node->type = (uint16_t)type;
    node->extension = extension;

    switch (node->type) {
    case MARKDOWN_CORE_NODE_HEADING:
        node->as.heading.level = 1;
        break;

    case MARKDOWN_CORE_NODE_LIST: {
        markdown_core_list *list = &node->as.list;
        list->list_type = MARKDOWN_CORE_BULLET_LIST;
        list->start = 0;
        list->tight = false;
        break;
    }

    default:
        break;
    }

    if (node->extension && node->extension->opaque_alloc_func) {
        node->extension->opaque_alloc_func(node->extension, mem, node);
    }

    return node;
}

markdown_core_node *markdown_core_node_new_with_ext(
    markdown_core_node_type type,
    const markdown_core_syntax_extension *extension
) {
    return markdown_core_node_new_with_mem_and_ext(type, markdown_core_get_default_mem_allocator(), extension);
}

markdown_core_node *markdown_core_node_new_with_mem(markdown_core_node_type type, markdown_core_mem *mem) {
    return markdown_core_node_new_with_mem_and_ext(type, mem, NULL);
}

markdown_core_node *markdown_core_node_new(markdown_core_node_type type) {
    return markdown_core_node_new_with_ext(type, NULL);
}

static void free_node_as(markdown_core_node *node) {
    switch (node->type) {
    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        markdown_core_optional_chunk_free(NODE_MEM(node), &node->as.code.info);
        markdown_core_chunk_free(NODE_MEM(node), &node->as.code.literal);
        break;
    case MARKDOWN_CORE_NODE_TEXT:
    case MARKDOWN_CORE_NODE_HTML:
    case MARKDOWN_CORE_NODE_CODE:
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
        markdown_core_chunk_free(NODE_MEM(node), &node->as.literal);
        break;
    case MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION:
        markdown_core_association_free(NODE_MEM(node), &node->as.association);
        break;
    case MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE:
        markdown_core_association_free(NODE_MEM(node), &node->as.footnote_reference.association);
        break;
    case MARKDOWN_CORE_NODE_LINK_REFERENCE:
    case MARKDOWN_CORE_NODE_IMAGE_REFERENCE:
        markdown_core_association_free(NODE_MEM(node), &node->as.reference.association);
        break;
    case MARKDOWN_CORE_NODE_LINK:
    case MARKDOWN_CORE_NODE_IMAGE:
        markdown_core_chunk_free(NODE_MEM(node), &node->as.link.url);
        markdown_core_optional_chunk_free(NODE_MEM(node), &node->as.link.title);
        break;
    case MARKDOWN_CORE_NODE_REFERENCE_DEFINITION:
        if (node->as.definition) {
            markdown_core_association_free(NODE_MEM(node), &node->as.definition->association);
            markdown_core_chunk_free(NODE_MEM(node), &node->as.definition->url);
            markdown_core_optional_chunk_free(NODE_MEM(node), &node->as.definition->title);
            NODE_MEM(node)->free(node->as.definition);
            node->as.definition = NULL;
        }
        break;
    default:
        break;
    }
}

// Free a markdown_core_node list and any children.
static void S_free_nodes(markdown_core_node *e) {
    markdown_core_node *next;
    while (e != NULL) {
        /* A BORROWED LIST IS DETACHED, NOT WALKED. The children hanging off
         * this block are the holder's, and the splice below would pull them
         * into this walk and free them under every other borrower. Cut them
         * loose, then release the one hold this block had. */
        if (e->link.holder && !(e->flags & MARKDOWN_CORE_NODE__ORIGIN)) {
            markdown_core_holder *holder = e->link.holder;
            if (!(e->flags & MARKDOWN_CORE_NODE__CACHE_OWNER)) {
                e->first_child = NULL;
                e->last_child = NULL;
            }
            e->link.holder = NULL;
            e->flags &= ~MARKDOWN_CORE_NODE__CACHE_OWNER;
            markdown_core_holder_release(holder);
        }

        if (e->frozen_content) {
            /* `content.ptr` aliases the frozen bytes; only the reference is
             * this node's to give up. */
            markdown_core_buf_release(e->frozen_content);
        } else {
            markdown_core_strbuf_free(&e->content);
        }

        if (e->as.opaque && e->extension && e->extension->opaque_free_func) {
            e->extension->opaque_free_func(e->extension, NODE_MEM(e), e);
        }

        free_node_as(e);

        if (MARKDOWN_CORE_NODE_ARRAY_P(e)) {
            /* A container's vector joins the walk the way an intrusive list
             * always has: the entries are chained through their own `next`
             * fields -- every entry here is this tree's to write -- and the
             * vector itself goes back to the allocator. The splice stays
             * allocation-free, which a free path must be. */
            size_t i;
            for (i = 0; i + 1 < e->children.count; i++) {
                e->children.vec[i]->next = e->children.vec[i + 1];
            }
            if (e->children.count) {
                e->children.vec[e->children.count - 1]->next = e->next;
                next = e->children.vec[0];
            } else {
                next = e->next;
            }
            if (e->children.vec && !(e->flags & MARKDOWN_CORE_NODE__ARENA)) {
                /* An arena shell's vector is arena memory; it goes with the
                 * pages, not through free(). */
                NODE_MEM(e)->free(e->children.vec);
            }
        } else {
            if (e->last_child) {
                // Splice children into list
                e->last_child->next = e->next;
                e->next = e->first_child;
            }
            next = e->next;
        }
        /* An arena node's memory is the arena's to take back (#161): the
         * releases above are done, and `forget` frees the pages once the
         * last node is handed in. */
        if (e->flags & MARKDOWN_CORE_NODE__ARENA) {
            markdown_core_node_arena_forget(e);
        } else {
            NODE_MEM(e)->free(e);
        }
        e = next;
    }
}

void markdown_core_node_free(markdown_core_node *node) {
    S_node_unlink(node);
    node->next = NULL;
    S_free_nodes(node);
}

markdown_core_holder *markdown_core_holder_new(markdown_core_mem *mem) {
    markdown_core_holder *holder = (markdown_core_holder *)mem->calloc(1, sizeof(*holder));
    if (holder) {
        holder->mem = mem;
        /* Born with the creator's hold, like a frozen buffer (#153). */
        markdown_core_atomic_init(&holder->refs, 1);
    }
    return holder;
}

void markdown_core_holder_hold(markdown_core_holder *holder) { markdown_core_atomic_increment(&holder->refs); }

void markdown_core_holder_release(markdown_core_holder *holder) {
    if (markdown_core_atomic_decrement(&holder->refs) != 0) {
        return;
    }
    /* The list's `next` chain is exactly what `S_free_nodes` walks; the
     * `parent` it never reads is NULL on every node here. */
    S_free_nodes(holder->first_child);
    holder->mem->free(holder);
}

/* Pure pointer moves: every chunk in the list already holds its bytes --
 * a retained slice of frozen content or a private allocation -- so the
 * store that used to copy (T19's own-at-the-boundary rule, via node_own)
 * now moves and cannot fail (#153). */
void markdown_core_holder_take_children(markdown_core_holder *holder, markdown_core_node *block) {
    markdown_core_node *child;
    for (child = block->first_child; child; child = child->next) {
        child->parent = NULL;
    }
    holder->first_child = block->first_child;
    holder->last_child = block->last_child;
    block->first_child = NULL;
    block->last_child = NULL;
}

void markdown_core_node_borrow_children(markdown_core_node *block, markdown_core_holder *holder) {
    block->first_child = holder->first_child;
    block->last_child = holder->last_child;
    block->link.holder = holder;
    block->flags &= ~(MARKDOWN_CORE_NODE__CACHE_OWNER | MARKDOWN_CORE_NODE__ORIGIN);
    markdown_core_holder_hold(holder);
}

markdown_core_node_type markdown_core_node_get_type(markdown_core_node *node) {
    if (node == NULL) {
        return MARKDOWN_CORE_NODE_NONE;
    } else {
        return (markdown_core_node_type)node->type;
    }
}

int markdown_core_node_set_type(markdown_core_node *node, markdown_core_node_type type) {
    markdown_core_node_type initial_type;

    if (type == node->type) {
        return 1;
    }

    initial_type = (markdown_core_node_type)node->type;
    node->type = (uint16_t)type;

    if (!S_can_contain(node->parent, node)) {
        node->type = (uint16_t)initial_type;
        return 0;
    }

    /* We rollback the type to free the union members appropriately */
    node->type = (uint16_t)initial_type;
    free_node_as(node);

    node->type = (uint16_t)type;

    return 1;
}

const char *markdown_core_node_get_type_string(markdown_core_node *node) {
    if (node == NULL) {
        return "NONE";
    }

    if (node->extension && node->extension->get_type_string_func) {
        return node->extension->get_type_string_func(node->extension, node);
    }

    switch (node->type) {
    case MARKDOWN_CORE_NODE_NONE:
        return "none";
    case MARKDOWN_CORE_NODE_DOCUMENT:
        return "document";
    case MARKDOWN_CORE_NODE_BLOCK_QUOTE:
        return "block_quote";
    case MARKDOWN_CORE_NODE_LIST:
        return "list";
    case MARKDOWN_CORE_NODE_LIST_ITEM:
        return "list_item";
    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        return "code_block";
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
        return "html_block";
    case MARKDOWN_CORE_NODE_PARAGRAPH:
        return "paragraph";
    case MARKDOWN_CORE_NODE_HEADING:
        return "heading";
    case MARKDOWN_CORE_NODE_THEMATIC_BREAK:
        return "thematic_break";
    /* Both definition kinds read `<unknown>` here until Step 9b, which is the
     * name the concrete record set printed for the owner of every footnote
     * definition's marker bytes. */
    case MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION:
        return "footnote_definition";
    case MARKDOWN_CORE_NODE_REFERENCE_DEFINITION:
        return "reference_definition";
    case MARKDOWN_CORE_NODE_LINK_REFERENCE:
        return "link_reference";
    case MARKDOWN_CORE_NODE_IMAGE_REFERENCE:
        return "image_reference";
    case MARKDOWN_CORE_NODE_TEXT:
        return "text";
    case MARKDOWN_CORE_NODE_SOFT_BREAK:
        return "soft_break";
    case MARKDOWN_CORE_NODE_LINE_BREAK:
        return "line_break";
    case MARKDOWN_CORE_NODE_CODE:
        return "code";
    case MARKDOWN_CORE_NODE_HTML:
        return "html";
    case MARKDOWN_CORE_NODE_EMPHASIS:
        return "emphasis";
    case MARKDOWN_CORE_NODE_STRONG:
        return "strong";
    case MARKDOWN_CORE_NODE_LINK:
        return "link";
    case MARKDOWN_CORE_NODE_IMAGE:
        return "image";
    case MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE:
        return "footnote_reference";
    }

    return "<unknown>";
}

markdown_core_node *markdown_core_node_next(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    } else {
        return node->next;
    }
}

markdown_core_node *markdown_core_node_previous(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    } else {
        return node->prev;
    }
}

markdown_core_node *markdown_core_node_parent(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    } else {
        return node->parent;
    }
}

markdown_core_node *markdown_core_node_first_child(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    } else {
        return node->first_child;
    }
}

markdown_core_node *markdown_core_node_last_child(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    } else {
        return node->last_child;
    }
}

const char *markdown_core_node_get_literal(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    }

    switch (node->type) {
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
    case MARKDOWN_CORE_NODE_TEXT:
    case MARKDOWN_CORE_NODE_HTML:
    case MARKDOWN_CORE_NODE_CODE:
        return markdown_core_chunk_to_cstr(NODE_MEM(node), &node->as.literal);

    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        return markdown_core_chunk_to_cstr(NODE_MEM(node), &node->as.code.literal);

    default:
        break;
    }

    return NULL;
}

int markdown_core_node_set_literal(markdown_core_node *node, const char *content) {
    if (node == NULL) {
        return 0;
    }

    switch (node->type) {
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
    case MARKDOWN_CORE_NODE_TEXT:
    case MARKDOWN_CORE_NODE_HTML:
    case MARKDOWN_CORE_NODE_CODE:
        return markdown_core_chunk_set_cstr(NODE_MEM(node), &node->as.literal, content);

    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        return markdown_core_chunk_set_cstr(NODE_MEM(node), &node->as.code.literal, content);

    default:
        break;
    }

    return 0;
}

const char *markdown_core_node_get_string_content(markdown_core_node *node) { return (char *)node->content.ptr; }

int markdown_core_node_set_string_content(markdown_core_node *node, const char *content) {
    /* THE THAW (#153): frozen content is shared and immutable, so a write
     * starts a fresh private arena and drops this node's reference --
     * sharers keep theirs, and the retained slices inline literals hold
     * stay valid through their own references. Without it the setter grew
     * a replacement arena while `frozen_content` still claimed the bytes:
     * the free path released only the buffer and leaked the replacement,
     * and node_check rejects the half-thawed shape. The strbuf is reset
     * BEFORE the release: the release may free the very bytes
     * `content.ptr` aliases. */
    if (node->frozen_content) {
        markdown_core_buf *frozen = node->frozen_content;
        node->frozen_content = NULL;
        markdown_core_strbuf_init(node->content.mem, &node->content, 0);
        markdown_core_buf_release(frozen);
    }
    markdown_core_strbuf_sets(&node->content, content);
    return true;
}

int markdown_core_node_get_heading_level(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }

    switch (node->type) {
    case MARKDOWN_CORE_NODE_HEADING:
        return node->as.heading.level;

    default:
        break;
    }

    return 0;
}

int markdown_core_node_set_heading_level(markdown_core_node *node, int level) {
    if (node == NULL || level < 1 || level > 6) {
        return 0;
    }

    switch (node->type) {
    case MARKDOWN_CORE_NODE_HEADING:
        node->as.heading.level = level;
        return 1;

    default:
        break;
    }

    return 0;
}

markdown_core_list_type markdown_core_node_get_list_type(markdown_core_node *node) {
    if (node == NULL) {
        return MARKDOWN_CORE_NO_LIST;
    }

    if (node->type == MARKDOWN_CORE_NODE_LIST) {
        return node->as.list.list_type;
    } else {
        return MARKDOWN_CORE_NO_LIST;
    }
}

int markdown_core_node_set_list_type(markdown_core_node *node, markdown_core_list_type type) {
    if (!(type == MARKDOWN_CORE_BULLET_LIST || type == MARKDOWN_CORE_ORDERED_LIST)) {
        return 0;
    }

    if (node == NULL) {
        return 0;
    }

    if (node->type == MARKDOWN_CORE_NODE_LIST) {
        node->as.list.list_type = type;
        return 1;
    } else {
        return 0;
    }
}

markdown_core_delim_type markdown_core_node_get_list_delim(markdown_core_node *node) {
    if (node == NULL) {
        return MARKDOWN_CORE_NO_DELIM;
    }

    if (node->type == MARKDOWN_CORE_NODE_LIST) {
        return node->as.list.delimiter;
    } else {
        return MARKDOWN_CORE_NO_DELIM;
    }
}

int markdown_core_node_set_list_delim(markdown_core_node *node, markdown_core_delim_type delim) {
    if (!(delim == MARKDOWN_CORE_PERIOD_DELIM || delim == MARKDOWN_CORE_PAREN_DELIM)) {
        return 0;
    }

    if (node == NULL) {
        return 0;
    }

    if (node->type == MARKDOWN_CORE_NODE_LIST) {
        node->as.list.delimiter = delim;
        return 1;
    } else {
        return 0;
    }
}

int markdown_core_node_get_list_start(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }

    if (node->type == MARKDOWN_CORE_NODE_LIST) {
        return node->as.list.start;
    } else {
        return 0;
    }
}

int markdown_core_node_set_list_start(markdown_core_node *node, int start) {
    if (node == NULL || start < 0) {
        return 0;
    }

    if (node->type == MARKDOWN_CORE_NODE_LIST) {
        node->as.list.start = start;
        return 1;
    } else {
        return 0;
    }
}

int markdown_core_node_get_list_tight(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }

    if (node->type == MARKDOWN_CORE_NODE_LIST) {
        return node->as.list.tight;
    } else {
        return 0;
    }
}

int markdown_core_node_set_list_tight(markdown_core_node *node, int tight) {
    if (node == NULL) {
        return 0;
    }

    if (node->type == MARKDOWN_CORE_NODE_LIST) {
        node->as.list.tight = tight == 1;
        return 1;
    } else {
        return 0;
    }
}

const char *markdown_core_node_get_fence_info(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    }

    if (node->type == MARKDOWN_CORE_NODE_CODE_BLOCK) {
        /* ABSENT IS NULL. `markdown_core_chunk_to_cstr` allocates a `""` for a
         * chunk with no data, which would answer "the source wrote an empty
         * info string" for a fence that wrote none (requirement 14). */
        if (!node->as.code.info.has_value) {
            return NULL;
        }
        return markdown_core_chunk_to_cstr(NODE_MEM(node), &node->as.code.info.value);
    } else {
        return NULL;
    }
}

int markdown_core_node_set_fence_info(markdown_core_node *node, const char *info) {
    if (node == NULL) {
        return 0;
    }

    if (node->type == MARKDOWN_CORE_NODE_CODE_BLOCK) {
        /* A NULL argument is ABSENCE and anything else is presence, including
         * `""`; the caller states which, and this is the write site. */
        if (!markdown_core_chunk_set_cstr(NODE_MEM(node), &node->as.code.info.value, info)) {
            return 0;
        }
        node->as.code.info.has_value = info != NULL;
        return 1;
    } else {
        return 0;
    }
}

int markdown_core_node_get_fence_closed(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }

    if (node->type == MARKDOWN_CORE_NODE_CODE_BLOCK) {
        return node->as.code.fenced && node->as.code.fence_closed;
    } else {
        return 0;
    }
}

const char *markdown_core_node_get_url(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    }

    switch (node->type) {
    case MARKDOWN_CORE_NODE_LINK:
    case MARKDOWN_CORE_NODE_IMAGE:
        return markdown_core_chunk_to_cstr(NODE_MEM(node), &node->as.link.url);
    default:
        break;
    }

    return NULL;
}

int markdown_core_node_set_url(markdown_core_node *node, const char *url) {
    if (node == NULL) {
        return 0;
    }

    switch (node->type) {
    case MARKDOWN_CORE_NODE_LINK:
    case MARKDOWN_CORE_NODE_IMAGE:
        return markdown_core_chunk_set_cstr(NODE_MEM(node), &node->as.link.url, url);
    default:
        break;
    }

    return 0;
}

const char *markdown_core_node_get_title(markdown_core_node *node) {
    if (node == NULL) {
        return NULL;
    }

    switch (node->type) {
    case MARKDOWN_CORE_NODE_LINK:
    case MARKDOWN_CORE_NODE_IMAGE:
        /* ABSENT IS NULL, for the reason `get_fence_info` states. */
        if (!node->as.link.title.has_value) {
            return NULL;
        }
        return markdown_core_chunk_to_cstr(NODE_MEM(node), &node->as.link.title.value);
    default:
        break;
    }

    return NULL;
}

int markdown_core_node_set_title(markdown_core_node *node, const char *title) {
    if (node == NULL) {
        return 0;
    }

    switch (node->type) {
    case MARKDOWN_CORE_NODE_LINK:
    case MARKDOWN_CORE_NODE_IMAGE:
        if (!markdown_core_chunk_set_cstr(NODE_MEM(node), &node->as.link.title.value, title)) {
            return 0;
        }
        node->as.link.title.has_value = title != NULL;
        return 1;
    default:
        break;
    }

    return 0;
}

int markdown_core_node_set_syntax_extension(markdown_core_node *node, const markdown_core_syntax_extension *extension) {
    if (node == NULL) {
        return 0;
    }

    node->extension = extension;
    return 1;
}

int markdown_core_node_get_start_line(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }
    return node->start_line;
}

int markdown_core_node_get_start_column(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }
    return node->start_column;
}

int markdown_core_node_get_end_line(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }
    return node->end_line;
}

int markdown_core_node_get_end_column(markdown_core_node *node) {
    if (node == NULL) {
        return 0;
    }
    return node->end_column;
}

// Unlink a node without adjusting its next, prev, and parent pointers.
static void S_node_unlink(markdown_core_node *node) {
    markdown_core_node *parent;
    if (node == NULL) {
        return;
    }

    if (node->prev) {
        node->prev->next = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    }

    parent = node->parent;
    if (parent == NULL) {
        return;
    }
    if (MARKDOWN_CORE_NODE_ARRAY_P(parent)) {
        /* The vector is the parent's sibling order (#161): close the gap.
         * The dual links above already spliced the neighbors. */
        size_t i;
        for (i = 0; i < parent->children.count; i++) {
            if (parent->children.vec[i] == node) {
                memmove(
                    &parent->children.vec[i],
                    &parent->children.vec[i + 1],
                    (parent->children.count - i - 1) * sizeof(*parent->children.vec)
                );
                parent->children.count--;
                break;
            }
        }
        return;
    }
    // Adjust first_child and last_child of parent.
    if (parent->first_child == node) {
        parent->first_child = node->next;
    }
    if (parent->last_child == node) {
        parent->last_child = node->prev;
    }
}

/* Grow the parent's vector by one at `at`. Fallible, so every splice runs it
 * FIRST: a refused insert leaves both the vector and the links untouched. An
 * arena parent's vector is arena memory (never realloc'd): growth takes a
 * fresh bump and the old bytes ride out with the pages. */
static int S_vec_insert(markdown_core_node *parent, size_t at, markdown_core_node *child) {
    markdown_core_node **grown;
    if (parent->flags & MARKDOWN_CORE_NODE__ARENA) {
        grown = (markdown_core_node **)markdown_core_node_arena_bytes(
            markdown_core_node_arena_of(parent),
            (parent->children.count + 1) * sizeof(*grown)
        );
        if (!grown) {
            return 0;
        }
        memcpy(grown, parent->children.vec, at * sizeof(*grown));
        memcpy(&grown[at + 1], &parent->children.vec[at], (parent->children.count - at) * sizeof(*grown));
    } else {
        grown = (markdown_core_node **)NODE_MEM(parent)->realloc(
            parent->children.vec,
            (parent->children.count + 1) * sizeof(*grown)
        );
        if (!grown) {
            return 0;
        }
        memmove(&grown[at + 1], &grown[at], (parent->children.count - at) * sizeof(*grown));
    }
    grown[at] = child;
    parent->children.vec = grown;
    parent->children.count++;
    return 1;
}

static size_t S_vec_index_of(const markdown_core_node *parent, const markdown_core_node *child) {
    size_t i = 0;
    while (i < parent->children.count && parent->children.vec[i] != child) {
        i++;
    }
    return i;
}

void markdown_core_node_unlink(markdown_core_node *node) {
    S_node_unlink(node);

    node->next = NULL;
    node->prev = NULL;
    node->parent = NULL;
}

int markdown_core_node_insert_before(markdown_core_node *node, markdown_core_node *sibling) {
    if (node == NULL || sibling == NULL) {
        return 0;
    }

    /* A node cannot be its own sibling. `S_can_contain(node->parent, sibling)`
     * cannot see this: with `sibling == node`, the ancestor walk starts at the
     * PARENT and never meets the child, so it answers yes. The splice below
     * then unlinks the node and re-links it to itself -- measured,
     * `insert_before(b, b)` returns 1 and leaves `b->next == b` and
     * `b->prev == b`, an unbounded sibling list that any traversal walks
     * forever. That is D34, and the safety flag never covered it. */
    if (node == sibling) {
        return 0;
    }

    if (!node->parent || !S_can_contain(node->parent, sibling)) {
        return 0;
    }

    S_node_unlink(sibling);

    markdown_core_node *parent = node->parent;
    if (MARKDOWN_CORE_NODE_ARRAY_P(parent) && !S_vec_insert(parent, S_vec_index_of(parent, node), sibling)) {
        sibling->next = NULL;
        sibling->prev = NULL;
        sibling->parent = NULL;
        return 0;
    }

    markdown_core_node *old_prev = node->prev;

    // Insert 'sibling' between 'old_prev' and 'node'.
    if (old_prev) {
        old_prev->next = sibling;
    }
    sibling->prev = old_prev;
    sibling->next = node;
    node->prev = sibling;

    sibling->parent = parent;

    // Adjust first_child of parent if inserted as first child.
    if (parent && !MARKDOWN_CORE_NODE_ARRAY_P(parent) && !old_prev) {
        parent->first_child = sibling;
    }

    return 1;
}

int markdown_core_node_insert_after(markdown_core_node *node, markdown_core_node *sibling) {
    if (node == NULL || sibling == NULL) {
        return 0;
    }

    /* A node cannot be its own sibling. `S_can_contain(node->parent, sibling)`
     * cannot see this: with `sibling == node`, the ancestor walk starts at the
     * PARENT and never meets the child, so it answers yes. The splice below
     * then unlinks the node and re-links it to itself -- measured,
     * `insert_before(b, b)` returns 1 and leaves `b->next == b` and
     * `b->prev == b`, an unbounded sibling list that any traversal walks
     * forever. That is D34, and the safety flag never covered it. */
    if (node == sibling) {
        return 0;
    }

    if (!node->parent || !S_can_contain(node->parent, sibling)) {
        return 0;
    }

    S_node_unlink(sibling);

    markdown_core_node *parent = node->parent;
    if (MARKDOWN_CORE_NODE_ARRAY_P(parent) && !S_vec_insert(parent, S_vec_index_of(parent, node) + 1, sibling)) {
        sibling->next = NULL;
        sibling->prev = NULL;
        sibling->parent = NULL;
        return 0;
    }

    markdown_core_node *old_next = node->next;

    // Insert 'sibling' between 'node' and 'old_next'.
    if (old_next) {
        old_next->prev = sibling;
    }
    sibling->next = old_next;
    sibling->prev = node;
    node->next = sibling;

    sibling->parent = parent;

    // Adjust last_child of parent if inserted as last child.
    if (parent && !MARKDOWN_CORE_NODE_ARRAY_P(parent) && !old_next) {
        parent->last_child = sibling;
    }

    return 1;
}

int markdown_core_node_replace(markdown_core_node *oldnode, markdown_core_node *newnode) {
    if (!markdown_core_node_insert_before(oldnode, newnode)) {
        return 0;
    }
    markdown_core_node_unlink(oldnode);
    return 1;
}

int markdown_core_node_prepend_child(markdown_core_node *node, markdown_core_node *child) {
    if (!S_can_contain(node, child)) {
        return 0;
    }

    S_node_unlink(child);

    if (MARKDOWN_CORE_NODE_ARRAY_P(node)) {
        markdown_core_node *old_first = node->children.count ? node->children.vec[0] : NULL;
        if (!S_vec_insert(node, 0, child)) {
            return 0;
        }
        child->next = old_first;
        child->prev = NULL;
        child->parent = node;
        if (old_first) {
            old_first->prev = child;
        }
        return 1;
    }

    markdown_core_node *old_first_child = node->first_child;

    child->next = old_first_child;
    child->prev = NULL;
    child->parent = node;
    node->first_child = child;

    if (old_first_child) {
        old_first_child->prev = child;
    } else {
        // Also set last_child if node previously had no children.
        node->last_child = child;
    }

    return 1;
}

int markdown_core_node_append_child(markdown_core_node *node, markdown_core_node *child) {
    if (!S_can_contain(node, child)) {
        return 0;
    }

    S_node_unlink(child);

    if (MARKDOWN_CORE_NODE_ARRAY_P(node)) {
        markdown_core_node *back = node->children.count ? node->children.vec[node->children.count - 1] : NULL;
        if (!S_vec_insert(node, node->children.count, child)) {
            return 0;
        }
        child->next = NULL;
        child->prev = back;
        child->parent = node;
        if (back) {
            back->next = child;
        }
        return 1;
    }

    markdown_core_node *old_last_child = node->last_child;

    child->next = NULL;
    child->prev = old_last_child;
    child->parent = node;
    node->last_child = child;

    if (old_last_child) {
        old_last_child->next = child;
    } else {
        // Also set first_child if node previously had no children.
        node->first_child = child;
    }

    return 1;
}

static void S_print_error(FILE *out, markdown_core_node *node, const char *elem) {
    if (out == NULL) {
        return;
    }
    fprintf(
        out,
        "Invalid '%s' in node type %s at %d:%d\n",
        elem,
        markdown_core_node_get_type_string(node),
        node->start_line,
        node->start_column
    );
}

int markdown_core_node_check(markdown_core_node *node, FILE *out) {
    /* Iterator-driven since #161/D9: the walk itself understands both child
     * shapes, and the checker asks per parent what its shape promises. A
     * borrowed list's nodes have no parent by contract (node.h), so the
     * parent check is not applied to them -- "repairing" it would hand the
     * list to whichever borrower was checked last. */
    markdown_core_iter walk;
    markdown_core_event_type ev_type;
    int errors = 0;

    if (!node) {
        return 0;
    }

    markdown_core_iter_init(&walk, node);
    while ((ev_type = markdown_core_iter_next(&walk)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *cur = markdown_core_iter_get_node(&walk);
        markdown_core_node *parent = NULL;
        if (ev_type != MARKDOWN_CORE_EVENT_ENTER) {
            continue;
        }
        /* The frame the ENTER just pushed is `cur` itself when it has
         * children; the parent context sits one below. */
        if (walk.depth > 0 && walk.frames[walk.depth - 1].node == cur) {
            parent = walk.depth > 1 ? walk.frames[walk.depth - 2].node : NULL;
        } else if (walk.depth > 0) {
            parent = walk.frames[walk.depth - 1].node;
        }

        /* #152/#153's invariant: a block that borrows its list owns no
         * mutable content arena -- its bytes, if any, alias a frozen buffer
         * the block holds a reference to (`asize == 0` marks the alias),
         * and the borrowed list is backed by its holder. Reported but not
         * repaired: freeing here would assert an ownership claim this walk
         * cannot verify. */
        if (MARKDOWN_CORE_NODE_BORROWED_P(cur) && cur->content.asize != 0) {
            S_print_error(out, cur, "borrowed content");
            ++errors;
        }
        if (cur->frozen_content && cur->content.asize != 0) {
            S_print_error(out, cur, "frozen content");
            ++errors;
        }
        if (parent == NULL) {
            continue;
        }
        if (MARKDOWN_CORE_NODE_ARRAY_P(parent)) {
            /* The vector is the order; a child may say this parent or, once
             * it is shared (#161), no parent at all -- never another tree's. */
            if (cur->parent != NULL && cur->parent != parent) {
                S_print_error(out, cur, "parent");
                ++errors;
            }
        } else if (MARKDOWN_CORE_NODE_BORROWED_P(parent)) {
            /* Parentless by contract; nothing to repair. */
        } else {
            if (cur->parent != parent) {
                S_print_error(out, cur, "parent");
                cur->parent = parent;
                ++errors;
            }
            if (cur->prev == NULL && parent->first_child != cur) {
                S_print_error(out, parent, "first_child");
                ++errors;
            }
            if (cur->next == NULL && parent->last_child != cur) {
                S_print_error(out, parent, "last_child");
                parent->last_child = cur;
                ++errors;
            }
            if (cur->next && cur->next->prev != cur) {
                S_print_error(out, cur->next, "prev");
                cur->next->prev = cur;
                ++errors;
            }
        }
    }

    return errors;
}
