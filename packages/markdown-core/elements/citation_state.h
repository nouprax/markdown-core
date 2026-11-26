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

/* A brace of a run: where it starts, where it closes, the brace around it,
 * whether it may be a key and holds anything, and whether a code span or a
 * comment in it is cut short, which may close past its `}` once the input
 * goes on. */
typedef struct {
    bufsize_t start, end, previous;
    bool valid, content, cut;
} citation_brace;

/* The index's code spans read ahead of the run's own, from backtick runs the
 * run may not take as openers, so they keep a memo of their own
 * (services.h, MARKDOWN_CORE_SERVICE_CODE_SPANS). */
typedef struct {
    citation_brace *entries;
    size_t count, cursor;
    markdown_core_skip_memo ticks;
    bool ready;
} citation_brace_index;

/* ONE RUN'S CITATIONS (the citation element's run record): the run's
 * brace index, built the first time a braced key is read. */
typedef struct markdown_core_citation_entry markdown_core_citation_entry;
typedef struct {
    citation_brace_index braces;
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
 * the bytes and entries its scans visited, and the capacity reserved for
 * brace records, for its complexity gates. */
typedef struct {
    size_t work, brace_bytes;
} markdown_core_citation_work;

#endif
