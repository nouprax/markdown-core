#include <string.h>

#include "alloc.h"
#include "text_tree.h"

#define TEXT_PIECE_MAX ((size_t)1024)
#define TEXT_PIECE_MIN (TEXT_PIECE_MAX / 4)
#define TEXT_TREE_DEPTH MARKDOWN_CORE_TEXT_TREE_DEPTH

struct markdown_core_text_piece {
    markdown_core_text_piece *before, *after;
    /* This piece's bytes and units, and its subtree's bytes, units and
     * pieces. */
    size_t size, units;
    size_t total_size, total_units, count;
    int level;
    uint8_t bytes[];
};

static inline size_t byte_units(uint8_t byte) { return (byte & 0xC0) == 0x80 ? 0 : byte >= 0xF0 ? 2 : 1; }

static inline int level_of(const markdown_core_text_piece *piece) { return piece ? piece->level : 0; }
static inline size_t size_of(const markdown_core_text_piece *piece) { return piece ? piece->total_size : 0; }
static inline size_t units_of(const markdown_core_text_piece *piece) { return piece ? piece->total_units : 0; }
static inline size_t count_of(const markdown_core_text_piece *piece) { return piece ? piece->count : 0; }

static void update(markdown_core_text_piece *piece) {
    int before = level_of(piece->before), after = level_of(piece->after);
    piece->level = 1 + (before > after ? before : after);
    piece->total_size = size_of(piece->before) + piece->size + size_of(piece->after);
    piece->total_units = units_of(piece->before) + piece->units + units_of(piece->after);
    piece->count = count_of(piece->before) + 1 + count_of(piece->after);
}

static markdown_core_text_piece *lift_before(markdown_core_text_piece *piece) {
    markdown_core_text_piece *before = piece->before;
    piece->before = before->after;
    before->after = piece;
    update(piece);
    update(before);
    return before;
}

static markdown_core_text_piece *lift_after(markdown_core_text_piece *piece) {
    markdown_core_text_piece *after = piece->after;
    piece->after = after->before;
    after->before = piece;
    update(piece);
    update(after);
    return after;
}

/* The subtree rooted at `piece`, balanced again after one insertion or
 * removal below it, with its sums current. */
static markdown_core_text_piece *rebalance(markdown_core_text_piece *piece) {
    if (!piece) {
        return NULL;
    }
    int balance = level_of(piece->before) - level_of(piece->after);
    if (balance > 1) {
        if (level_of(piece->before->before) < level_of(piece->before->after)) {
            piece->before = lift_after(piece->before);
        }
        return lift_before(piece);
    }
    if (balance < -1) {
        if (level_of(piece->after->after) < level_of(piece->after->before)) {
            piece->after = lift_before(piece->after);
        }
        return lift_after(piece);
    }
    update(piece);
    return piece;
}

/* Rebalances every subtree on a path, deepest first. */
static void rebalance_path(markdown_core_text_piece **path[], size_t depth) {
    while (depth--) {
        *path[depth] = rebalance(*path[depth]);
    }
}

/* How many bytes of a word have their high bit set in `flags`, which holds
 * only high bits. */
static inline size_t marked(uint64_t flags) { return (size_t)(((flags >> 7) * 0x0101010101010101u) >> 56); }

/* The units of `size` bytes, eight at a time: every byte but a continuation
 * byte (10xxxxxx) counts one, and a byte that begins a four-byte scalar
 * (11110xxx and above) one more. */
static size_t units_in(const uint8_t *bytes, size_t size) {
    const uint64_t high = 0x8080808080808080u;
    size_t units = size, i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t word;
        memcpy(&word, bytes + i, sizeof(word));
        units -= marked(word & ~(word << 1) & high);
        units += marked(word & (word << 1) & (word << 2) & (word << 3) & high);
    }
    for (; i < size; i++) {
        units += byte_units(bytes[i]);
        units--;
    }
    return units;
}

