#ifndef MARKDOWN_CORE_ARENA_H
#define MARKDOWN_CORE_ARENA_H

#include <markdown-core.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Size-classed slab arena behind the markdown_core_mem interface.
 *
 * A session allocates everything it owns — nodes, content buffers, tables,
 * the staged parsers — through its arena. Freed blocks go to per-class
 * freelists and are reused by later commits; memory returns to the base
 * allocator only at release, so a long-lived session holds its high-water
 * mark. Growing a block within its class capacity is a no-op, which absorbs
 * most content-buffer reallocations. Requests above the largest class pass
 * through to the base allocator and are tracked so release stays wholesale.
 *
 * The arena embeds its markdown_core_mem first and the allocator functions
 * recover it by casting back; there is no global state and no locking — an
 * arena is confined to its session exactly like the session's other state.
 * Allocation failure (base refill, overflow) surfaces as NULL through the
 * markdown_core_mem contract.
 */
typedef struct markdown_core_arena markdown_core_arena;

/** Creates an arena over `base`. Returns NULL on allocation failure. */
markdown_core_arena *markdown_core_arena_new(markdown_core_mem *base);

/** The arena's allocator face; valid until markdown_core_arena_release. */
markdown_core_mem *markdown_core_arena_mem(markdown_core_arena *arena);

/** Returns every slab and passthrough block to the base allocator and frees
 * the arena. Everything ever allocated through the arena dies with it. */
void markdown_core_arena_release(markdown_core_arena *arena);

#ifdef __cplusplus
}
#endif

#endif
