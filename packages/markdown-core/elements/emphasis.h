#ifndef MARKDOWN_CORE_ELEMENT_EMPHASIS_H
#define MARKDOWN_CORE_ELEMENT_EMPHASIS_H
#include "element.h"
#include "delimiter.h"
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_EMPHASIS;
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_EMPHASIS_UNDERSCORE;

/* THE FLANKING RULES OF A DELIMITER RUN (CommonMark 0.31.2, 6.2). A run is
 * left-flanking when white space does not follow it and either punctuation
 * does not follow it or white space or punctuation precedes it;
 * right-flanking the other way about. Emphasis provides its `*` rule as the
 * MARKDOWN_CORE_SERVICE_FLANK service (services.h), which the other
 * paired-run elements (strikethrough, mark, insertion) take. */
#endif
