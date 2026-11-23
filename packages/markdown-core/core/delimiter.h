#ifndef MARKDOWN_CORE_DELIMITER_H
#define MARKDOWN_CORE_DELIMITER_H

#include "node_type.h"
#include "buffer.h"
#include "markdown-core-element-api.h"
#include "stack.h"

/* THE NEIGHBOURS OF A DELIMITER RUN: whether the character before it and
 * the one after it are Unicode white space or punctuation (a symbol counts
 * as punctuation), the start and the end of the input reading as a
 * newline. The engine finds them (inlines.c); what they make of the run is
 * the grammar of the element whose rule the run is (`flank` below). */
typedef struct markdown_core_flank {
    bool space_before, space_after, punct_before, punct_after;
} markdown_core_flank;

/* What a run may do, as a rule's `flank` answers. */
enum { MARKDOWN_CORE_FLANK_OPENS = 1u, MARKDOWN_CORE_FLANK_CLOSES = 2u };

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
    /* What the run's neighbours make of a run of an inline body, the
     * element's grammar: whether it opens and whether it closes. NULL: it
     * does neither. */
    unsigned (*flank)(const markdown_core_element_instance *self, markdown_core_flank);
    delimiter_body body;
    markdown_core_node_type single_kind, double_kind;
    bool exact_run;
} delimiter_rule_spec;

/* An item of the run's delimiter stack (stack.h). */
struct delimiter {
    markdown_core_stack_item item;
    /* Borrowed marker Text or field owner; NULL for a content boundary. */
    markdown_core_node *node;
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
MARKDOWN_CORE_STACK_OF(delimiter, delimiter)

#endif