/* A piece holding `size` bytes from `bytes`, with its units. */
static markdown_core_text_piece *piece_new(const uint8_t *bytes, size_t size) {
    markdown_core_text_piece *piece = markdown_core_alloc(1, sizeof(*piece) + size);
    if (!piece) {
        return NULL;
    }
    memcpy(piece->bytes, bytes, size);
    piece->size = size;
    piece->units = units_in(bytes, size);
    update(piece);
    return piece;
}

/* Inserts `piece` so that `rank` pieces precede it. */
static void insert_at(markdown_core_text_tree *text, size_t rank, markdown_core_text_piece *piece) {
    markdown_core_text_piece **path[TEXT_TREE_DEPTH];
    size_t depth = 0;
    markdown_core_text_piece **slot = &text->root;
    while (*slot) {
        path[depth++] = slot;
        size_t before = count_of((*slot)->before);
        if (rank <= before) {
            slot = &(*slot)->before;
        } else {
            rank -= before + 1;
            slot = &(*slot)->after;
        }
    }
    *slot = piece;
    rebalance_path(path, depth);
}

/* Removes the piece `rank` pieces follow and frees it. */
static void remove_at(markdown_core_text_tree *text, size_t rank) {
    markdown_core_text_piece **path[TEXT_TREE_DEPTH];
    size_t depth = 0;
    markdown_core_text_piece **slot = &text->root;
    for (;;) {
        path[depth++] = slot;
        size_t before = count_of((*slot)->before);
        if (rank == before) {
            break;
        }
        if (rank < before) {
            slot = &(*slot)->before;
        } else {
            rank -= before + 1;
            slot = &(*slot)->after;
        }
    }
    markdown_core_text_piece *piece = *slot;
    if (!piece->after) {
        *slot = piece->before;
        depth--;
    } else {
        /* The next piece, the first of the after subtree, takes its place. */
        size_t mark = depth;
        markdown_core_text_piece **next = &piece->after;
        while ((*next)->before) {
            path[depth++] = next;
            next = &(*next)->before;
        }
        markdown_core_text_piece *successor = *next;
        *next = successor->after;
        successor->before = piece->before;
        successor->after = piece->after;
        *slot = successor;
        if (depth > mark) {
            path[mark] = &successor->after;
        }
    }
    markdown_core_free(piece);
    rebalance_path(path, depth);
}

typedef markdown_core_text_cursor text_cursor;

/* The piece holding byte `offset`, or the last piece when `offset` is the
 * size of the text; its rank and where it begins. NULL for an empty text. */
static markdown_core_text_piece *cursor_seek(text_cursor *cursor, const markdown_core_text_tree *text, size_t offset,
                                             size_t *rank, size_t *begin) {
    markdown_core_text_piece *piece = text->root;
    size_t base = 0, counted = 0;
    cursor->depth = 0;
    cursor->piece = NULL;
    if (piece && offset >= piece->total_size) {
        offset = piece->total_size - 1;
    }
    while (piece) {
        size_t before = size_of(piece->before);
        if (offset < base + before) {
            cursor->stack[cursor->depth++] = piece;
            piece = piece->before;
        } else if (offset < base + before + piece->size) {
            base += before;
            counted += count_of(piece->before);
            break;
        } else {
            base += before + piece->size;
            counted += count_of(piece->before) + 1;
            piece = piece->after;
        }
    }
    cursor->piece = piece;
    *rank = counted;
    *begin = base;
    return piece;
}

static markdown_core_text_piece *cursor_next(text_cursor *cursor) {
    for (markdown_core_text_piece *piece = cursor->piece->after; piece; piece = piece->before) {
        cursor->stack[cursor->depth++] = piece;
    }
    cursor->piece = cursor->depth ? cursor->stack[--cursor->depth] : NULL;
    return cursor->piece;
}

