#ifndef MARKDOWN_CORE_ARENA_H
#define MARKDOWN_CORE_ARENA_H

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

/* Zeroed storage that lives as long as the arena. */
void *markdown_core_arena_alloc(markdown_core_arena *arena, size_t size);
/* Zeroed storage for a record that may be recycled with the same size. */
void *markdown_core_arena_take(markdown_core_arena *arena, size_t size);
void markdown_core_arena_recycle(markdown_core_arena *arena, void *record, size_t size);

#ifdef __cplusplus
}
#endif

#endif
