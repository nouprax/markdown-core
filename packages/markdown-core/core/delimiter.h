#ifndef MARKDOWN_CORE_DELIMITER_H
#define MARKDOWN_CORE_DELIMITER_H

#include "markdown-core.h"
#include "markdown-core-extension-api.h"

/* Private to the inline engine. Extensions receive read-only marker views
 * and construct opaque AST values; stack ordering and reduction belong here. */
/* Stack events share source order and lifetime. Only MARKER entries take
 * part in pairing; boundaries constrain content and fields suspend token
 * completion until their independently owned inline trees have been parsed. */
typedef enum {
    DELIMITER_MARKER,
    DELIMITER_BOUNDARY,
    DELIMITER_FIELD,
    DELIMITER_CITATION_TOKEN,
    DELIMITER_AFFIX_BOUNDARY
} delimiter_kind;

struct delimiter {
    struct delimiter *previous;
    struct delimiter *next;
    /* Borrowed marker Text or field owner; NULL for a content boundary. */
    markdown_core_node *node;
    /** The extension that pushed it, or NULL for a core rule. One load. */
    const markdown_core_extension *owner;
    bufsize_t position;
    delimiter_kind kind;
    bufsize_t length;
    markdown_core_delimiter_rule rule;
    int can_open;
    int can_close;
};

#endif
