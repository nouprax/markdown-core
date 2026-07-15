#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include "registry.h"
#include "node.h"
#include "markdown-core.h"

markdown_core_node_type MARKDOWN_CORE_NODE_LAST_BLOCK = MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION;
markdown_core_node_type MARKDOWN_CORE_NODE_LAST_INLINE = MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE;

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

markdown_core_mem MARKDOWN_CORE_DEFAULT_MEM_ALLOCATOR = {xcalloc, xrealloc, xfree};

markdown_core_mem *markdown_core_get_default_mem_allocator(void) { return &MARKDOWN_CORE_DEFAULT_MEM_ALLOCATOR; }
