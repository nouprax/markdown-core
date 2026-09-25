#ifndef MARKDOWN_CORE_ATTRIBUTES_H
#define MARKDOWN_CORE_ATTRIBUTES_H

#include <stdint.h>

#include "buffer.h"
#include "chunk.h"

typedef struct {
    markdown_core_chunk name;
    markdown_core_chunk value;
} markdown_core_record;

struct markdown_core_resource;

/* One normalized value. Empty anchor bytes mean no anchor. The lists retain
 * every occurrence.
 *
 * A VALUE IS ONE ALLOCATION. Its records, its classes and every string they
 * and the anchor name live in `storage`, laid out once the whole container
 * has been read, and they begin and end with the value: no chunk in it owns
 * anything of its own. The anchor is the one member a consumer may replace
 * with a string it made (a chunk with `alloc` set), and the release frees
 * such an anchor beside `storage`. Every string in `storage` is
 * NUL-terminated.
 *
 * An anchor the parser computed for a heading that declares an implicit
 * reference is instead the bytes after the `#` of that reference's
 * destination (node.h), and the value holds the resource they belong to in
 * `anchor_owner`. The hold is the value's, like `storage`, so the anchor lives
 * exactly as long as the value does, whatever kind the node carrying it is. */
typedef struct markdown_core_attribute_value {
    markdown_core_chunk anchor;
    markdown_core_chunk *classes;
    markdown_core_record *records;
    /* Each class and record is at least one byte of its container, so a
     * count is bounded like any length of the input. */
    uint32_t class_count;
    uint32_t record_count;
    void *storage;
    struct markdown_core_resource *anchor_owner;
} markdown_core_attributes;

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
} markdown_core_attribute_parser;

/* Whether a value owns anything a release must free. Everything it can own
 * is its block or hangs off its anchor -- a string of its own, or the hold on
 * the resource it borrows from -- so a value with neither owns nothing.
 * Almost every value is empty -- every node carries one and no Text has
 * attributes -- so this is the first test a release makes, shared with the
 * node release that makes it in place before calling. */
static MARKDOWN_CORE_INLINE bool markdown_core_attributes_owns(const markdown_core_attributes *value) {
    return value->storage || value->anchor.data;
}
void markdown_core_attributes_free(markdown_core_attributes *value);
/* A value holding one class, `bytes`, and nothing else: the value an element
 * makes when its syntax names a class without an attribute container. The
 * caller supplies an empty value. Returns 0 on allocation failure, leaving it
 * empty. */
int markdown_core_attributes_single_class(markdown_core_attributes *value, const unsigned char *bytes,
                                          bufsize_t length);
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

#endif
