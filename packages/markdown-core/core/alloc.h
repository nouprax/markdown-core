#ifndef MARKDOWN_CORE_ALLOC_H
#define MARKDOWN_CORE_ALLOC_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* THE LIBRARY HAS ONE ALLOCATOR, AND IT IS NOT A PARSE PARAMETER.
 *
 * It used to be one. `markdown_core_mem` -- three function pointers -- was
 * threaded through 43 signatures and stored on every node, so that a caller
 * could supply a different allocator per parse. No caller can.
 * `core/exports/markdown_core.map` exports 75 symbols and none of them is
 * allocator-related, the one installed header (`include/markdown_core.h`)
 * declares nothing about it, and the only exported parse entry reaches
 * `elements/ast.c`, which hardcodes the default. The configurability was
 * reachable from the test suite alone, which links the static archive and so
 * reaches internals.
 *
 * Its cost was not only the plumbing. Because the allocator was PER-NODE
 * state, `markdown_core_node_free`, which takes a node and no parser, had to
 * find it on the node -- which is why every creation entry point was
 * `_with_mem`, and why a node carried a pointer that is invariant for the
 * whole process.
 *
 * THE SEAM IS THE LINKER, NOT A GLOBAL, and that is forced by two contracts
 * this repository already holds:
 *
 *   - `scripts/audit-package-contents.sh`: "The engine holds no process-level
 *     mutable state by contract: every object in the installed static archives
 *     must be free of writable data/bss/common/TLS definitions." A swappable
 *     allocator variable is exactly that, so there is no global to swap.
 *   - `elements/CMakeLists.txt`: "All static consumers, including internal
 *     tests and the CLI, use one complete archive." The tests link the archive
 *     that ships, so a test-only compile flag would ship with it.
 *
 * What is left is substitution at link time. `core/alloc.c` defines these
 * three functions AND NOTHING ELSE, so a test object that defines them keeps
 * that archive member from ever being pulled: an archive member is pulled only
 * to resolve an undefined symbol, and nothing else in the library needs
 * anything from that file. Any fourth symbol added to `alloc.c` would turn
 * interposition into a duplicate-definition error, which is why the file stays
 * as small as it is. `tests/runners/oom_runner.c` holds the failure-injection
 * counters that used to live behind `sweep_mem`; they sit in the test binary,
 * where mutable state is nobody's contract to keep.
 *
 * DECISION, not a side effect: concurrent parses now share this allocator, so
 * two threads can no longer parse with different ones. Nothing needs that.
 * `tests/runners/concurrency_runner.c` drives every thread through
 * `markdown_core_document_parse`, the default-allocator entry, in all three of
 * its modes. There is no state here to race on: the shipped definitions hold
 * nothing and forward straight to the C library.
 */
void *markdown_core_alloc(size_t count, size_t size);
void *markdown_core_realloc(void *pointer, size_t size);
void markdown_core_free(void *pointer);

/* A grow-only typed vector's storage. Refusal leaves both its pointer and
 * capacity intact; ownership stays with the caller until disposal. Assign
 * the returned pointer only on success. Returning a value avoids aliasing a
 * typed pointer object through void **. Requests here are nonempty. */
static inline void *markdown_core_reserve(void *values, size_t *capacity, size_t count, size_t width) {
    if (count <= *capacity) {
        return values;
    }
    if (!width || count > SIZE_MAX / width) {
        return NULL;
    }
    size_t limit = SIZE_MAX / width;
    size_t next = *capacity ? *capacity : 8;
    if (next > limit) {
        next = limit;
    }
    while (next < count) {
        next = next > limit / 2 ? limit : next * 2;
    }
    void *grown = markdown_core_realloc(values, next * width);
    if (!grown) {
        return NULL;
    }
    *capacity = next;
    return grown;
}

#ifdef __cplusplus
}
#endif

#endif