/* Copies bytes [start, end) of the text into `out`. */
static void copy_span(const markdown_core_text_tree *text, size_t start, size_t end, uint8_t *out) {
    if (start == end) {
        return;
    }
    text_cursor cursor;
    size_t rank, begin;
    markdown_core_text_piece *piece = cursor_seek(&cursor, text, start, &rank, &begin);
    size_t from = start - begin;
    while (start < end) {
        size_t take = piece->size - from;
        if (take > end - start) {
            take = end - start;
        }
        memcpy(out, piece->bytes + from, take);
        out += take;
        start += take;
        from = 0;
        if (start < end) {
            piece = cursor_next(&cursor);
        }
    }
}

bool markdown_core_text_tree_init(markdown_core_text_tree *text, const uint8_t *bytes, size_t size) {
    markdown_core_byte_edit edit = {0, 0, size};
    text->root = NULL;
    return markdown_core_text_tree_replace(text, &edit, &bytes, 1);
}

void markdown_core_text_tree_dispose(markdown_core_text_tree *text) {
    /* Pieces are freed as the in-order walk leaves them. */
    markdown_core_text_piece *stack[TEXT_TREE_DEPTH];
    size_t depth = 0;
    markdown_core_text_piece *piece = text->root;
    while (piece || depth) {
        while (piece) {
            stack[depth++] = piece;
            piece = piece->before;
        }
        piece = stack[--depth];
        markdown_core_text_piece *after = piece->after;
        markdown_core_free(piece);
        piece = after;
    }
    text->root = NULL;
}

size_t markdown_core_text_tree_size(const markdown_core_text_tree *text) { return size_of(text->root); }

size_t markdown_core_text_tree_units(const markdown_core_text_tree *text) { return units_of(text->root); }

bool markdown_core_text_tree_offset(const markdown_core_text_tree *text, size_t units, size_t *offset) {
    /* The piece whose units hold `units`: the answer is the first byte at or
     * after its start that begins a scalar with `units` before it, or the end
     * of the text. */
    text_cursor cursor = {.depth = 0};
    markdown_core_text_piece *piece = text->root;
    size_t base = 0, counted = 0;
    while (piece) {
        size_t before = units_of(piece->before);
        if (units < counted + before) {
            cursor.stack[cursor.depth++] = piece;
            piece = piece->before;
        } else if (units <= counted + before + piece->units) {
            counted += before;
            base += size_of(piece->before);
            break;
        } else {
            counted += before + piece->units;
            base += size_of(piece->before) + piece->size;
            piece = piece->after;
        }
    }
    cursor.piece = piece;
    for (; piece; piece = cursor_next(&cursor)) {
        for (size_t i = 0; i < piece->size; i++) {
            size_t width = byte_units(piece->bytes[i]);
            if (width && counted >= units) {
                *offset = base + i;
                return counted == units;
            }
            counted += width;
        }
        base += piece->size;
    }
    *offset = base;
    return counted == units;
}

bool markdown_core_text_tree_boundary(const markdown_core_text_tree *text, size_t offset) {
    if (offset >= size_of(text->root)) {
        return true;
    }
    text_cursor cursor;
    size_t rank, begin;
    const markdown_core_text_piece *piece = cursor_seek(&cursor, text, offset, &rank, &begin);
    return byte_units(piece->bytes[offset - begin]) != 0;
}

/* THE PIECES ONE PART OF A BATCH MAKES AGAIN: pieces [first, last] of the
 * text before the batch, which hold bytes [from, to), and the edits
 * [edit, edit + count) that fall in them. */
typedef struct {
    size_t first, last, from, to, edit, count;
    /* The bytes it writes: what its pieces keep and what its edits insert. */
    size_t written;
} text_patch;

