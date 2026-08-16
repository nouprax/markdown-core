#include "source.h"

#include <stdint.h>
#include <string.h>

// The stored bytes, in one buffer that only ever grows at the end.
//
// This was an AVL-balanced rope of windows into refcounted immutable
// buffers, then a growable buffer spliced in place by an edit primitive,
// then — while every document owned its own copy — a buffer filled once at
// construction. What the engine asks of it now is what a chain of appends
// actually needs: keep these bytes, take more at the end, and give me the
// bytes. Every caller is in extensions/document.c.
//
// Capacity doubles, so the copying a chain does over its life is linear in
// its final length. That is the whole of the byte layer's share of the
// living-tree plan's amortized bound: the per-tick O(document) join and the
// per-document copy of the same text are both gone, and what a tick pays for
// bytes is its own chunk.

struct markdown_core_source {
    markdown_core_mem *mem;
    uint8_t *bytes;
    size_t length;
    size_t capacity;
};

markdown_core_source *markdown_core_source_new(markdown_core_mem *mem) {
    markdown_core_source *source = (markdown_core_source *)mem->calloc(mem, 1, sizeof(markdown_core_source));
    if (source) {
        source->mem = mem;
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

bool markdown_core_source_reserve(markdown_core_source *source, size_t additional) {
    size_t needed;
    size_t capacity;
    uint8_t *grown;
    if (additional <= source->capacity - source->length) {
        return true;
    }
    if (additional > (size_t)PTRDIFF_MAX - source->length) {
        /* Not a C object, so not an allocation: a difference of two pointers
         * into the buffer has to be representable, and a size between
         * PTRDIFF_MAX and SIZE_MAX is arithmetically fine and still cannot
         * be allocated — which is what ASan reports when an allocator is
         * handed one. Chunk lengths arrive here straight from public API,
         * which is what makes the guard reachable at all. */
        return false;
    }
    needed = source->length + additional;
    /* The first reservation takes exactly what it was asked for: a document
     * built from text in one call never grows, and rounding it up would cost
     * every one-shot parse memory it will never use. Growth after that
     * doubles, which is what makes a chain's copying linear in its final
     * length. The doubling terminates without a wrap guard: `needed` is at
     * most PTRDIFF_MAX, so the loop stops at the first power of two that
     * covers it, and an allocator asked for something that large fails
     * honestly. */
    capacity = source->capacity;
    if (!capacity) {
        capacity = needed;
    }
    while (capacity < needed) {
        capacity *= 2;
    }
    grown = (uint8_t *)source->mem->realloc(source->mem, source->bytes, capacity);
    if (!grown) {
        return false;
    }
    source->bytes = grown;
    source->capacity = capacity;
    return true;
}

void markdown_core_source_commit(markdown_core_source *source, const uint8_t *bytes, size_t length) {
    if (!length) {
        return;
    }
    memcpy(source->bytes + source->length, bytes, length);
    source->length += length;
}

size_t markdown_core_source_length(const markdown_core_source *source) { return source->length; }

const uint8_t *markdown_core_source_run_at(const markdown_core_source *source, size_t offset, size_t *run_length) {
    *run_length = source->length - offset;
    return source->bytes + offset;
}
