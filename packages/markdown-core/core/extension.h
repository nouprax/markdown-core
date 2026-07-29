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
    /* Bytes scan_delims treats as transparent when classifying emphasis
     * flanking. Only bytes that cannot appear in user text (internal
     * sentinel delimiters) or that inherited gfm semantics require ('~')
     * belong here; real punctuation must stay visible to flanking so
     * default-options parses keep CommonMark emphasis behavior. */
    const unsigned char *flanking_skip_chars;
    size_t flanking_skip_char_count;
    const char *name;
    markdown_core_get_type_string_func get_type_string;
    markdown_core_can_contain_func can_contain;
    markdown_core_contains_inlines_func contains_inlines;
    markdown_core_finalize_transient_inline_owner_func finalize_transient_inline_owner;
    markdown_core_prepare_inline_domain_func prepare_inline_domain;
    markdown_core_accepts_lines_func accepts_lines;
    markdown_core_postprocess_block_func postprocess_block;
    markdown_core_alloc_opaque_func alloc_opaque;
    markdown_core_free_opaque_func free_opaque;
};

#endif
