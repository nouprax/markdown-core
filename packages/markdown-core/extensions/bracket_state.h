#ifndef MARKDOWN_CORE_BRACKET_STATE_H
#define MARKDOWN_CORE_BRACKET_STATE_H
#include "delimiter.h"
#include "citation_state.h"
typedef enum { BRACKET_UNMATCHED, BRACKET_MATCHED, BRACKET_REJECTED } markdown_core_bracket_match;

typedef enum { BRACKET_LINK, BRACKET_IMAGE, BRACKET_FOOTNOTE } bracket_kind;

typedef struct bracket {
    struct bracket *previous;
    markdown_core_node *inl_text;
    bufsize_t position;
    /* Last pipe read as ordinary text at this bracket's depth. Opaque tokens,
     * escapes and nested brackets never update the enclosing image. */
    bufsize_t image_pipe;
    bracket_kind kind;
    bool outer_no_link_openers;
    bool active;
    bool bracket_after;
    bool in_bracket_image0;
    bool in_bracket_image1;
    citation_tokens citations;
    citation_token *author;
    /* A tail waits for its key's enclosing owner. All token nodes stay in the
     * AST; this parser-owned continuation borrows the exact bounded range. */
    markdown_core_node *close_text;
    delimiter *delim_end;
    bufsize_t close_position;
    bool pending_no_link_openers;
    struct bracket *pending_previous, *pending_next;
} bracket;

#endif
