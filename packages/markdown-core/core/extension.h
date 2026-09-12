#ifndef MARKDOWN_CORE_EXTENSION_H
#define MARKDOWN_CORE_EXTENSION_H

#include "markdown-core.h"
#include "markdown-core-extension-api.h"
#include "config.h"
#include "chunk.h"
#include "delimiter.h"

/* A speculative opener reads following lines through its caller's source
 * view. Captured and streaming inputs therefore use the same grammar without
 * nesting parser transactions or changing tree ownership. */
typedef struct markdown_core_block_reader {
    void *context;
    int (*next)(void *context, markdown_core_chunk *input, int *first, int *indent);
} markdown_core_block_reader;

typedef int (*markdown_core_probe_block_func)(markdown_core_parser *parser, markdown_core_chunk *input, int first,
                                              int indent, markdown_core_block_reader *reader);

/* Node-valued fields are independent child-tree roots. This internal hook
 * exposes their owning slots only to parser phases; it does not change the
 * public child iterator or make a field a parent/child edge. Destruction
 * transfers these roots to the shared iterative walk and clears their slots
 * before opaque_free_func releases the extension payload. That callback must
 * not recursively destroy node-valued fields. Kind conversion preserves the
 * extension payload and these roots. */
typedef int (*markdown_core_owned_subtree_visitor)(markdown_core_node **root_slot, void *context);
typedef int (*markdown_core_visit_owned_subtrees_func)(const markdown_core_extension *extension,
                                                       markdown_core_node *node,
                                                       markdown_core_owned_subtree_visitor visitor, void *context);

/* Block grammar precedence is independent of attach order and syntax origin.
 * Container prefixes precede leaf blocks; list/definition markers follow
 * headings, fences, HTML, setext and thematic breaks. Both slots use the same
 * non-consuming recognition contract and committed-open callback. */
typedef enum {
    MARKDOWN_CORE_BLOCK_PREFIX,
    MARKDOWN_CORE_BLOCK_MARKER,
    MARKDOWN_CORE_BLOCK_PRECEDENCE_COUNT
} markdown_core_block_precedence;
struct markdown_core_block_start_context;
struct markdown_core_block_start;

struct markdown_core_extension {
    markdown_core_block_precedence block_precedence;
    bool (*scan_block_start)(markdown_core_parser *, struct markdown_core_block_start_context *,
                             struct markdown_core_block_start *);
    /* Last refusal before an ordinary paragraph, after opaque blocks/tables. */
    markdown_core_open_block_func try_opening_paragraph;
    markdown_core_match_block_func last_block_matches;
    /* The speculative form of `last_block_matches` a block-start lookahead asks
     * (see the typedef); required of an extension whose blocks contain blocks. */
    markdown_core_continues_block_func continues_block;
    markdown_core_open_block_func try_opening_block;
    /* Non-consuming recognition before this extension's block-opening slot.
     * Shares the producer's grammar; may report allocation failure, but never
     * opens a node or claims source. */
    markdown_core_probe_block_func probe_block;
    /* Parsed delimiter semantics are declared by their element. Pairing,
     * run classification and node construction stay in the shared engine. */
    markdown_core_delimiter_rule delimiter_rule;
    unsigned char delimiter_character;
    delimiter_rule_spec delimiter;
    /* Optional non-consuming token predicate. Text scanning consults the
     * parser's byte-indexed projection; all owners of a shared byte must
     * agree, otherwise that byte always reaches ordinary extension dispatch. */
    bool (*is_inline_start)(markdown_core_inline_parser *, bufsize_t);
    markdown_core_match_inline_func match_inline;
    markdown_core_inline_from_delim_func insert_inline_from_delim;
    /* THREE byte sets, not one list.
     *
     * `special_inline_chars` was a single `llist` read by five consumers that
     * each meant something different by it: two byte tables were folded out of
     * it, `try_extensions` used it for cursor dispatch,
     * `get_extension_for_special_char` used it for delimiter-tag OWNERSHIP,
     * `bracket_takes_close_bracket` used it for `]` arbitration, and
     * `handle_backslash` used it to disable a core optimisation. One list
     * cannot say three different things, and D1 and D2 are what happens when it
     * tries: `set_emphasis` folded every byte an extension named into
     * `skip_chars`, which killed CommonMark flanking merely by attaching the
     * extension, and `'}'` sat in the list dispatching to nothing.
     *
     * Each set is a NUL-terminated byte list; NUL itself is never a member
     * because source normalization replaces it before inlines run. A NULL set
     * is empty. */
    const char *terminates_text;      /* ends a text run: subject_find_special_char */
    const char *dispatch;             /* offered to match_inline, and owns a delimiter tag */
    const char *flanking_transparent; /* scan_delims looks through it */
    const char *name;
    markdown_core_get_type_string_func get_type_string_func;
    markdown_core_can_contain_func can_contain_func;
    markdown_core_contains_inlines_func contains_inlines_func;
    markdown_core_accepts_lines_func accepts_lines_func;
    markdown_core_postprocess_func postprocess_func;
    markdown_core_opaque_alloc_func opaque_alloc_func;
    markdown_core_opaque_free_func opaque_free_func;
    markdown_core_visit_owned_subtrees_func visit_owned_subtrees_func;
};

#endif
