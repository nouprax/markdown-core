#ifndef MARKDOWN_CORE_AST_INTERNAL_H
#define MARKDOWN_CORE_AST_INTERNAL_H

#include "../include/markdown_core.h"
#include <markdown-core.h>
#include <parser.h>

/* C LINKAGE, AND WINDOWS IS THE ONLY PLACE THIS SHOWS. The Itanium ABI does not
 * mangle a variable at global scope, so `MARKDOWN_CORE_EXTENSION_*` resolves on
 * Linux and macOS whether or not the declaration says `extern "C"`; MSVC mangles
 * every variable, and a C++ translation unit including this header without the
 * guard fails to link with LNK2019. */
#ifdef __cplusplus
extern "C" {
#endif

struct markdown_core_document {
    markdown_core_mem *mem;
    markdown_core_node *root;
};

/* Testable implementation of the public facade transaction. The public entry
 * supplies the default allocator; allocation-failure tests supply an injected
 * allocator and assert the same consumer-visible error contract. */
markdown_core_document *markdown_core_document_parse_with_mem(const uint8_t *source, size_t length,
                                                              const markdown_core_parse_options *requested_options,
                                                              markdown_core_mem *mem, markdown_core_error **error);

#ifdef __cplusplus
}
#endif

#endif
