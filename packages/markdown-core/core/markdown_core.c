#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include "node.h"
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

/* READ-ONLY, AND THE FILE-SCOPE `static` IS PART OF IT. Three function pointers
 * that never change are writable global state in the shipped archive otherwise,
 * which `scripts/audit-package-contents.sh` rejects and which nothing wants: a
 * process that can scribble on this redirects every allocation the library
 * makes. The cast below is the only place constness is dropped, and no caller
 * writes through the result -- `markdown_core_mem` is passed around to be
 * CALLED, not assigned to. */
static const markdown_core_mem MARKDOWN_CORE_DEFAULT_MEM_ALLOCATOR = {xcalloc, xrealloc, xfree};

markdown_core_mem *markdown_core_get_default_mem_allocator(void) {
    return (markdown_core_mem *)&MARKDOWN_CORE_DEFAULT_MEM_ALLOCATOR;
}
