#ifndef MARKDOWN_CORE_ELEMENT_EMPHASIS_H
#define MARKDOWN_CORE_ELEMENT_EMPHASIS_H
#include "element.h"
#include "delimiter.h"
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_EMPHASIS;
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_EMPHASIS_UNDERSCORE;

/* THE FLANKING RULES OF A DELIMITER RUN (CommonMark 0.31.2, 6.2), the
 * emphasis grammar the other paired-run elements (strikethrough, mark,
 * insertion) take as well. A run is left-flanking when white space does not
 * follow it and either punctuation does not follow it or white space or
 * punctuation precedes it; right-flanking the other way about. `free`: a
 * left-flanking run opens and a right-flanking one closes (`*`). `bound`: a
 * run flanking on both sides opens only after punctuation and closes only
 * before it (`_`). */
unsigned markdown_core_emphasis_free(markdown_core_flank flank);
unsigned markdown_core_emphasis_bound(markdown_core_flank flank);
#endif
