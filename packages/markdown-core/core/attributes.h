#ifndef MARKDOWN_CORE_ATTRIBUTES_H
#define MARKDOWN_CORE_ATTRIBUTES_H

#include "chunk.h"

typedef struct {
    markdown_core_chunk name;
    markdown_core_chunk value;
} markdown_core_record;

/* One normalized value. Empty anchor bytes mean no anchor. The vectors retain
 * every occurrence; capacity is private construction state, never semantics. */
typedef struct markdown_core_attribute_value {
    markdown_core_chunk anchor;
    markdown_core_chunk *classes;
    size_t class_count, class_capacity;
    markdown_core_record *records;
    size_t record_count, record_capacity;
} markdown_core_attributes;

/* An index belongs to one immutable input extent. It recognizes every suffix
 * once, so overlapping failed candidates cannot repeatedly scan that extent.
 * Values are allocated and decoded only after recognition succeeds. */
typedef struct {
    markdown_core_mem *mem;
    const unsigned char *data;
    bufsize_t length;
    struct markdown_core_attribute_suffix {
        bufsize_t end;
        bufsize_t assignment_end;
    } *ends;
    size_t work;
    int oom;
} markdown_core_attribute_parser;

void markdown_core_attributes_free(markdown_core_mem *mem, markdown_core_attributes *value);
void markdown_core_attribute_parser_free(markdown_core_attribute_parser *parser);
/* Recognition only: no values are decoded until the owner commits. Zero
 * denotes a malformed candidate. The index is shared for the whole extent. */
bufsize_t markdown_core_attributes_end(markdown_core_attribute_parser *parser, bufsize_t start);
/* First complete container ending at `end`, outside escaped punctuation.
 * The caller selects the permitted line/extent. Returns -1 on failure. */
bufsize_t markdown_core_attributes_tail(markdown_core_attribute_parser *parser, bufsize_t start, bufsize_t end);
/* The caller supplies an empty result. Success transfers its allocations and
 * advances end; failure leaves both outputs untouched. Allocation failure is
 * sticky in parser->oom and never becomes a grammar fallback. */
int markdown_core_attributes_parse(markdown_core_attribute_parser *parser, bufsize_t start,
                                   markdown_core_attributes *result, bufsize_t *end);

#endif
