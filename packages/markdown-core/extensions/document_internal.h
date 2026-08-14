#ifndef MARKDOWN_CORE_DOCUMENT_INTERNAL_H
#define MARKDOWN_CORE_DOCUMENT_INTERNAL_H

#include "../include/markdown_core.h"
#include "arena.h"
#include "ast_internal.h"

#include <map.h>
#include <markdown-core.h>

#include "source.h"

// AddressSanitizer detection: document pooling is bypassed under ASan so the
// sanitizer keeps seeing individual allocations (see markdown_core_document_alloc).
#ifndef MARKDOWN_CORE_ASAN
#if defined(__SANITIZE_ADDRESS__)
#define MARKDOWN_CORE_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MARKDOWN_CORE_ASAN 1
#else
#define MARKDOWN_CORE_ASAN 0
#endif
#else
#define MARKDOWN_CORE_ASAN 0
#endif
#endif

/** SplitMix64 finalizer: whitens the entropy that mints a chain's series
 * salt (see markdown_core_chain.series). */
static inline uint64_t markdown_core_mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

/* ONE BUILD'S OUTPUT: the tree and the diagnostics that describe it, and the
 * arena they came from. A generation is taken whole and released whole,
 * which is what makes a failed build cost exactly one release and a
 * successful one exactly one swap. */
typedef struct document_generation {
    markdown_core_arena *arena;
    markdown_core_mem *mem;
    markdown_core_node *root;
    markdown_core_diagnostic *diagnostics;
    size_t diagnostic_count;
} document_generation;

/* THE CHAIN OWNER. One per chain — a document and every successor an
 * append produced from it — shared by every live handle on the chain and
 * released with the last of them.
 *
 * The chain is what makes supersession enforceable: a mutation is legal only
 * on the handle whose revision sits just behind the chain clock, so a
 * superseded handle fails deterministically instead of forking history, and
 * the linear history that results is what lets a consumer destroy and
 * rebuild derived state in place. Only the refcount is atomic: handles on
 * one chain may be freed from different threads, but mutations are
 * externally serialized by contract, so the clock is a plain field the
 * current mutation owns. */
typedef struct markdown_core_chain {
    volatile uint32_t refcount;
    /* The chain clock: the revision the next successful append publishes,
     * strictly +1 per append — a failed build burns no number. Doubles as
     * the head predicate: a handle is the live head exactly while its
     * revision + 1 equals this, so supersession is one increment. */
    uint64_t next_revision;
    uint64_t series; /* the salt every document on the chain shares,
                        minted once at chain birth */
    /* THE CHAIN'S BYTES. One buffer for the whole chain: appends land at the
     * end and every document is a length watermark into it (source.h), so a
     * tick copies its own chunk rather than the document. The head's
     * watermark is always the stored length — bytes are committed here only
     * once the mutation that describes them has succeeded. Owned; released
     * with the chain, after the last handle. */
    markdown_core_source *source;
    /* The chain's base allocator: every successor builds over it, so a chain
     * opened over an injected allocator stays observable to the injection —
     * mutations do not silently fall back to the default. Borrowed; the
     * opener guarantees it outlives the chain. */
    markdown_core_mem *mem;
    /* A failed append is "the chain is done": nothing further may mutate it,
     * the caller holds the text, and recovery is a rebuild. */
    bool poisoned;
    /* THE TICK LEDGER. Every mutation lands in exactly one of these, and
     * their sum is the number of mutations the chain has served. Today the
     * warm count is structurally zero — there is no warm path yet — and the
     * counter exists at full corpus exercise BEFORE the path it will measure,
     * so that when the first warm tick lands the share it takes is read off a
     * gate that has been honest all along rather than one written to greet
     * it. The milestone's bound is about bytes, not ticks: `rebuilt_bytes` is
     * what the fallback actually costs, since a tenth of the ticks each
     * reparsing the whole document is not a tenth of a problem. */
    uint64_t warm_ticks;
    uint64_t rebuilt_ticks;
    uint64_t rebuilt_bytes;
    /* THE IDENTITY COUNTER. Monotonic, starts at 1, never reused, and one
     * per chain because identity is what a consumer keys on across a whole
     * stream. A build that fails after minting burns the numbers it took;
     * they are unique either way, and a counter that could go backwards to
     * reclaim them would be the defect. */
    uint64_t next_id;
    /* THE OPTIONS, fixed for the chain's whole life: changing what the
     * parser means is a new chain, so every build on this one reads these. */
    markdown_core_parse_options options;
    /* Whether builds pool their allocations in an arena. Fixed at birth, so
     * a generation is released the same way it was taken. */
    bool pooled;
    /* THE HEAD'S GENERATION. A build produces a tree and the diagnostics
     * that describe it, out of one arena; publishing swaps the whole thing
     * in and releases what it replaced. The head is the only generation the
     * chain keeps, which is exactly what a superseded handle answering for
     * no tree buys (the Mutation section of the public header). */
    document_generation head;
} markdown_core_chain;

/* A HANDLE, and nothing more. Everything a document is made of belongs to
 * the chain; what a handle carries is which document it names. */
struct markdown_core_document {
    // This document's text: the chain's first `length` bytes. A watermark,
    // not a buffer — successors only ever add bytes past it, so what this
    // document describes never moves and never changes.
    size_t length;
    /* This handle's place on the chain: it is the live head — and mutation
     * legal — exactly while revision + 1 == chain->next_revision. */
    uint64_t revision;
    markdown_core_chain *chain;
};

/** Internal constructor for an empty document over an explicit allocator;
 * the public markdown_core_document_new uses the default allocator with
 * pooling. `pooled` routes every document-owned allocation through an
 * arena over `mem` — pass false when detached nodes must outlive the document
 * or when injection needs to see individual allocations. */
markdown_core_document *markdown_core_document_open_with_mem(
    const markdown_core_parse_options *options,
    markdown_core_mem *mem,
    bool pooled,
    markdown_core_error **error
);

/** Whether two same-raw-type nodes' OWN projections differ: kind and scalar
 * fields, string-valued fields, and canonical text bytes — child lists and
 * descendants are the diff walk's to judge. Allocation failure reports
 * "differs" so a revision bump can never be missed. Defined in ast.c next to
 * the dump implementation, which reads the same fields. */
bool markdown_core_ast_projection_changed(const markdown_core_node *a, const markdown_core_node *b);

/** DIFF: assigns `nw`'s identities from `old` (which may be NULL) and stamps
 * revisions. Reads no text; reparses nothing. A pure function of two trees,
 * which is what lets the parse be a pure function of (bytes, options). */
bool markdown_core_document_diff(
    markdown_core_chain *chain,
    markdown_core_mem *mem,
    markdown_core_node *old_root,
    markdown_core_node *new_root,
    uint64_t new_revision,
    markdown_core_error **error
);

/** Adopts ids from `old_root` (may be NULL) onto `new_root`, assigns
 * last_changed_rev = new_rev to every added/changed/bubbled node, and carries
 * the old revision over for untouched subtrees. Returns false on allocation
 * failure (the trees are left consistent; the caller discards `new_root`). */
bool markdown_core_diff_trees(
    markdown_core_chain *chain,
    markdown_core_mem *mem,
    markdown_core_node *old_root,
    markdown_core_node *new_root,
    uint64_t new_rev
);

/** Creates a parser configured with the document's options and extensions.
 * Returns NULL on allocation or extension-registry failure with *error set
 * when non-NULL. Defined in document.c. */
markdown_core_parser *markdown_core_document_new_parser(
    const markdown_core_parse_options *options,
    markdown_core_mem *mem,
    markdown_core_error **error
);

#endif
