#include <stdlib.h>
#include <assert.h>

#include "markdown-core.h"
#include "syntax_extension.h"
#include "buffer.h"

extern markdown_core_mem MARKDOWN_CORE_DEFAULT_MEM_ALLOCATOR;

static markdown_core_mem *_mem = &MARKDOWN_CORE_DEFAULT_MEM_ALLOCATOR;

void markdown_core_syntax_extension_free(markdown_core_mem *mem, markdown_core_syntax_extension *extension) {
    if (extension->free_function && extension->priv) {
        extension->free_function(mem, extension->priv);
    }

    markdown_core_llist_free(mem, extension->special_inline_chars);
    mem->free(extension->name);
    mem->free(extension);
}

markdown_core_syntax_extension *markdown_core_syntax_extension_new(const char *name) {
    markdown_core_syntax_extension *res =
        (markdown_core_syntax_extension *)_mem->calloc(1, sizeof(markdown_core_syntax_extension));
    if (!res)
        return NULL;
    res->name = (char *)_mem->calloc(1, sizeof(char) * (strlen(name)) + 1);
    if (!res->name) {
        _mem->free(res);
        return NULL;
    }
    strcpy(res->name, name);
    return res;
}

markdown_core_node_type markdown_core_syntax_extension_add_node(int is_inline) {
    markdown_core_node_type *ref = !is_inline ? &MARKDOWN_CORE_NODE_LAST_BLOCK : &MARKDOWN_CORE_NODE_LAST_INLINE;

    if ((*ref & MARKDOWN_CORE_NODE_VALUE_MASK) == MARKDOWN_CORE_NODE_VALUE_MASK) {
        assert(false);
        return (markdown_core_node_type)0;
    }

    return *ref = (markdown_core_node_type)((int)*ref + 1);
}

void markdown_core_syntax_extension_set_emphasis(markdown_core_syntax_extension *extension, int emphasis) {
    extension->emphasis = emphasis == 1;
}

void markdown_core_syntax_extension_set_open_block_func(markdown_core_syntax_extension *extension,
                                                        markdown_core_open_block_func func) {
    extension->try_opening_block = func;
}

void markdown_core_syntax_extension_set_match_block_func(markdown_core_syntax_extension *extension,
                                                         markdown_core_match_block_func func) {
    extension->last_block_matches = func;
}

void markdown_core_syntax_extension_set_match_inline_func(markdown_core_syntax_extension *extension,
                                                          markdown_core_match_inline_func func) {
    extension->match_inline = func;
}

void markdown_core_syntax_extension_set_inline_from_delim_func(markdown_core_syntax_extension *extension,
                                                               markdown_core_inline_from_delim_func func) {
    extension->insert_inline_from_delim = func;
}

void markdown_core_syntax_extension_set_special_inline_chars(markdown_core_syntax_extension *extension,
                                                             markdown_core_llist *special_chars) {
    extension->special_inline_chars = special_chars;
}

void markdown_core_syntax_extension_set_get_type_string_func(markdown_core_syntax_extension *extension,
                                                             markdown_core_get_type_string_func func) {
    extension->get_type_string_func = func;
}

void markdown_core_syntax_extension_set_can_contain_func(markdown_core_syntax_extension *extension,
                                                         markdown_core_can_contain_func func) {
    extension->can_contain_func = func;
}

void markdown_core_syntax_extension_set_contains_inlines_func(markdown_core_syntax_extension *extension,
                                                              markdown_core_contains_inlines_func func) {
    extension->contains_inlines_func = func;
}

void markdown_core_syntax_extension_set_accepts_lines_func(markdown_core_syntax_extension *extension,
                                                           markdown_core_accepts_lines_func func) {
    extension->accepts_lines_func = func;
}

void markdown_core_syntax_extension_set_postprocess_func(markdown_core_syntax_extension *extension,
                                                         markdown_core_postprocess_func func) {
    extension->postprocess_func = func;
}

void markdown_core_syntax_extension_set_private(markdown_core_syntax_extension *extension, void *priv,
                                                markdown_core_free_func free_func) {
    extension->priv = priv;
    extension->free_function = free_func;
}

void *markdown_core_syntax_extension_get_private(markdown_core_syntax_extension *extension) { return extension->priv; }

void markdown_core_syntax_extension_set_opaque_alloc_func(markdown_core_syntax_extension *extension,
                                                          markdown_core_opaque_alloc_func func) {
    extension->opaque_alloc_func = func;
}

void markdown_core_syntax_extension_set_opaque_free_func(markdown_core_syntax_extension *extension,
                                                         markdown_core_opaque_free_func func) {
    extension->opaque_free_func = func;
}