/* Adds the piece after `patch`, or before it when it holds the last piece. */
static void patch_widen(const markdown_core_text_tree *text, text_patch *patch) {
    text_cursor cursor;
    size_t rank, begin;
    if (patch->last + 1 < count_of(text->root)) {
        size_t size = cursor_seek(&cursor, text, patch->to, &rank, &begin)->size;
        patch->last++;
        patch->to += size;
        patch->written += size;
    } else {
        cursor_seek(&cursor, text, patch->from - 1, &rank, &begin);
        patch->written += patch->from - begin;
        patch->first--;
        patch->from = begin;
    }
}

/* `patch` with `next`, the patch after it, taken in; they share a piece. */
static void patch_join(text_patch *patch, const text_patch *next) {
    size_t shared = patch->to - next->from;
    patch->written += next->written - shared;
    patch->last = next->last;
    patch->to = next->to;
    patch->count += next->count;
}

bool markdown_core_text_tree_replace(markdown_core_text_tree *text, const markdown_core_byte_edit *edits,
                                     const uint8_t *const *texts, size_t count) {
    /* Each edit falls in the pieces that hold its start and its end; edits
     * that fall in a common piece make those pieces again together, as one
     * patch, and a patch that would write fewer than TEXT_PIECE_MIN bytes
     * takes in a neighbouring piece. Patches are found in the text before
     * the batch, every new piece is allocated, and only then does the tree
     * change, last patch first, so that every rank still to use holds. */
    size_t pieces = count_of(text->root);
    text_patch *patches = markdown_core_alloc(count + 1, sizeof(*patches));
    if (!patches) {
        return false;
    }
    size_t used = 0;
    text_cursor cursor;
    for (size_t i = 0; i < count; i++) {
        text_patch patch = {0, 0, 0, 0, i, 1, 0};
        if (pieces) {
            cursor_seek(&cursor, text, edits[i].start, &patch.first, &patch.from);
            size_t size = cursor_seek(&cursor, text, edits[i].end, &patch.last, &patch.to)->size;
            patch.to += size;
        }
        patch.written = patch.to - patch.from - (edits[i].end - edits[i].start) + edits[i].size;
        /* An empty text has no pieces to tell edits apart: one patch. */
        if (used && (!pieces || patch.first <= patches[used - 1].last)) {
            patch_join(&patches[used - 1], &patch);
        } else {
            patches[used++] = patch;
        }
    }
    /* Every patch writes TEXT_PIECE_MIN bytes or holds every piece. A
     * widened patch that reaches the next one, or the previous one when it
     * widens backwards at the end of the text, joins it. */
    for (size_t r = 0; r < used;) {
        text_patch *patch = &patches[r];
        if (patch->written >= TEXT_PIECE_MIN || patch->last - patch->first + 1 >= pieces) {
            r++;
            continue;
        }
        patch_widen(text, patch);
        if (r + 1 < used && patches[r + 1].first <= patch->last) {
            patch_join(patch, &patches[r + 1]);
            memmove(&patches[r + 1], &patches[r + 2], (used - r - 2) * sizeof(*patches));
            used--;
        } else if (r && patches[r - 1].last >= patch->first) {
            patch_join(&patches[r - 1], patch);
            used--;
            r--;
        }
    }
    /* Each patch writes its bytes into `scratch` and cuts them into `made`
     * near-equal pieces; a piece ends on a scalar boundary when one is within
     * three bytes of its end. */
    size_t made = 0, widest = 0;
    for (size_t r = 0; r < used; r++) {
        made += (patches[r].written + TEXT_PIECE_MAX - 1) / TEXT_PIECE_MAX;
        widest = patches[r].written > widest ? patches[r].written : widest;
    }
    uint8_t *scratch = markdown_core_alloc(widest + 1, 1);
    markdown_core_text_piece **fresh = markdown_core_alloc(made + 1, sizeof(*fresh));
    bool allocated = scratch && fresh;
    size_t built = 0;
    for (size_t r = 0; r < used && allocated; r++) {
        const text_patch *patch = &patches[r];
        size_t at = patch->from, written = 0;
        for (size_t i = patch->edit; i < patch->edit + patch->count; i++) {
            copy_span(text, at, edits[i].start, scratch + written);
            written += edits[i].start - at;
            if (edits[i].size) {
                memcpy(scratch + written, texts[i], edits[i].size);
            }
            written += edits[i].size;
            at = edits[i].end;
        }
        copy_span(text, at, patch->to, scratch + written);
        size_t parts = (patch->written + TEXT_PIECE_MAX - 1) / TEXT_PIECE_MAX, offset = 0;
        for (size_t i = 0; i < parts && allocated; i++) {
            size_t stop = (size_t)((uint64_t)patch->written * (i + 1) / parts);
            for (size_t back = 0; i + 1 < parts && back < 3 && stop - back > offset + 1; back++) {
                if ((scratch[stop - back] & 0xC0) != 0x80) {
                    stop -= back;
                    break;
                }
            }
            fresh[built] = piece_new(scratch + offset, stop - offset);
            allocated = fresh[built] != NULL;
            built += allocated;
            offset = stop;
        }
    }
    markdown_core_free(scratch);
    if (!allocated) {
        while (built--) {
            markdown_core_free(fresh[built]);
        }
        markdown_core_free(fresh);
        markdown_core_free(patches);
        return false;
    }
    for (size_t r = used; r--;) {
        const text_patch *patch = &patches[r];
        size_t parts = (patch->written + TEXT_PIECE_MAX - 1) / TEXT_PIECE_MAX;
        built -= parts;
        for (size_t i = 0; pieces && i < patch->last - patch->first + 1; i++) {
            remove_at(text, patch->first);
        }
        for (size_t i = 0; i < parts; i++) {
            insert_at(text, patch->first + i, fresh[built + i]);
        }
    }
    markdown_core_free(fresh);
    markdown_core_free(patches);
    return true;
}

