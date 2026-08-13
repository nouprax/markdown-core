#ifndef MARKDOWN_CORE_SESSION_INTERNAL_H
#define MARKDOWN_CORE_SESSION_INTERNAL_H

#include "../include/markdown_core.h"
#include "arena.h"
#include "ast_internal.h"

#include <map.h>
#include <markdown-core.h>

#include "source.h"

// AddressSanitizer detection: document pooling is bypassed under ASan so the
// sanitizer keeps seeing individual allocations (see markdown_core_document_open_with_mem).
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

/** SplitMix64 finalizer shared by the document's open-addressing tables. */
static inline uint64_t markdown_core_mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

/* One per series, shared by every document in it: see the note in
 * document.c. Holds the revision counter that keeps two successors of one
 * predecessor apart, and the reference count that ends it with the last
 * document that came from it. */
typedef struct markdown_core_series_clock {
    volatile uint64_t next_revision;
    volatile uint32_t refcount;
} markdown_core_series_clock;

struct markdown_core_document {
    markdown_core_mem *mem;
    markdown_core_parse_options options;
    // The document's bytes: one growable buffer, mutable and singly owned,
    // filled once when the document is built (source.h). It was a persistent
    // rope for two reasons now gone — the bounded-neighbourhood copy was
    // 11.1's removed work bound, and no successor needs a predecessor's
    // bytes: an edit is handed the whole new text and fills a buffer of its
    // own.
    markdown_core_source *source;
    markdown_core_node *root; // the committed tree, owned
    // What an editor underlines, in source order, owned. Taken from the
    // parser when this document takes its tree, so it describes exactly the
    // committed text.
    markdown_core_diagnostic *diagnostics;
    size_t diagnostic_count;
    uint64_t next_id; // monotonic, starts at 1, never reused
    uint64_t series;
    uint64_t revision;
    markdown_core_series_clock *clock;
    int total_lines;      // parser line count of the committed text
    int last_line_length; // parser's final-line length of the committed text
    // When pooled, every document-owned allocation flows through this arena
    // (document->mem is its allocator face) and teardown is a wholesale
    // release. NULL for unpooled documents: the one-shot parse (its detached
    // tree must outlive the document and a parse keeps its v1 memory
    // profile) and the ASan suites.
    markdown_core_arena *arena;
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
    const markdown_core_document *old,
    markdown_core_document *nw,
    markdown_core_error **error
);

/** Adopts ids from `old_root` (may be NULL) onto `new_root`, assigns
 * last_changed_rev = new_rev to every added/changed/bubbled node, and carries
 * the old revision over for untouched subtrees. Returns false on allocation
 * failure (the trees are left consistent; the caller discards `new_root`). */
bool markdown_core_diff_trees(
    markdown_core_document *document,
    markdown_core_node *old_root,
    markdown_core_node *new_root,
    uint64_t new_rev
);

/** Creates a parser configured with the document's options and extensions.
 * Returns NULL on allocation or extension-registry failure with *error set
 * when non-NULL. Defined in document.c. */
markdown_core_parser *markdown_core_document_new_parser(markdown_core_document *document, markdown_core_error **error);

#endif
