#ifndef MARKDOWN_CORE_SYNTAX_EXTENSION_H
#define MARKDOWN_CORE_SYNTAX_EXTENSION_H

#include "markdown-core.h"
#include "markdown-core-extension-api.h"
#include "config.h"

struct markdown_core_syntax_extension {
    markdown_core_match_block_func last_block_matches;
    markdown_core_open_block_func try_opening_block;
    markdown_core_match_inline_func match_inline;
    markdown_core_inline_from_delim_func insert_inline_from_delim;
    markdown_core_llist *special_inline_chars;
    char *name;
    void *priv;
    bool emphasis;
    markdown_core_free_func free_function;
    markdown_core_get_type_string_func get_type_string_func;
    markdown_core_can_contain_func can_contain_func;
    markdown_core_contains_inlines_func contains_inlines_func;
    markdown_core_accepts_lines_func accepts_lines_func;
    markdown_core_postprocess_func postprocess_func;
    markdown_core_opaque_alloc_func opaque_alloc_func;
    markdown_core_opaque_free_func opaque_free_func;
};

#endif
