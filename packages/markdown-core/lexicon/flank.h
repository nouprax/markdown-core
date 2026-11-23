#ifndef MARKDOWN_CORE_LEXICON_FLANK_H
#define MARKDOWN_CORE_LEXICON_FLANK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* THE NEIGHBOURS OF A DELIMITER RUN: whether the character before it and
 * the one after it are Unicode white space or punctuation (a symbol counts
 * as punctuation), the start and the end of the input reading as a
 * newline. The engine finds them (inlines.c); a delimiter rule says what
 * they make of the run (delimiter.h, `flank`). */
typedef struct markdown_core_flank {
    bool space_before, space_after, punct_before, punct_after;
} markdown_core_flank;

/* What a run may do, as a rule's `flank` answers. */
enum { MARKDOWN_CORE_FLANK_OPENS = 1u, MARKDOWN_CORE_FLANK_CLOSES = 2u };

/* THE FLANKING RULES OF A DELIMITER RUN (CommonMark 0.31.2, 6.2), a lexical
 * rule several elements share. A run is left-flanking when white space does
 * not follow it and either punctuation does not follow it or white space or
 * punctuation precedes it; right-flanking the other way about. `free`: a
 * left-flanking run opens and a right-flanking one closes (`*`). `bound`:
 * a run flanking on both sides opens only after punctuation and closes only
 * before it (`_`). */
unsigned markdown_core_flank_free(markdown_core_flank flank);
unsigned markdown_core_flank_bound(markdown_core_flank flank);

#ifdef __cplusplus
}
#endif

#endif
