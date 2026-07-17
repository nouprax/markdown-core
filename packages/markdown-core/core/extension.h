#ifndef MARKDOWN_CORE_EXTENSION_H
#define MARKDOWN_CORE_EXTENSION_H

#include "markdown-core.h"
#include "markdown-core-extension-api.h"
#include "config.h"

// Extension descriptors are immutable compile-time data: every bundled
// extension defines one `static const` instance and hands out a pointer to
// it. The engine never allocates, mutates, or frees a descriptor, which is
// what keeps the parse path free of process-global mutable state.
struct markdown_core_extension {
    markdown_core_match_block_func last_block_matches;
    markdown_core_open_block_func try_opening_block;
    markdown_core_match_inline_func match_inline;
    markdown_core_inline_from_delim_func insert_inline_from_delim;
    const unsigned char *special_inline_chars;
    size_t special_inline_char_count;
    const char *name;
    bool emphasis;
    markdown_core_get_type_string_func get_type_string;
    markdown_core_can_contain_func can_contain;
    markdown_core_contains_inlines_func contains_inlines;
    markdown_core_accepts_lines_func accepts_lines;
    markdown_core_postprocess_block_func postprocess_block;
    markdown_core_alloc_opaque_func alloc_opaque;
    markdown_core_free_opaque_func free_opaque;
};

#endif
