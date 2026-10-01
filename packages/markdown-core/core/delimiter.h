#ifndef MARKDOWN_CORE_DELIMITER_H
#define MARKDOWN_CORE_DELIMITER_H

#include "node_type.h"
#include "buffer.h"
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

/* AN INLINE CONTAINER BEING PARSED IS A BUILDER (5.11: open blocks are
 * builders). Its top-level inline nodes are a list of items in source order,
 * so a closer can wrap the nodes between its opener and itself, and a bracket
 * can take the nodes after its opener, in O(1) each; the nodes inside a node
 * an item holds are already that node's children tree. When the run ends its
 * nodes become the container's children, in order. An item holds its node's
 * reference. */
struct markdown_core_inline_item {
    markdown_core_node *node;
    struct markdown_core_inline_item *prev, *next;
};

struct delimiter {
    struct delimiter *previous;
    struct delimiter *next;
    /* The marker Text's or field owner's item; NULL for a content boundary. */
    markdown_core_inline_item *item;
    /** The instance of the element that pushed it, or NULL for a core rule.
     *  One load. */
    const markdown_core_element_instance *owner;
    bufsize_t position;
    delimiter_kind kind;
    bufsize_t length;
    markdown_core_delimiter_rule rule;
    int can_open;
    int can_close;
};

#endif