void markdown_core_text_tree_copy(const markdown_core_text_tree *text, uint8_t *bytes) {
    copy_span(text, 0, size_of(text->root), bytes);
}

bool markdown_core_text_cursor_seek(markdown_core_text_cursor *cursor, const markdown_core_text_tree *text,
                                    size_t offset, const uint8_t **bytes, size_t *size, size_t *start) {
    size_t rank;
    if (offset >= size_of(text->root)) {
        return false;
    }
    const markdown_core_text_piece *piece = cursor_seek(cursor, text, offset, &rank, start);
    *bytes = piece->bytes;
    *size = piece->size;
    return true;
}

bool markdown_core_text_cursor_next(markdown_core_text_cursor *cursor, const uint8_t **bytes, size_t *size) {
    const markdown_core_text_piece *piece = cursor_next(cursor);
    if (!piece) {
        return false;
    }
    *bytes = piece->bytes;
    *size = piece->size;
    return true;
}

uint8_t markdown_core_text_tree_byte(const markdown_core_text_tree *text, size_t offset) {
    text_cursor cursor;
    size_t rank, begin;
    const markdown_core_text_piece *piece = cursor_seek(&cursor, text, offset, &rank, &begin);
    return piece->bytes[offset - begin];
}

/* Backwards from `offset`, one piece at a time: each piece is found again
 * from the root, since a reader keeps only the ancestors still to come. */
size_t markdown_core_text_tree_line_start(const markdown_core_text_tree *text, size_t offset) {
    size_t at = offset;
    while (at > 0) {
        text_cursor cursor;
        size_t rank, begin;
        const markdown_core_text_piece *piece = cursor_seek(&cursor, text, at - 1, &rank, &begin);
        for (size_t i = at - begin; i-- > 0;) {
            uint8_t byte = piece->bytes[i];
            if (byte == '\n' || byte == '\r') {
                return begin + i + 1;
            }
        }
        at = begin;
    }
    return 0;
}
