#ifndef MARKDOWN_CORE_TEXT_TREE_H
#define MARKDOWN_CORE_TEXT_TREE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* THE TEXT A SESSION HOLDS (docs/plans/2026-09-29-incremental-parsing.md,
 * 5.1): a balanced tree of byte pieces in source order. Each piece records
 * its bytes and UTF-16 units, and each subtree their sums, so finding an
 * offset, replacing a range and shifting everything after it cost O(log n)
 * wherever the edit is, plus the bytes the edit writes.
 *
 * A byte counts the UTF-16 units of the scalar it begins: a continuation byte
 * none, a byte that begins a four-byte scalar two, any other byte one. That
 * is the rule scope queries count columns by, so a sum over any pieces is the
 * units of their bytes whatever the bytes are, and the text is never checked
 * for being well formed.
 *
 * Every piece holds between TEXT_PIECE_MIN and TEXT_PIECE_MAX bytes, except
 * the one piece of a text shorter than that. A batch of edits re-chunks the
 * pieces its edits touch, with a neighbour where they would be too few bytes,
 * so the tree holds O(n / TEXT_PIECE_MIN) pieces after any history of edits.
 * A piece is never changed after it is made: a batch makes new pieces for
 * what it writes and frees the ones it replaces. */

typedef struct markdown_core_text_piece markdown_core_text_piece;

typedef struct markdown_core_text_tree {
    markdown_core_text_piece *root;
} markdown_core_text_tree;

/* The text of `bytes`. False, with an empty text, when a piece could not be
 * allocated. */
bool markdown_core_text_tree_init(markdown_core_text_tree *text, const uint8_t *bytes, size_t size);
void markdown_core_text_tree_dispose(markdown_core_text_tree *text);

size_t markdown_core_text_tree_size(const markdown_core_text_tree *text);
size_t markdown_core_text_tree_units(const markdown_core_text_tree *text);

/* The byte offset at which the UTF-16 units before it are `units`: the end of
 * a scalar, or of the text. False when `units` is past the text or between
 * the two units of one scalar. */
bool markdown_core_text_tree_offset(const markdown_core_text_tree *text, size_t units, size_t *offset);

/* Whether a scalar begins at byte `offset`, which is at most the text's size:
 * the end of the text, or a byte that is not a continuation byte -- the rule
 * the UTF-16 count reads bytes by. */
bool markdown_core_text_tree_boundary(const markdown_core_text_tree *text, size_t offset);

/* ONE EDIT between two texts, in bytes of the text before it: bytes
 * [start, end) were replaced by `size` bytes. */
typedef struct markdown_core_byte_edit {
    size_t start, end, size;
} markdown_core_byte_edit;

/* Applies a batch of `count` edits, disjoint and in source order, each in
 * bytes of the text before the batch; edit i writes the bytes at `texts[i]`.
 * The pieces a batch touches are made again once, whatever number of its
 * edits fall in them. False, with the text unchanged, when a piece could not
 * be allocated. */
bool markdown_core_text_tree_replace(markdown_core_text_tree *text, const markdown_core_byte_edit *edits,
                                     const uint8_t *const *texts, size_t count);

/* Copies the whole text into `bytes`, which holds its size. */
void markdown_core_text_tree_copy(const markdown_core_text_tree *text, uint8_t *bytes);

/* An AVL tree of n pieces is at most 1.44 log2(n + 2) high, and a text holds
 * fewer than 2^40 pieces. */
#define MARKDOWN_CORE_TEXT_TREE_DEPTH 64

/* A READER OF THE PIECES in source order from a byte offset: the piece in
 * hand and the ancestors still to come. Pieces are never changed after they
 * are made, so the bytes a reader hands out stay as they are until the text
 * is next edited. */
typedef struct markdown_core_text_cursor {
    markdown_core_text_piece *stack[MARKDOWN_CORE_TEXT_TREE_DEPTH];
    size_t depth;
    markdown_core_text_piece *piece;
} markdown_core_text_cursor;

/* The piece that holds byte `offset`: its bytes, its size and the offset it
 * begins at. False when `offset` is at or past the end of the text. */
bool markdown_core_text_cursor_seek(markdown_core_text_cursor *cursor, const markdown_core_text_tree *text,
                                    size_t offset, const uint8_t **bytes, size_t *size, size_t *start);
/* The piece after the one in hand, or false after the last. */
bool markdown_core_text_cursor_next(markdown_core_text_cursor *cursor, const uint8_t **bytes, size_t *size);

/* Where the line that holds byte `offset` begins: just after the line
 * terminator before it, or 0. A CR LF pair is one terminator. */
size_t markdown_core_text_tree_line_start(const markdown_core_text_tree *text, size_t offset);
/* The byte at `offset`, which is inside the text. */
uint8_t markdown_core_text_tree_byte(const markdown_core_text_tree *text, size_t offset);

#ifdef __cplusplus
}
#endif

#endif
