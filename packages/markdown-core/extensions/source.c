#include "source.h"

#include <stdint.h>
#include <string.h>

// The stored bytes, in one buffer filled at construction.
//
// This was an AVL-balanced rope of windows into refcounted immutable buffers,
// then a growable buffer spliced in place by an edit primitive. Both served
// callers that no longer exist: the engine's one mutation, append, builds a
// whole successor document and reads the predecessor's bytes through run_at —
// a per-tick copy the living-tree plan §2 retires by making the chain own one
// growable buffer. Until then, what the engine asks of this file is exactly
// what it exports: hold these bytes, and give me the bytes. Every caller is
// in extensions/document.c.

struct markdown_core_source {
    markdown_core_mem *mem;
    uint8_t *bytes;
    size_t length;
};

markdown_core_source *markdown_core_source_new(markdown_core_mem *mem, const uint8_t *bytes, size_t length) {
    markdown_core_source *source;
    if (length > (size_t)PTRDIFF_MAX) {
        /* Not a C object, so not an allocation: a difference of two pointers
         * into the buffer has to be representable, and a size between
         * PTRDIFF_MAX and SIZE_MAX is arithmetically fine and still cannot
         * be allocated — which is what ASan reports when an allocator is
         * handed one. The length arrives here straight from public API,
         * which is what makes the guard reachable at all. */
        return NULL;
    }
    source = (markdown_core_source *)mem->calloc(mem, 1, sizeof(markdown_core_source));
    if (!source) {
        return NULL;
    }
    source->mem = mem;
    if (length) {
        source->bytes = (uint8_t *)mem->realloc(mem, NULL, length);
        if (!source->bytes) {
            mem->free(mem, source);
            return NULL;
        }
        memcpy(source->bytes, bytes, length);
        source->length = length;
    }
    return source;
}

void markdown_core_source_release(markdown_core_source *source) {
    markdown_core_mem *mem;
    if (!source) {
        return;
    }
    mem = source->mem;
    if (source->bytes) {
        mem->free(mem, source->bytes);
    }
    mem->free(mem, source);
}

size_t markdown_core_source_length(const markdown_core_source *source) { return source->length; }

const uint8_t *markdown_core_source_run_at(const markdown_core_source *source, size_t offset, size_t *run_length) {
    *run_length = source->length - offset;
    return source->bytes + offset;
}
