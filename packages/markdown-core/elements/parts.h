#ifndef MARKDOWN_CORE_PARTS_H
#define MARKDOWN_CORE_PARTS_H

#include <stdbool.h>
#include <stddef.h>

#include "../include/markdown_core.h"
#include "buffer.h"
#include "node.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A PART of a node's own source (docs/plans/2026-10-23-lsp-completion-engine.md,
 * 3.3): the bytes from `start` to `end` of the node's text that a field of
 * slot kind `slot` holds, NONE for a field no slot kind names (a literal, a
 * title, a cross link's label), as written: the value's own bytes, escapes
 * and references included, without the quotes, brackets or sigil around it.
 * A `lent` part holds a field of the node that holds this one, which takes
 * it as its own (a list item's block identifier, written at its first
 * paragraph's end): it is neither this node's field nor its marker. */
typedef struct markdown_core_part {
    markdown_core_slot_kind slot;
    bufsize_t start, end;
    bool lent;
} markdown_core_part;

/* WHAT A NODE'S READER READS AND WRITES. `text` is the node's source, its
 * runs joined, `length` bytes; `held` are the ranges of it the nodes the
 * node holds take, its children and its node-valued fields, in source
 * order. A line ending follows the text, for a scanner that reads to one. A reader adds the node's parts; the rest of
 * its own source, which neither a part nor a held node takes, is its markers. The reader reads the text as the element
 * that made the node read it, with that element's scanners, so a part is where the parse read the field. */
typedef struct markdown_core_parts {
    const markdown_core_node *node;
    const unsigned char *text;
    bufsize_t length;
    const markdown_core_place *held;
    size_t held_count;
    markdown_core_part *items;
    size_t count, capacity;
    bool failed;
} markdown_core_parts;

/* Adds the part of slot kind `slot` from `start` to `end`, unless it is
 * empty. */
void markdown_core_parts_add(markdown_core_parts *parts, markdown_core_slot_kind slot, bufsize_t start, bufsize_t end);
/* Adds the range from `start` to `end` as lent to the node's holder. */
void markdown_core_parts_lend(markdown_core_parts *parts, markdown_core_slot_kind slot, bufsize_t start, bufsize_t end);
/* Where the held node the text holds at `at` ends, or `at` when none starts
 * there. */
bufsize_t markdown_core_parts_skip(const markdown_core_parts *parts, bufsize_t at);
/* Where the first held node that starts at or after `at` starts, or the
 * text's length. */
bufsize_t markdown_core_parts_next(const markdown_core_parts *parts, bufsize_t at);
/* Whether a held node holds the byte at `at`. */
bool markdown_core_parts_within(const markdown_core_parts *parts, bufsize_t at);
/* Where the last held node ends, or `at` when none ends after it. */
bufsize_t markdown_core_parts_last(const markdown_core_parts *parts, bufsize_t at);

/* Adds the field of slot kind NONE that holds `literal`, `size` bytes of a
 * block's literal whose lines end the text's lines from `from` on: the
 * block took each line but for what it stripped from its start, an
 * indentation or a container's prefix. Answers where the last line ends. */
bufsize_t markdown_core_parts_lines(markdown_core_parts *parts, bufsize_t from, const unsigned char *literal,
                                    bufsize_t size);

/* Adds the field of slot kind NONE that holds `literal`, `size` bytes of a
 * node's literal, found line by line in the text from `from` on: each of
 * its lines is the first place after the one before that holds its bytes.
 * Answers where the last line ends. */
bufsize_t markdown_core_parts_find(markdown_core_parts *parts, bufsize_t from, const unsigned char *literal,
                                   bufsize_t size);

/* The source ranges of `node`'s fields of slot kind `slot`, in the tree
 * `root` parsed from `length` bytes of `source`: one per run of each field,
 * in source order, an array the caller frees with markdown_core_free. */
markdown_core_status markdown_core_parts_fields(const markdown_core_node *root, const markdown_core_node *node,
                                                markdown_core_slot_kind slot, const uint8_t *source, size_t length,
                                                markdown_core_place **places, size_t *count);

/* THE READERS, one for each element whose nodes hold fields; a kind with
 * none has none, and its whole own source is markers (an emphasis's
 * delimiters). */
void markdown_core_literal_parts(markdown_core_parts *parts);
/* The parts of the attribute container whose `{` is at `brace`: one past
 * its `}`, or `brace` when none closes there. */
bufsize_t markdown_core_attributes_parts(markdown_core_parts *parts, bufsize_t brace);
/* The parts of the container that ends a block's line at `end`, but for
 * spaces and tabs, read after `from` as the block read it: its `{`, or the
 * end of the line's other bytes when it has none. */
bufsize_t markdown_core_attributes_tail_parts(markdown_core_parts *parts, bufsize_t from, bufsize_t end);
void markdown_core_link_parts(markdown_core_parts *parts);
void markdown_core_reference_parts(markdown_core_parts *parts);
void markdown_core_cross_link_parts(markdown_core_parts *parts);
void markdown_core_code_parts(markdown_core_parts *parts);
void markdown_core_code_block_parts(markdown_core_parts *parts);
void markdown_core_heading_parts(markdown_core_parts *parts);
void markdown_core_callout_parts(markdown_core_parts *parts);
void markdown_core_directive_parts(markdown_core_parts *parts);
void markdown_core_topic_parts(markdown_core_parts *parts);
void markdown_core_citation_parts(markdown_core_parts *parts);
void markdown_core_footnote_parts(markdown_core_parts *parts);
void markdown_core_specimen_parts(markdown_core_parts *parts);
void markdown_core_span_parts(markdown_core_parts *parts);
void markdown_core_block_identifier_parts(markdown_core_parts *parts);
void markdown_core_formula_parts(markdown_core_parts *parts);
void markdown_core_comment_parts(markdown_core_parts *parts);
void markdown_core_list_item_parts(markdown_core_parts *parts);
void markdown_core_metadata_parts(markdown_core_parts *parts);
void markdown_core_table_row_parts(markdown_core_parts *parts);

#ifdef __cplusplus
}
#endif

#endif
