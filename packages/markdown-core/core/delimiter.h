#ifndef MARKDOWN_CORE_DELIMITER_H
#define MARKDOWN_CORE_DELIMITER_H

#include "markdown-core.h"
#include "markdown-core-element-api.h"

/* Private to the inline engine. Elements receive read-only marker views
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

/* Run spelling, pair ambiguity and body grammar are independent rule
 * properties. Every parsed body uses the same matcher and constructor;
 * opaque bodies retain their element's literal decoder. */
typedef enum { DELIMITER_INLINE_BODY, DELIMITER_WORD_BODY } delimiter_body;
typedef struct {
    bufsize_t minimum_width, maximum_width;
    bufsize_t run_limit; /* zero means a maximal run */
    bool rule_of_three;
    bool punctuation_bound;
    delimiter_body body;
    markdown_core_node_type single_kind, double_kind;
    bool exact_run;
} delimiter_rule_spec;

struct delimiter {
    struct delimiter *previous;
    struct delimiter *next;
    /* Borrowed marker Text or field owner; NULL for a content boundary. */
    markdown_core_node *node;
    /** The element that pushed it, or NULL for a core rule. One load. */
    const markdown_core_element *owner;
    bufsize_t position;
    delimiter_kind kind;
    bufsize_t length;
    markdown_core_delimiter_rule rule;
    int can_open;
    int can_close;
};

#endif
