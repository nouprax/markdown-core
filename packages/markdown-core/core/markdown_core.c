#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include "markdown-core.h"

int markdown_core_version(void) { return MARKDOWN_CORE_VERSION; }

const char *markdown_core_version_string(void) { return MARKDOWN_CORE_VERSION_STRING; }

static void *xcalloc(size_t nmem, size_t size) {
    void *ptr = calloc(nmem, size);
    if (!ptr) {
        fprintf(stderr, "[markdown_core] calloc returned null pointer, aborting\n");
        abort();
    }
    return ptr;
}

static void *xrealloc(void *ptr, size_t size) {
    void *new_ptr = realloc(ptr, size);
    if (!new_ptr) {
        fprintf(stderr, "[markdown_core] realloc returned null pointer, aborting\n");
        abort();
    }
    return new_ptr;
}

static void xfree(void *ptr) { free(ptr); }

/* Immutable by contract: the engine has no process-level mutable state, so
 * the default allocator lives in read-only storage. Callers receive a
 * non-const pointer because markdown_core_mem flows through APIs that also
 * accept caller-owned allocators; writes through it would fault. */
static const markdown_core_mem MARKDOWN_CORE_DEFAULT_MEM_ALLOCATOR = {xcalloc, xrealloc, xfree};

markdown_core_mem *markdown_core_get_default_mem_allocator(void) {
    return (markdown_core_mem *)&MARKDOWN_CORE_DEFAULT_MEM_ALLOCATOR;
}
