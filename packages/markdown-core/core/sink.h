#ifndef MARKDOWN_CORE_SINK_H
#define MARKDOWN_CORE_SINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../include/markdown_core.h"
#include "chunk.h"
#include "node.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WHERE A FIELD ENDS: the bytes of the field at the front of `data`, `length`
 * bytes of the source from the field's start to its line's end, as the
 * element that read the field's start reads the rest of it. The scan starts
 * at the field's start, not at a cursor in it, so what the field holds
 * before the cursor (a segment before a `/`, an open parenthesis) decides
 * how it goes on past the cursor. Negative when the scan could not
 * allocate. */
typedef bufsize_t (*markdown_core_field_end)(const unsigned char *data, bufsize_t length);

/* A FIELD a scanner reads when its input ends, as the scanner reports it:
 * its slot kind, the kind of the construct that owns it, the offset it
 * starts at in the scanner's input, how it goes on past a cursor, the input
 * ranges of the earlier fields it depends on, and whether it declares a
 * name, which the document must confirm by finishing its construct. */
typedef struct markdown_core_field {
    markdown_core_slot_kind slot;
    markdown_core_node_kind owner;
    bufsize_t start;
    markdown_core_field_end end;
    bufsize_t prefix[2][2];
    size_t prefix_count;
    bool declares;
} markdown_core_field;

/* A construct the parse of the text before a cursor leaves open: the kind it
 * opens, or NONE when its kind is still open too (a `[` may open a link, a
 * span or a cite), the source range of its opening syntax, and whether a
 * line of the block phase read it rather than an inline run. */
typedef struct markdown_core_context_opener {
    markdown_core_node_kind kind;
    markdown_core_place place;
    bool block;
} markdown_core_context_opener;

/* A field an open construct reads when the text ends: its slot kind, the
 * kind of the construct that owns it, the construct's opener (its index in
 * the sink's inline openers, or -1 for a block's), the source offset the
 * field starts at, how the field goes on past the cursor, the source ranges
 * of the earlier fields it depends on, at most two, and whether it declares
 * a name, which only a construct the document finishes does. */
typedef struct markdown_core_context_field {
    markdown_core_slot_kind kind;
    markdown_core_node_kind owner;
    int32_t opener;
    uint32_t start;
    markdown_core_field_end end;
    markdown_core_place prefix[2];
    uint32_t prefix_count;
    bool declares;
} markdown_core_context_field;

/* THE CONTEXT SINK (docs/plans/2026-10-23-lsp-completion-engine.md, 3.1):
 * what the parse of the text before a cursor reports of what it leaves open,
 * the parse's `sink`, NULL in every other parse. `source` is the text and
 * `length` its length, the cursor. The block phase lists the blocks open as the text ends,
 * outermost first; an inline run whose content reaches the end, but for
 * spaces and tabs, lists its openers and the fields they read, in the order
 * its tokens read them. `failed` is set when a list could not grow. */
typedef struct markdown_core_context_sink {
    const unsigned char *source;
    size_t length;
    /* The inline root whose run reports, which reached the end, or NULL,
     * and, once a report needs them, its source places that read content
     * with the content bytes before each, so a lookup is a binary search. */
    const struct markdown_core_node *root;
    uint32_t *root_places, *root_reads;
    size_t root_place_count;
    bool root_indexed;
    const markdown_core_node **blocks;
    size_t block_count, block_capacity;
    markdown_core_context_opener *openers;
    size_t opener_count, opener_capacity;
    markdown_core_context_field *fields;
    size_t field_count, field_capacity;
    /* The openers and the fields by hash, so a listing finds its duplicate
     * in bounded work: each slot holds an index plus one, or 0. */
    uint32_t *opener_slots, *field_slots;
    size_t opener_slot_count, field_slot_count;
    bool failed;
} markdown_core_context_sink;

/* Lists the opener of `kind` at source range `place`, a block's when
 * `block`, unless it is listed: its index, or -1 when it could not be
 * listed. */
int32_t markdown_core_context_opener_add(markdown_core_context_sink *sink, markdown_core_node_kind kind,
                                         markdown_core_place place, bool block);
/* Lists `field`, unless a field of its kind, owner, opener and start that
 * declares as it does is listed: a scanner may read one end twice. */
void markdown_core_context_field_add(markdown_core_context_sink *sink, const markdown_core_context_field *field);
/* Releases what the sink lists, which leaves it empty. */
void markdown_core_context_sink_clear(markdown_core_context_sink *sink);

#ifdef __cplusplus
}
#endif

#endif
