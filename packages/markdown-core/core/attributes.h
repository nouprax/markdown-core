#ifndef MARKDOWN_CORE_ATTRIBUTES_H
#define MARKDOWN_CORE_ATTRIBUTES_H

#include <stdint.h>
#include "chunk.h"
#include "diagnostics.h"
#include "map.h"

typedef struct {
    markdown_core_chunk name;
    markdown_core_chunk value;
} markdown_core_record;

/* One normalized value. Empty anchor bytes mean no anchor. The vectors retain
 * every occurrence; capacity is private construction state, never semantics. */
typedef struct markdown_core_attribute_value {
    markdown_core_chunk anchor;
    markdown_core_chunk *classes;
    markdown_core_record *records;
    /* Counts and capacities are bounded by the extent they were read from,
     * which is a `bufsize_t`; the accessors still answer in `size_t`. */
    uint32_t class_count, class_capacity;
    uint32_t record_count, record_capacity;
    /* The vectors were taken from a parse transaction's arena and go with the
     * document: releasing the value walks them for what the chunks own and
     * hands neither vector back to the allocator. */
    uint32_t borrowed;
} markdown_core_attributes;

/* Demand-driven facts belong to one immutable input extent. Only queried
 * member suffixes, value joins and braces encountered inside a value need
 * records; ordinary source bytes never allocate index entries. */
typedef struct {
    markdown_core_mem *mem;
    /* The parse transaction's arena, when there is one: the normalized bytes
     * of every value are taken from it and the chunks borrow them, so a
     * parse costs no allocation per attribute and the tree releases none.
     * NULL outside a transaction, where each value is the allocator's. */
    struct markdown_core_arena *store;
    const unsigned char *data;
    bufsize_t length;
    /* Facts recorded so far, whether or not they are indexed yet. */
    uint32_t fact_count;
    markdown_core_key_index facts;
    struct markdown_core_attribute_arena *arena;
    /* The one buffer the values that need decoding are decoded through, kept
     * for the parser's life: a value's bytes are copied out of it before the
     * next is read, so a parser that meets an escape or an entity takes one
     * buffer for all of them, and one that meets neither takes none. */
    markdown_core_strbuf decoded;
#if MARKDOWN_CORE_DIAGNOSTICS
    size_t work;
#endif
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
