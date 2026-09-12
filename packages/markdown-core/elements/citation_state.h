#ifndef MARKDOWN_CORE_CITATION_STATE_H
#define MARKDOWN_CORE_CITATION_STATE_H
#include "delimiter.h"
/* Citation candidates borrow token nodes until the enclosing bracket chooses
 * their owner. They retain source coordinates, never a second inline tree. */
typedef struct citation_token {
    struct citation_token *next;
    markdown_core_node *node;
    delimiter *boundary;
    struct bracket *tail;
    bufsize_t start, key_start, key_end, end, tail_start;
    bool key, suppress;
} citation_token;

typedef struct {
    citation_token *first, *last;
} citation_tokens;

typedef struct {
    bufsize_t start, end, previous;
    bool valid, content;
} citation_brace;

typedef struct {
    citation_brace *entries;
    size_t count, cursor;
    bool ready;
} citation_brace_index;

#endif
