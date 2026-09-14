#ifndef MARKDOWN_CORE_ARENA_H
#define MARKDOWN_CORE_ARENA_H

#include <stdbool.h>
#include <stddef.h>

#include "markdown-core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* THE STORAGE OF ONE PARSE TRANSACTION.
 *
 * Nodes, their initial records and the parser's short-lived fixed-size
 * records (delimiters, brackets) are carved out of blocks the arena takes
 * from the caller's allocator. The document that the transaction produces
 * owns the arena through its root and releases every block at once; a
 * record given back during the parse joins a size-class pool and is handed
 * out again, so the arena never holds more than the peak live records plus
 * one partially filled block. Growth failure reports NULL, which the parse
 * treats exactly like any other terminal allocation failure. */
typedef struct markdown_core_arena markdown_core_arena;

markdown_core_arena *markdown_core_arena_new(markdown_core_mem *mem);
void markdown_core_arena_free(markdown_core_arena *arena);
markdown_core_mem *markdown_core_arena_mem(const markdown_core_arena *arena);

/* Storage that lives as long as the arena, not zeroed: the caller writes
 * every byte it asked for. */
void *markdown_core_arena_alloc(markdown_core_arena *arena, size_t size);
/* The same, for bytes that need no alignment -- copied text. Short strings
 * pack against each other inside a slab rather than each taking an alignment
 * granule. Record storage is unaffected: the two never share a cursor. */
void *markdown_core_arena_text(markdown_core_arena *arena, size_t size);
/* Grow the arena's latest allocation in place: `storage`, the `size` bytes
 * served last, becomes `needed` bytes when its block has the room. Returns
 * false, changing nothing, when `storage` is not the latest allocation or
 * the block is full; the caller then allocates elsewhere. */
bool markdown_core_arena_extend(markdown_core_arena *arena, const void *storage, size_t size, size_t needed);
/* Recycled records are pooled by size class, and every record has a class:
 * every size this large or smaller has one of its own, and a larger record is
 * served at the next power of two. So a record the arena handed out can
 * always be handed back, whatever its size, and a caller that replaces its
 * storage does not have to know which sizes the pools accept. A record above
 * this bound pays the round-up to the next power of two, which is why a
 * record of a fixed size of its own choosing is still worth keeping within
 * it. */
#define MARKDOWN_CORE_ARENA_GRANULE 16
#define MARKDOWN_CORE_ARENA_CLASSES 64
#define MARKDOWN_CORE_ARENA_RECYCLED_MAX ((size_t)MARKDOWN_CORE_ARENA_GRANULE * MARKDOWN_CORE_ARENA_CLASSES)

/* Zeroed storage for a record that may be recycled with the same size. */
void *markdown_core_arena_take(markdown_core_arena *arena, size_t size);
void markdown_core_arena_recycle(markdown_core_arena *arena, void *record, size_t size);
/* Whether `record` lies inside storage this arena handed out: the identity of
 * the transaction a node belongs to, read from its address. */
bool markdown_core_arena_owns(const markdown_core_arena *arena, const void *record);

#ifdef __cplusplus
}
#endif

#endif
