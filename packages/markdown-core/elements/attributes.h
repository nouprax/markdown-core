#ifndef MARKDOWN_CORE_ELEMENT_ATTRIBUTES_H
#define MARKDOWN_CORE_ELEMENT_ATTRIBUTES_H
#include "inlines.h"
#include "../core/attributes.h"
#include "services.h"

/* THE ATTRIBUTE GRAMMAR, the attribute element's: its recognizer and its
 * decoder, which other elements ask through the service it provides
 * (services.h, MARKDOWN_CORE_SERVICE_ATTRIBUTES), and the engine's own
 * readers and tests here. */
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
    /* Where the last parse read each member, in the parsed bytes, as the
     * field it is (markdown_core_attributes_fields). */
    markdown_core_field_spot *places;
    size_t place_count, place_capacity;
    /* The bytes every recogniser that borrowed this workspace examined, each
     * added once as that recogniser is released, for the attribute grammar's
     * complexity gate. */
    size_t work;
    /* The decoder of the references a quoted value decodes (services.h,
     * MARKDOWN_CORE_SERVICE_REFERENCES): set by
     * `markdown_core_attributes_scratch`, none in a workspace of no parse,
     * which decodes none. */
    markdown_core_need references;
} markdown_core_attribute_scratch;

/* A recogniser (services.h, markdown_core_attribute_parser) memoises, by
 * position, the answer for every member boundary and every `=` it passes:
 * its `ends`, one per byte of its extent. */
struct markdown_core_attribute_suffix {
    bufsize_t end;
    bufsize_t assignment_end;
};

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
/* The members the last parse read, as fields where `to` has them go
 * (markdown_core_field_to), in the parsed bytes' offsets. */
void markdown_core_attributes_fields(const markdown_core_attribute_parser *parser, const markdown_core_field_to *to);

/* The workspace a recognizer of the parse borrows: the attribute element's
 * (`scratch`). */
markdown_core_attribute_scratch *markdown_core_attributes_scratch(const markdown_core_element_instance *self);
struct markdown_core_field;
/* THE SERVICE'S `closes` (services.h): where an attribute
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
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_ATTRIBUTES;

#endif
