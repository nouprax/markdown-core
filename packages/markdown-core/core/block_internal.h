#ifndef MARKDOWN_CORE_BLOCK_INTERNAL_H
#define MARKDOWN_CORE_BLOCK_INTERNAL_H
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "markdown_core_ctype.h"
#include "utf8.h"
#include "inlines.h"
#include "element.h"
#include "tokenizer.h"
#include "compile.h"
#define CODE_INDENT 4
#define TAB_STOP 4
#ifndef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif

uint64_t markdown_core_source_key(const void *entry);
bool markdown_core_block_last_line_blank(const markdown_core_node *node);
markdown_core_node_type markdown_core_block_type(const markdown_core_node *node);
void markdown_core_block_set_end_to_current_line(markdown_core_parser *parser, markdown_core_node *b);
void markdown_core_block_rebase_content_marks(markdown_core_parser *parser, markdown_core_node *node, bufsize_t cut,
                                              bufsize_t remaining);
/* Whether `node`, a closed block, ends with a blank line (blocks.c). */
bool markdown_core_block_ends_with_blank_line(const markdown_core_node *node);
/* Closes the open blocks from the current one down to `b`, which is open
 * above the active input's root, each ending where the line in hand says;
 * returns the block that is current after them. */
markdown_core_node *markdown_core_block_close(markdown_core_parser *parser, markdown_core_node *b);
/* `node`, a block made whole, its own children settled, settles into
 * `parent`, which has no open or pending child: it joins the end of the
 * parent's children and the completion queue (blocks.c, A BLOCK SETTLES). */
void markdown_core_block_settle(markdown_core_parser *parser, markdown_core_node *node, markdown_core_node *parent);
/* The same before the last of `parent`'s children: the block it was read
 * from as that closes, or the block it was split from. */
void markdown_core_block_settle_before(markdown_core_parser *parser, markdown_core_node *node,
                                       markdown_core_node *parent);
/* `field`, a field root `holder` holds -- a callout's title, a definition's
 * term, a block directive's label, a table's caption -- settles as `holder`
 * installs it, once the line that writes it is read: it holds as a settled
 * block does from then on, and joins the completion queue with `holder`, to
 * complete on its own (blocks.c, A BLOCK SETTLES). */
void markdown_core_block_settle_field(markdown_core_parser *parser, markdown_core_node *field,
                                      markdown_core_node *holder);
/* The current block, its range settled, closes and becomes the pending child
 * of the block below it. */
void markdown_core_block_close_current(markdown_core_parser *parser);
typedef struct markdown_core_block_start_context {
    markdown_core_node *container;
    /* The line, at its first non-space byte. */
    markdown_core_reader *reader;
    /* `paragraph`: the container is a paragraph the line would continue.
     * `lazy`: the line missed the current block's prefix, and that block
     * would take it as text. Indented code, an HTML block of the seventh
     * kind, a dash-led table and the paragraph hooks open on neither, so such
     * a line stays text. Every other start reads `paragraph` alone, if
     * anything: a list that cannot interrupt a paragraph still opens on a
     * lazy line, as in cmark. */
    bool paragraph, lazy, all_matched;
    size_t depth;
} block_start_context;

/* WHAT THE START FOUND: the claiming start's own record of what its
 * construct read, which its token's handler builds from -- a list's
 * marker, a specimen's label, a callout's header -- in a layout only the
 * element knows. The construct writes it before the handler reads it, and
 * the engine never reads it. An element
 * names it as its type with MARKDOWN_CORE_BLOCK_FOUND, and holds at build
 * time that the type fits (MARKDOWN_CORE_BLOCK_FOUND_FITS). */
#define MARKDOWN_CORE_BLOCK_FOUND_SIZE 128
#define MARKDOWN_CORE_BLOCK_FOUND(start, type) ((type *)(void *)(start)->found.bytes)
/* C99 has no _Static_assert: an array of negative size fails the build. */
#define MARKDOWN_CORE_BLOCK_FOUND_FITS(type, name)                                                                     \
    typedef char name[sizeof(type) <= MARKDOWN_CORE_BLOCK_FOUND_SIZE ? 1 : -1]

typedef struct markdown_core_block_start {
    /* The start whose construct found it. */
    const markdown_core_element_instance *owner;
    markdown_core_node_type kind;
    bufsize_t matched;
    union {
        void *pointer;
        uint64_t whole;
        double real;
        unsigned char bytes[MARKDOWN_CORE_BLOCK_FOUND_SIZE];
    } found;
} block_start;
/* A TURN OF THE LINE: one block start tried on a line (element.h,
 * markdown_core_block_construct), or one open block's continuation
 * (markdown_core_line_ops `continuation`), the state its construct reads
 * through the tokenizer's reader and its token's handler through the
 * compiler's. A start reads the line in `context` and writes what it found
 * in `start`; the handler opens the block in `*container` and says in
 * `opened` whether it did. A continuation reads the line on `reader` for
 * the open block `node`, in a lookahead (`look`) about to add `joining`. */
