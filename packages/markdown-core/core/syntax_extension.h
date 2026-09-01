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
     * because the feed replaces it before inlines run. A NULL set is empty. */
    const char *terminates_text;      /* ends a text run: subject_find_special_char */
    const char *dispatch;             /* offered to match_inline, and owns a delimiter tag */
    const char *flanking_transparent; /* scan_delims looks through it */
    const char *name;
    markdown_core_get_type_string_func get_type_string_func;
    markdown_core_can_contain_func can_contain_func;
    markdown_core_contains_inlines_func contains_inlines_func;
    markdown_core_accepts_lines_func accepts_lines_func;
    markdown_core_postprocess_block_func postprocess_block_func;
    /* WHICH BLOCKS `postprocess_block_func` WANTS, by TYPE NAME -- the string
     * `markdown_core_node_get_type_string` answers, the one vocabulary the
     * core and an extension share: the core cannot name an extension's block
     * type, which lives in the extension's own header, but every block
     * answers a name. The same idiom as the byte sets above with the element
     * boundary written out, since here the element is itself a string: a
     * NUL-separated list, walked as `for (p = set; *p; p += strlen(p) + 1)`,
     * so each compare is a plain strcmp. THE SET ENDS WITH AN EMPTY NAME, so
     * the literal spells its last NUL out before the one the compiler adds --
     *
     *     .postprocess_blocks = "formula_block\0code_block\0paragraph\0",
     *
     * A byte set needs no such thing because one NUL ends it; a name set
     * without it walks off the end of the literal into whatever the linker
     * put next, which ASan caught on the first build. NULL means no per-block
     * hook.
     *
     * ONE MEMBER IS NOT A NAME. `"*inlines"` selects every block the parser's
     * own `contains_inlines` answers true for, and `*` cannot begin a type
     * name. The two kinds of member say two different things about what the
     * hook DOES, and the projection cache (T9, #161) acts on the difference:
     * `"*inlines"` declares a pass over the block's INLINE CONTENT; a NAME
     * declares a pass over the block NODE -- it may replace or remove it.
     * Either kind runs on every FRESH projection of its block. A DERIVE hit
     * runs neither: the retained node itself is the answer, the hook's
     * node-level effect baked in (D9), and the node it would be offered is
     * frozen for every tree at once. The SEAL (`finish`) runs the NAME
     * hooks once more per stored block: it hands back the CST shell rather
     * than the retained node, so the hook reproduces its node-level effect
     * there -- the children stay the stored list, every node of it frozen.
     * A hook that REPLACES the block keeps it out of the store, so a
     * replacing hook runs on every projection.
     *
     * `contains_inlines_func` is consulted at node construction and at
     * each validated type or descriptor change, and its answer is FROZEN
     * into the node between those points; the engine never re-asks it in
     * steady state, so a stateful answer cannot classify a block one way
     * at adoption and another at projection, and derive and seal always
     * agree. Per-projection side effects on a block the cache retains are
     * not part of this contract. */
    const char *postprocess_blocks;
    markdown_core_close_block_func close_block_func;
    markdown_core_opaque_alloc_func opaque_alloc_func;
    markdown_core_opaque_free_func opaque_free_func;
    markdown_core_opaque_copy_func opaque_copy_func;
};

#endif
