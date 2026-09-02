#ifndef MARKDOWN_CORE_DELIMITER_H
#define MARKDOWN_CORE_DELIMITER_H

#include "markdown-core.h"
#include "markdown-core-extension-api.h"

/* The delimiter stack's element, PRIVATE TO CORE.
 *
 * It sat in `markdown-core-extension-api.h` under the comment "Exposed raw for
 * now" from 1.0 until Step 3, so every field was part of the extension surface
 * and none of them could change without an ABI break. The three extensions that
 * push delimiters read eight fields between them and write none, so the whole
 * exposure buys eight one-line accessors -- and buys back the freedom to change
 * the representation, which Step 8 needs.
 *
 * `owner` and `rule` are 3.3's; see the note beside the rule enum for what the
 * byte they replaced was doing. */
struct delimiter {
    struct delimiter *previous;
    struct delimiter *next;
    markdown_core_node *inl_text;
    /** The extension that pushed it, or NULL for a core rule. One load. */
    const markdown_core_extension *owner;
    bufsize_t position;
    bufsize_t length;
    markdown_core_delimiter_rule rule;
    int can_open;
    int can_close;
};

#endif