typedef struct markdown_core_block_turn {
    markdown_core_parser *parser;
    block_start_context *context;
    block_start *start;
    markdown_core_node **container;
    bool opened;
    markdown_core_reader *reader;
    markdown_core_node *node;
    const markdown_core_node *joining;
    bool look;
    /* The item a front cut asks for (element.h, markdown_core_cut). */
    markdown_core_cut *cut;
    /* Where the line starts in the data the reading's tokens count in,
     * which the compile is handed: a token's bytes are at its `start`, and
     * on the line at `start - base`. */
    uint32_t base;
    /* Where the line's bytes a start read are the line's text again, or
     * UINT32_MAX: a handler that finds the start makes nothing of them (an
     * underline under a paragraph its items took whole) gives them back
     * (markdown_core_give_back), as a resolver makes a token text. */
    uint32_t gives;
} markdown_core_block_turn;

/* What a continuation said of the line: not continued, continued, or the
 * verdict its token gave (MARKDOWN_CORE_TOKEN_CLOSE, _PEND, _TAKE). */
enum { MARKDOWN_CORE_LINE_OFF = 0, MARKDOWN_CORE_LINE_ON = 1 };
/* The continuation's verdict on the line, `kind` (MARKDOWN_CORE_TOKEN_CLOSE,
 * _PEND, _TAKE), and it succeeds. */
markdown_core_state markdown_core_block_continued(markdown_core_tokenizer *tokenizer, uint16_t kind);
/* The continuation ends with `verdict`: MARKDOWN_CORE_LINE_OFF fails, _ON
 * succeeds, a verdict token's kind succeeds with that token. */
markdown_core_state markdown_core_block_verdict(markdown_core_tokenizer *tokenizer, int verdict);

/* THE INDENTED CONTINUATION (micromark's list item and footnote
 * continuations): the line goes on past `columns` columns of indentation,
 * its PREFIX, or, when `blank` lets it, on a blank line, past its spaces. */
markdown_core_state markdown_core_block_indented(markdown_core_tokenizer *tokenizer, uint32_t columns, bool blank,
                                                 int code);

static inline markdown_core_block_turn *markdown_core_block_turn_of(const markdown_core_tokenizer *tokenizer) {
    return (markdown_core_block_turn *)tokenizer->reader;
}
static inline markdown_core_block_turn *markdown_core_block_turned(const markdown_core_compiler *compiler) {
    return (markdown_core_block_turn *)compiler->reader;
}
/* The start found its block: one token of the element's block kind
 * `kind`, at the line's first non-space byte, and the construct succeeds. */
/* The core's handler of the line's text a block takes
 * (MARKDOWN_CORE_TOKEN_FLOW). */
extern const markdown_core_compile_ops MARKDOWN_CORE_FLOW_TOKEN;
markdown_core_state markdown_core_block_found(markdown_core_tokenizer *tokenizer, uint16_t kind);
/* The token kind of the element's block token `kind` for the construct
 * being tried, which enters and exits it itself. */
static inline uint16_t markdown_core_block_token(const markdown_core_tokenizer *tokenizer, int kind) {
    return (uint16_t)(((const markdown_core_element_instance *)markdown_core_self(tokenizer))->block_base + kind);
}
/* The same, the token reading the line's bytes to `end`, an offset on the
 * line, which the cursor does not move to: a block's text a later step of
 * the line reads from where the cursor stands. */
markdown_core_state markdown_core_block_span(markdown_core_tokenizer *tokenizer, uint16_t kind, bufsize_t end);

/* A READING OF ITS OWN (micromark's subtokenize, one chunk): `construct`,
 * its context the asker's, read over the `size` bytes at `data` from their
 * first, through `turn`, which the asker sets. `found` says whether it was, and `events` holds its tokens until
 * markdown_core_alone_end. False, with the parse failed, when storage could
 * not be had. */
typedef struct {
    /* What the construct reads the line through, the asker's to set. */
    markdown_core_block_turn turn;
    const markdown_core_construct *construct;
    struct markdown_core_pass *pass;
    const markdown_core_events *events;
    bool found;
} markdown_core_alone;
bool markdown_core_alone_read(markdown_core_parser *parser, markdown_core_alone *alone,
                              const markdown_core_construct *construct, const unsigned char *data, uint32_t size);
void markdown_core_alone_end(markdown_core_parser *parser, markdown_core_alone *alone);

/* THE ITEM'S PADDING (list.c): one to four columns of space after an item
 * marker, not followed by the line's end; refused, the reading reads one
 * column of space if there is one, padding of one column. */
extern const markdown_core_construct markdown_core_item_padding;
/* Adds the rest of the line in hand, from the reader's offset, to `node`'s content. */
void markdown_core_block_add_line(markdown_core_parser *parser, markdown_core_node *node);
markdown_core_node *markdown_core_block_parent_for(markdown_core_parser *parser, markdown_core_node *parent,
                                                   markdown_core_node_type kind);
/* Commit a parent selected by block_parent_for without repeating its policy. */
markdown_core_node *markdown_core_parser_add_child_validated(markdown_core_parser *parser, markdown_core_node *parent,
                                                             markdown_core_node_type kind, int start_column);
/* `child`, a block made for `parent`, which holds its kind, opens in it as
 * add_child_validated opens the block it makes: what is open inside the
 * container closes first, its pending child settles, and `child` is its last
 * child and the builder above it. NULL, with the parse failed, when the
 * children could not grow. */
markdown_core_node *markdown_core_parser_open_child(markdown_core_parser *parser, markdown_core_node *parent,
                                                    markdown_core_node *child);
#endif
