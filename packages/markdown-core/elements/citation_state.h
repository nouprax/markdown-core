#ifndef MARKDOWN_CORE_CITATION_STATE_H
#define MARKDOWN_CORE_CITATION_STATE_H
#include "services.h"
#include "inline_internal.h"

/* A key as its bytes spell it: the token's bytes, the key's inside them,
 * and whether it is suppressed (`-@key`). */
typedef struct citation_token {
    bufsize_t start, key_start, key_end, end;
    bool key, suppress;
} citation_token;

/* A BRACE a braced key's reading read (citation.c, THE BRACED KEY): where
 * it starts, where its key ends past its `}`, 0 when it is no key or no
 * `}` closes it, the brace it is in (UINT32_MAX for none), whether no
 * white space or faulty brace in it makes it no key so far, and whether it
 * holds anything. */
typedef struct {
    uint32_t start, end, previous;
    bool valid, content;
} citation_brace;

/* What the readings of a run's braced keys read, every brace they read in
 * the order of their starts, so that a key in a brace read before is not
 * read again: `count` of them, room for `room`. */
typedef struct {
    citation_brace *entries;
    size_t count, room;
} citation_brace_index;

/* ONE RUN'S CITATIONS (the citation element's run record): the braces read,
 * and where the braced key being read starts, its `{`, and where its `}`
 * ends it (citation.c, THE BRACED KEY). */
typedef struct markdown_core_citation_entry markdown_core_citation_entry;
typedef struct {
    citation_brace_index braces;
    uint32_t key_at, brace_at, brace_end;
    /* The reading's (citation.c, THE CITATION'S TEXT): whether it read a
     * token outside every label; the label whose end is read again and
     * the author it is read with (markdown_core_label_again); and, as it
     * compiles, the items of the bibliographies open there. */
    bool held, again;
    uint32_t label, author;
    markdown_core_citation_entry *entries;
    size_t entry_count, entry_room, base;
} markdown_core_citation_run;

/* THE CITATION GRAMMAR'S WORK (the citation element's parse record):
 * the bytes and entries its readings visited, and the capacity reserved
 * for brace records, for its complexity gates. */
typedef struct {
    size_t work, brace_bytes;
} markdown_core_citation_work;

#endif
