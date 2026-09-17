#include "alloc.h"
#include "node.h"
#include "markdown-core.h"

int markdown_core_version(void) { return MARKDOWN_CORE_VERSION; }

const char *markdown_core_version_string(void) { return MARKDOWN_CORE_VERSION_STRING; }

/* READ-ONLY, AND THE FILE-SCOPE `static` IS PART OF IT. Three function pointers
 * that never change are writable global state in the shipped archive otherwise,
 * which `scripts/audit-package-contents.sh` rejects and which nothing wants: a
 * process that can scribble on this redirects every allocation the library
 * makes. The cast below is the only place constness is dropped, and no caller
 * writes through the result -- `markdown_core_mem` is passed around to be
 * CALLED, not assigned to.
 *
 * The three entries are the library's own allocation functions rather than
 * file-scope wrappers around the C library, so that everything reaching this
 * table reaches the ONE substitution point described in `alloc.h`. */
static const markdown_core_mem MARKDOWN_CORE_DEFAULT_MEM_ALLOCATOR = {markdown_core_alloc, markdown_core_realloc,
                                                                      markdown_core_free};

markdown_core_mem *markdown_core_get_default_mem_allocator(void) {
    return (markdown_core_mem *)&MARKDOWN_CORE_DEFAULT_MEM_ALLOCATOR;
}
