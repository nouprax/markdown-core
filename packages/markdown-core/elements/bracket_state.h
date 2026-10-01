#ifndef MARKDOWN_CORE_BRACKET_STATE_H
#define MARKDOWN_CORE_BRACKET_STATE_H
#include "delimiter.h"
#include "inline_internal.h"
#include "citation_state.h"

typedef enum { BRACKET_UNMATCHED, BRACKET_MATCHED, BRACKET_REJECTED } markdown_core_bracket_match;

typedef enum { BRACKET_LINK, BRACKET_IMAGE, BRACKET_FOOTNOTE } bracket_kind;

typedef struct bracket {
    struct bracket *previous;
    /* The item of the opener's literal text in the run. */
    markdown_core_inline_item *inl_text;
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
    markdown_core_inline_item *close_text;
    delimiter *delim_end;
    bufsize_t close_position;
    bool pending_no_link_openers;
    struct bracket *pending_previous, *pending_next;
} bracket;

/* ONE RUN'S BRACKETS (the link element's run record): the open
 * brackets, innermost first; the brackets whose citation tail waits for its
 * key's owner; and whether a link may open here, which a link's own text
 * forbids until a new opener. Every bracket grammar -- link, image, span,
 * footnote, citation -- reads and writes this one record, through the
 * bracket algorithm's calls below and in link.h. */
typedef struct {
    bracket *last;
    bracket *pending;
    bool no_link_openers;
} markdown_core_bracket_scope;

/* THE BRACKET ALGORITHM'S WORK (the link element's parse record):
 * closers read and children moved into a bracket's owner, for its gate. */
typedef struct {
    size_t work;
} markdown_core_bracket_work;

/* The run's brackets: the run record of `link`, the link element's
 * instance, which every bracket grammar reaches as its link peer. */
static inline markdown_core_bracket_scope *markdown_core_brackets(const markdown_core_element_instance *link,
                                                                  const markdown_core_inline_state *inline_state) {
    return (markdown_core_bracket_scope *)markdown_core_run_state(inline_state, link);
}

/* The innermost open bracket, or NULL outside every bracket -- and in a
 * dialect without links (`link` NULL), which reads none. */
static inline bracket *markdown_core_open_bracket(const markdown_core_element_instance *link,
                                                  const markdown_core_inline_state *inline_state) {
    return link ? markdown_core_brackets(link, inline_state)->last : NULL;
}

/* What a bare token scanner may ask of the brackets around it (link.c), with
 * `link` its link peer: NULL, in a dialect without links, has no brackets. */
/** The surrounding bracket's closing byte, or zero outside brackets.
 * Bare token scanners preserve an unescaped closer for the shared algorithm. */
unsigned char markdown_core_inline_state_closing_bracket(const markdown_core_element_instance *link,
                                                         markdown_core_inline_state *inline_state);
/** The start of the current independent inline body. */
int markdown_core_inline_state_context_start(const markdown_core_element_instance *link,
                                             markdown_core_inline_state *inline_state);
/** Returns 1 if the inline state is currently in a bracket; pass 1 for 'image'
 * if you want to know about an image-type bracket, 0 for link-type. */
int markdown_core_inline_state_in_bracket(const markdown_core_element_instance *link,
                                          markdown_core_inline_state *inline_state, int image);

#endif
