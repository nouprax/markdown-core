#ifndef MARKDOWN_CORE_DOCUMENT_INTERNAL_H
#define MARKDOWN_CORE_DOCUMENT_INTERNAL_H

#include "../include/markdown_core.h"
#include "arena.h"
#include "ast_internal.h"

#include <map.h>
#include <markdown-core.h>
#include <parser.h>

#include "source.h"

// AddressSanitizer detection: document pooling is bypassed under ASan so the
// sanitizer keeps seeing individual allocations (see chain_new in document.c).
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

/* ONE BUILD'S OUTPUT, AND THE PARSER THAT MAY STILL GROW IT: the tree, the
 * diagnostics that describe it, the parser that owns the tree and is kept
 * at end of feed, the record of the publish that closed it — NULL only
 * before a build, and after a failed tick — and the arena they all came
 * from. A generation is
 * taken whole and released whole, which is what makes a failed build cost
 * exactly one release and a successful one exactly one swap; a WARM tick
 * does not make a generation, it grows this one in place. */
typedef struct document_generation {
    markdown_core_arena *arena;
    markdown_core_mem *mem;
    markdown_core_parser *parser; /* owns the tree: parser->root */
    markdown_core_warm_undo *undo;
    markdown_core_diagnostic *diagnostics;
    size_t diagnostic_count;
} document_generation;

/** The generation's tree — the parser's, which owns it — or NULL before a
 * build has produced one. The one place the answer lives. */
static inline markdown_core_node *document_generation_root(const document_generation *generation) {
    return generation->parser ? generation->parser->root : NULL;
}

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
     * the caller holds the text, and recovery is a new document. */
    bool poisoned;
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
    /* THE HEAD'S GENERATION. The first build produces a tree and the
     * diagnostics that describe it, out of one arena, and swaps the whole
     * thing in; every append grows it in place (document_tick_warm), and it
     * is released only with the chain. The head is the only generation the
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

/** The same projection, written to `out` as bytes: two nodes whose bytes
 * are equal are nodes the comparison above calls unchanged. Answers false
 * when the buffer lost an allocation. */
bool markdown_core_ast_projection_write(const markdown_core_node *node, markdown_core_strbuf *out);

/** A WITNESS of one block's own projection across the publishes of a
 * stream: the bytes above, except that a text which is the block's own
 * content buffer — a code block's literal, an HTML block's — is witnessed
 * by its length. What a streaming tick keeps of a spine block's PUBLISHED
 * projection, so the next publish can be compared against it — the block
 * is the same object, so there is no second node to compare — with no hash
 * trusted for a revision. The length suffices because the buffer only
 * grows for the block's life: its close moves it into the literal whole
 * and its retract moves it back (core/parser.h, MARKDOWN_CORE_WARM_CONTENT_MOVED),
 * so for ONE block across two publishes equal length is equal bytes, and a
 * growing fence costs its record nothing per byte it already holds. Two
 * different nodes have no such relation, and are compared by the exact
 * writer or markdown_core_ast_projection_changed; the concrete runner's
 * projection_witness_agrees holds the witness to the exact bytes over
 * every spine block of every tick of its streams. Answers false when the
 * buffer lost an allocation. */
bool markdown_core_ast_projection_witness(const markdown_core_node *node, markdown_core_strbuf *out);

/** Mints fresh identities over one subtree — every node id from the chain's
 * counter, every revision `rev` — for a subtree nothing pairs against. */
void markdown_core_diff_mint(markdown_core_chain *chain, markdown_core_node *root, uint64_t rev);

/** IDENTITY HANDOVER AT THE FRONTIER of a warm tick. `fresh` is the run of
 * children a spine block gained since the previous publish — what the feed
 * appended and what this close appended, in that order — and `retired` is
 * the run the previous close had appended there, detached at the retract and
 * owning its bytes. The two are diffed as two child lists — hash sweeps,
 * the middle aligned, residue minted, each pair classified by its fields
 * and its children — so a paired node keeps its id,
 * keeps its revision if nothing about it changed and takes `rev` otherwise,
 * and unpaired retired ids are never minted again. `*changed` is SET when
 * the runs differ at all — never cleared, so a block's two runs accumulate
 * into one verdict — which is what the spine block above them inherits; the
 * fresh run ends at `fresh_end` (exclusive; NULL for the end of the sibling
 * list). Requires the fresh run to be stamped. Frees nothing. Returns false
 * on allocation failure with the fresh run partly assigned — the caller
 * discards the tick. */
bool markdown_core_diff_frontier(
    markdown_core_chain *chain,
    markdown_core_mem *mem,
    markdown_core_node *retired,
    markdown_core_node *fresh,
    const markdown_core_node *fresh_end,
    uint64_t rev,
    bool *changed
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
