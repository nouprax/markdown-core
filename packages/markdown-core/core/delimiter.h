#ifndef MARKDOWN_CORE_DELIMITER_H
#define MARKDOWN_CORE_DELIMITER_H

#include "node_type.h"
#include "buffer.h"
#include "markdown-core-element-api.h"

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

#endif
