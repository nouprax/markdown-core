#ifndef MARKDOWN_CORE_ELEMENT_ATTRIBUTES_H
#define MARKDOWN_CORE_ELEMENT_ATTRIBUTES_H
#include "inlines.h"
#include "../core/attributes.h"

/* THE ATTRIBUTE GRAMMAR, the attribute element's: its recognizer and its
 * decoder, which the elements that declare it a peer ask (element.h,
 * `peers`). */
/* THE WORKSPACE A CONTAINER IS READ INTO before it is laid out as a value
 * (`markdown_core_attributes_parse`): its decoded strings, each
 * NUL-terminated, and each class and record as offsets into them, in source
 * order. A parse call clears it, fills it and lays it out, and leaves nothing
 * in it that the next call reads; parse calls do not nest. So one workspace
 * serves every reader of a parse transaction, nested extents included: the
 * transaction owns it (parser.h), it grows to the largest container the
 * document holds, and the value's one allocation is made at its exact size.
 * Valid zeroed; what it needs is established on first use. */
typedef struct markdown_core_attribute_scratch {
    markdown_core_strbuf strings;
    struct markdown_core_attribute_member {
        bufsize_t name, name_length; /* name < 0 for a class */
        bufsize_t value, value_length;
    } *members;
    size_t member_count, member_capacity;
    /* The bytes every recogniser that borrowed this workspace examined, each
     * added once as that recogniser is released, for the attribute grammar's
     * complexity gate. */
    size_t work;
} markdown_core_attribute_scratch;

/* A recogniser belongs to one immutable input extent. It walks forward from
 * each candidate brace it is asked about and memoises, by position, the
 * answer for every member boundary and every `=` it passes, so overlapping
 * candidates cannot repeatedly scan that extent (elements/attributes.c states
 * the memo's encoding and why its walks tile the extent). Values are decoded
 * only after recognition succeeds, into `scratch`, which the recogniser
 * borrows from whoever owns the parse: required by
 * `markdown_core_attributes_parse`, never touched by recognition. */
typedef struct {
    const unsigned char *data;
    bufsize_t length;
    struct markdown_core_attribute_suffix {
        bufsize_t end;
        bufsize_t assignment_end;
    } *ends;
    markdown_core_attribute_scratch *scratch;
    size_t work;
    int oom;
    /* Whether a quote's lookahead met the extent's end with no closer: what
     * the walks answered may change once the input goes on. */
    int ran_out;
} markdown_core_attribute_parser;

void markdown_core_attribute_parser_free(markdown_core_attribute_parser *parser);
void markdown_core_attribute_scratch_free(markdown_core_attribute_scratch *scratch);
/* Recognition only: no values are decoded until the owner commits. Zero
 * denotes a malformed candidate. The memo is shared for the whole extent. */
bufsize_t markdown_core_attributes_end(markdown_core_attribute_parser *parser, bufsize_t start);
/* First complete container ending at `end`, outside escaped punctuation.
 * The caller selects the permitted line/extent. Returns -1 on failure. */
bufsize_t markdown_core_attributes_tail(markdown_core_attribute_parser *parser, bufsize_t start, bufsize_t end);
/* The caller supplies an empty result. Success transfers its allocations and
 * advances end; failure leaves both outputs untouched. Allocation failure is
 * sticky in parser->oom and never becomes a grammar fallback. */
int markdown_core_attributes_parse(markdown_core_attribute_parser *parser, bufsize_t start,
                                   markdown_core_attributes *result, bufsize_t *end);

/* THE PARTS OF AN ATTRIBUTE BLOCK (parts.h) in a node of a kind of a peer:
 * those of the container whose `{` is at `brace`, one past its `}`, or
 * `brace` when none closes there; and those of the container that ends a
 * block's line at `end`, but for spaces and tabs, read after `from` as the
 * block's tail was taken (core/element.h, markdown_core_follow): its `{`,
 * or the end of the line's other bytes when it has none. */
struct markdown_core_parts;
bufsize_t markdown_core_attributes_parts(struct markdown_core_parts *parts, bufsize_t brace);
bufsize_t markdown_core_attributes_tail_parts(struct markdown_core_parts *parts, bufsize_t from, bufsize_t end);
/* The workspace a recognizer of the parse borrows: the attribute element's
 * (`scratch`). */
markdown_core_attribute_scratch *markdown_core_attributes_scratch(const markdown_core_element_instance *self);
struct markdown_core_field;
/* THE SYNTAX QUESTION A PEER ASKS (element.h, `peers`): where an attribute
 * block at content offset `start` of a construct of kind `owner` closes,
 * one past its `}`, or 0 when none closes there. The block's value is the
 * attribute element's to write: it updates the node the construct makes
 * (no-flush.md 10.13). */
bufsize_t markdown_core_attributes_closes(const markdown_core_element_instance *self,
                                          markdown_core_inline_state *inline_state, bufsize_t start,
                                          markdown_core_node_kind owner);
/* What keeps the members of the many braces of one stretch linear: zeroed
 * before the first brace, freed after the last. */
typedef struct markdown_core_member_memo {
    bufsize_t *walks, *values;
    struct markdown_core_field *found;
    size_t count, room, asked;
    bool refused;
} markdown_core_member_memo;
void markdown_core_member_memo_free(markdown_core_member_memo *memo);
/* THE MEMBER AN ATTRIBUTE BLOCK THAT DOES NOT CLOSE IS IN AT ITS END, the
 * block's `{` at `start` of the `n` bytes at `s`: an `#` identifier, a `.`
 * class, a record's name, or its value after the name its prefix holds; a
 * member boundary reads as a name not yet begun. False when the block is no
 * block whatever follows. The field's owner is left to the caller. A memo,
 * or NULL, is shared by the braces asked about on the same `n` bytes. */
bool markdown_core_attributes_open_member(const unsigned char *s, bufsize_t n, bufsize_t start,
                                          struct markdown_core_field *field, markdown_core_member_memo *memo);
/* Where a member's name, read from `data`, ends. */
bufsize_t markdown_core_attributes_name_end(const unsigned char *data, bufsize_t length);
/* The attribute tails a block of kind `owner` may open on the line being
 * read, `line`, at or after `from`, that the line's end at `end` leaves
 * open, to the parse's context sink: every brace that may be one, since
 * which is the tail only the finished line tells; whether there is one. */
bool markdown_core_attributes_report(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                     markdown_core_node_kind owner, const unsigned char *line, bufsize_t from,
                                     bufsize_t end);
/* A PEER'S SYNTAX QUESTION OF A BLOCK LINE: where the attribute block at
 * `start` of the `length` bytes at `data` closes, one past its `}`, or 0
 * when none closes there. What it recognized stays in `held`, zeroed before
 * the question, which the asker keeps: the block's value is the attribute
 * element's to write into the block the line opens
 * (markdown_core_attributes_write), from what it recognized, and `held` is
 * let go either way (markdown_core_attributes_release). */
bufsize_t markdown_core_attributes_close_at(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                            markdown_core_attribute_parser *held, const unsigned char *data,
                                            bufsize_t length, bufsize_t start);
bool markdown_core_attributes_write(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                    markdown_core_attribute_parser *held, bufsize_t start, markdown_core_node *node);
void markdown_core_attributes_release(const markdown_core_element_instance *self, markdown_core_attribute_parser *held);
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_ATTRIBUTES;

#endif
