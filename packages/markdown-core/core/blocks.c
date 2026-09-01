/**
 * Block parsing implementation.
 *
 * For a high-level overview of the block parsing process,
 * see http://spec.commonmark.org/0.24/#phase-1-block-structure
 */

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "markdown_core_ctype.h"
#include "syntax_extension.h"
#include "config.h"
#include "parser.h"
#include "markdown-core.h"
#include "node.h"
#include "references.h"
#include "utf8.h"
#include "scanners.h"
#include "inlines.h"
#include "houdini.h"
#include "buffer.h"
#include "iterator.h"

#define CODE_INDENT 4
#define TAB_STOP 4

/**
 * Very deeply nested lists can cause quadratic performance issues.
 * This constant is used in open_new_blocks() to limit the nesting
 * depth. It is unlikely that a non-contrived markdown document will
 * be nested this deeply.
 */
#define MAX_LIST_DEPTH 100

#ifndef MIN
#define MIN(x, y) ((x < y) ? x : y)
#endif

#define peek_at(i, n) (i)->data[n]

static bool S_html_literal_starts_with_comment(markdown_core_node *node) {
    markdown_core_chunk *literal;
    bufsize_t offset = 0;

    if (node->type != MARKDOWN_CORE_NODE_HTML_BLOCK && node->type != MARKDOWN_CORE_NODE_HTML) {
        return false;
    }

    literal = &node->as.literal;

    if (node->type == MARKDOWN_CORE_NODE_HTML_BLOCK) {
        while (offset < literal->len && (literal->data[offset] == ' ' || literal->data[offset] == '\t')) {
            offset++;
        }
    }

    return literal->len - offset >= 4 && memcmp(literal->data + offset, "<!--", 4) == 0;
}

/* THE INLINE HALF OF THE COMMENT STRIP, per block (T18; F13 requirement 3).
 * Frees every `HTML` node under `block` whose literal opens a comment, then
 * re-consolidates the block. `block` itself is never freed here: a block that
 * IS a comment is an `HTML_BLOCK`, and the tail removes that one after asking
 * the same predicate. The whole-tree strip this replaces freed its own root
 * inside its walk and then consolidated the freed pointer, which is why the
 * judgement and the removal are two places now. */
static bool S_strip_inline_comments(markdown_core_node *block) {
    bool stripped = false;
    markdown_core_iter walk;
    markdown_core_iter *iter = &walk;
    markdown_core_event_type ev_type;

    markdown_core_iter_init(iter, block);

    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = markdown_core_iter_get_node(iter);
        /* EXIT, not ENTER: the mutation rule names the node whose EXIT is
         * current, and it is the only moment the iterator's lookahead is
         * outside this node's subtree. `HTML` and `HTML_BLOCK` were both in
         * the old `S_is_leaf` list, so their EXIT was suppressed and freeing
         * at ENTER happened to be safe; with the contract total it is a
         * use-after-free on the very next `markdown_core_iter_next`. */
        if (ev_type == MARKDOWN_CORE_EVENT_EXIT && node != block && S_html_literal_starts_with_comment(node)) {
            markdown_core_node_free(node);
            stripped = true;
        }
    }

    if (walk.oom) {
        /* A refused spill truncated the walk (iterator.h): the strip may
         * have missed comments, so the parse answers as any lost
         * allocation does. */
        return false;
    }
    if (stripped) {
        return markdown_core_consolidate_text_nodes(block) != 0;
    }
    return true;
}

static bool S_last_line_blank(const markdown_core_node *node) {
    return (node->flags & MARKDOWN_CORE_NODE__LAST_LINE_BLANK) != 0;
}

static bool S_last_line_checked(const markdown_core_node *node) {
    return (node->flags & MARKDOWN_CORE_NODE__LAST_LINE_CHECKED) != 0;
}

static MARKDOWN_CORE_INLINE markdown_core_node_type S_type(const markdown_core_node *node) {
    return (markdown_core_node_type)node->type;
}

static void S_set_last_line_blank(markdown_core_node *node, bool is_blank) {
    if (is_blank) {
        node->flags |= MARKDOWN_CORE_NODE__LAST_LINE_BLANK;
    } else {
        node->flags &= ~MARKDOWN_CORE_NODE__LAST_LINE_BLANK;
    }
}

static void S_set_last_line_checked(markdown_core_node *node) { node->flags |= MARKDOWN_CORE_NODE__LAST_LINE_CHECKED; }

static MARKDOWN_CORE_INLINE bool S_is_line_end_char(char c) { return (c == '\n' || c == '\r'); }

static MARKDOWN_CORE_INLINE bool S_is_space_or_tab(char c) { return (c == ' ' || c == '\t'); }

static void S_parser_feed(markdown_core_parser *parser, const unsigned char *buffer, size_t len, bool eof);

static void S_process_line(markdown_core_parser *parser, const unsigned char *buffer, bufsize_t bytes);

static markdown_core_node *make_block(
    markdown_core_mem *mem,
    markdown_core_node_type tag,
    int start_line,
    int start_column
) {
    markdown_core_node *e;

    e = (markdown_core_node *)mem->calloc(1, sizeof(*e));
    if (!e) {
        return NULL;
    }
    markdown_core_strbuf_init(mem, &e->content, 32);
    e->type = (uint16_t)tag;
    e->flags = MARKDOWN_CORE_NODE__OPEN;
    e->start_line = start_line;
    e->start_column = start_column;
    e->end_line = start_line;

    return e;
}

// Create a root document node.
static markdown_core_node *make_document(markdown_core_mem *mem) {
    markdown_core_node *e = make_block(mem, MARKDOWN_CORE_NODE_DOCUMENT, 1, 1);
    return e;
}

void markdown_core_parser_touch(markdown_core_parser *parser, markdown_core_node *node) {
    node->stamp = ++parser->write_clock;
}

void markdown_core_parser_mint_block_id(markdown_core_parser *parser, markdown_core_node *node) {
    node->identifier = ++parser->block_ids_minted;
}

/* Appends and reports failure directly: a silent drop on allocation failure
 * would leave an attach that claims success while the extension is missing.
 *
 * Both extension lists hold pointers to the `static const` descriptors that
 * Step 3b made read-only, and every reader casts `data` straight back to a
 * `const markdown_core_syntax_extension *`. The const is discarded here and
 * nowhere else because markdown_core_llist is a generic list that cannot
 * carry it; typing the parameter keeps the cast to this one line. */
static int S_extension_list_append(
    markdown_core_mem *mem,
    markdown_core_llist **head,
    const markdown_core_syntax_extension *extension
) {
    markdown_core_llist *node = (markdown_core_llist *)mem->calloc(1, sizeof(*node));
    markdown_core_llist *tail;
    if (!node) {
        return 0;
    }
    node->data = (void *)(uintptr_t)extension;
    node->next = NULL;
    if (!*head) {
        *head = node;
        return 1;
    }
    for (tail = *head; tail->next; tail = tail->next)
        ;
    tail->next = node;
    return 1;
}

int markdown_core_parser_attach_syntax_extension(
    markdown_core_parser *parser,
    const markdown_core_syntax_extension *extension
) {
    /* Counted before the append so a half-attached extension (the second
     * append lost) still invalidates: the first list DID change. */
    parser->extension_generation++;
    if (!S_extension_list_append(parser->mem, &parser->syntax_extensions, extension)) {
        return 0;
    }
    if (extension->match_inline || extension->insert_inline_from_delim) {
        if (!S_extension_list_append(parser->mem, &parser->inline_syntax_extensions, extension)) {
            return 0;
        }
    }

    return 1;
}

static void markdown_core_parser_dispose(markdown_core_parser *parser) {
    if (parser->root) {
        markdown_core_node_free(parser->root);
    }

    if (parser->refmap) {
        markdown_core_map_free(parser->refmap);
    }

    /* The definition set holds labels and no nodes, so freeing it here cannot
     * reach the tree -- which is the whole difference between it and the map
     * `process_footnotes` used to build (D11). */
    if (parser->footnote_defs) {
        markdown_core_map_free(parser->footnote_defs);
        parser->footnote_defs = NULL;
    }

    /* The normalized source and its line index are per-parse and are released
     * with the rest of it. Requirement 12 is where a document keeps them.
     *
     * `mem` guards the first call: `markdown_core_parser_new_with_mem` reaches
     * here through `reset` on a calloc'd parser, and strbuf_free dereferences
     * the buffer's own allocator. */
    /* The content-to-source map outlives every block that indexes it and
     * nothing else does, so it is released here rather than with the node. */
    parser->mem->free(parser->line_marks);
    parser->line_marks = NULL;
    parser->line_marks_size = 0;
    parser->line_marks_alloc = 0;

    parser->mem->free(parser->tail_queue);
    parser->tail_queue = NULL;
    parser->tail_queue_size = 0;
    parser->tail_queue_alloc = 0;
    parser->mem->free(parser->fresh_queue);
    parser->fresh_queue = NULL;
    parser->fresh_queue_size = 0;
    parser->fresh_queue_alloc = 0;
    parser->mem->free(parser->tail_mask_pool);
    parser->mem->free((void *)parser->tail_name_rows);
    parser->tail_mask_pool = NULL;
    parser->tail_name_rows = NULL;
    parser->tail_name_row_size = 0;
    parser->tail_name_row_alloc = 0;
    parser->tail_mask_words = 0;
    parser->tail_mask_generation = 0;
}

static void markdown_core_parser_reset(markdown_core_parser *parser) {
    markdown_core_llist *saved_exts = parser->syntax_extensions;
    markdown_core_llist *saved_inline_exts = parser->inline_syntax_extensions;
    int saved_options = parser->options;
    bool saved_no_cache = parser->no_projection_cache;
    markdown_core_mem *saved_mem = parser->mem;

    markdown_core_parser_dispose(parser);

    memset(parser, 0, sizeof(markdown_core_parser));
    parser->mem = saved_mem;
    parser->no_projection_cache = saved_no_cache;

    markdown_core_strbuf_init(parser->mem, &parser->curline, 256);
    markdown_core_strbuf_init(parser->mem, &parser->linebuf, 0);

    markdown_core_node *document = make_document(parser->mem);

    parser->refmap = markdown_core_reference_map_new(parser->mem);
    parser->footnote_defs = markdown_core_footnote_definition_map_new(parser->mem);
    parser->root = document;
    parser->current = document;
    if (document) {
        markdown_core_parser_touch(parser, document);
        markdown_core_parser_mint_block_id(parser, document);
    }

    parser->syntax_extensions = saved_exts;
    parser->inline_syntax_extensions = saved_inline_exts;
    parser->options = saved_options;

    /* A reset that could not rebuild its structures poisons the parser: feed
     * becomes a no-op and finish reports failure. */
    if (!parser->root || !parser->refmap || !parser->footnote_defs || parser->curline.oom) {
        parser->oom = true;
    }

    markdown_core_inlines_reset_special_chars(parser);
}

markdown_core_parser *markdown_core_parser_new_with_mem(int options, markdown_core_mem *mem) {
    markdown_core_parser *parser = (markdown_core_parser *)mem->calloc(1, sizeof(markdown_core_parser));
    if (!parser) {
        return NULL;
    }
    parser->mem = mem;
    parser->options = options;
    markdown_core_parser_reset(parser);
    return parser;
}

markdown_core_parser *markdown_core_parser_new(int options) {
    return markdown_core_parser_new_with_mem(options, markdown_core_get_default_mem_allocator());
}

void markdown_core_parser_free(markdown_core_parser *parser) {
    markdown_core_mem *mem = parser->mem;
    markdown_core_parser_dispose(parser);
    markdown_core_strbuf_free(&parser->curline);
    markdown_core_strbuf_free(&parser->linebuf);
    markdown_core_llist_free(parser->mem, parser->syntax_extensions);
    markdown_core_llist_free(parser->mem, parser->inline_syntax_extensions);
    mem->free(parser);
}

static markdown_core_node *finalize(markdown_core_parser *parser, markdown_core_node *b);

/* "This block ends on the line being processed", lifted out of `finalize` so
 * that the extension close path can say the same thing. The three kinds that
 * take it there — the document, a closed fenced code block, a setext heading —
 * are the ones whose last line IS the line in hand; every other block ended on
 * the line before. An extension container closing on its own fence is a fourth,
 * and `finalize` cannot know that from the type alone. */
static void S_set_end_to_current_line(markdown_core_parser *parser, markdown_core_node *b) {
    markdown_core_parser_touch(parser, b);
    b->end_line = parser->line_number;
    b->end_column = parser->curline.size;
    if (b->end_column && parser->curline.ptr[b->end_column - 1] == '\n') {
        b->end_column -= 1;
    }
    if (b->end_column && parser->curline.ptr[b->end_column - 1] == '\r') {
        b->end_column -= 1;
    }
}

// Returns true if line has only space characters, else false.
static bool is_blank(markdown_core_strbuf *s, bufsize_t offset) {
    while (offset < s->size) {
        switch (s->ptr[offset]) {
        case '\r':
        case '\n':
            return true;
        case ' ':
            offset++;
            break;
        case '\t':
            offset++;
            break;
        default:
            return false;
        }
    }

    return true;
}

static MARKDOWN_CORE_INLINE bool extension_accepts_lines(markdown_core_node *node) {
    return node->extension && node->extension->accepts_lines_func &&
           node->extension->accepts_lines_func(node->extension, node) != 0;
}

static MARKDOWN_CORE_INLINE bool accepts_lines(markdown_core_node *node) {
    markdown_core_node_type block_type = S_type(node);

    if (extension_accepts_lines(node)) {
        return true;
    }

    return (
        block_type == MARKDOWN_CORE_NODE_PARAGRAPH || block_type == MARKDOWN_CORE_NODE_HEADING ||
        block_type == MARKDOWN_CORE_NODE_CODE_BLOCK
    );
}

static MARKDOWN_CORE_INLINE bool contains_inlines(markdown_core_node *node) {
    if (node->extension && node->extension->contains_inlines_func) {
        return node->extension->contains_inlines_func(node->extension, node) != 0;
    }

    return (node->type == MARKDOWN_CORE_NODE_PARAGRAPH || node->type == MARKDOWN_CORE_NODE_HEADING);
}

#define MARKDOWN_CORE_MAX_INLINE_DEPTH 256

/* Record where the bytes about to be appended to `node`'s content came from.
 *
 * `column` is a BYTE column counted from 1, which is what every position in
 * the tree is counted in; `parser->column` is not one, because it counts a tab
 * as the several columns it expands to. */
static void S_record_content_mark(markdown_core_parser *parser, markdown_core_node *node, bufsize_t column) {
    markdown_core_line_mark *mark;

    markdown_core_parser_touch(parser, node);
    if (parser->line_marks_size == parser->line_marks_alloc) {
        /* One mark per line, so the doubling never has a realistic ceiling to
         * reach; the guard is here because it is cheaper than reasoning about
         * whether it can. */
        bufsize_t alloc = parser->line_marks_alloc ? parser->line_marks_alloc * 2 : 64;
        markdown_core_line_mark *grown;
        if (parser->line_marks_alloc > (bufsize_t)(INT32_MAX / 2)) {
            parser->oom = true;
            return;
        }
        grown = (markdown_core_line_mark *)
                    parser->mem->realloc(parser->line_marks, (size_t)alloc * sizeof(markdown_core_line_mark));
        if (!grown) {
            parser->oom = true;
            return;
        }
        parser->line_marks = grown;
        parser->line_marks_alloc = alloc;
    }

    if (node->content_mark_count == 0) {
        node->content_mark = (int)parser->line_marks_size;
    } else {
        /* A block's marks are contiguous because only the deepest open block
         * takes lines and opening another one closes it. If that ever stops
         * being true the run below stops describing this block, silently. */
        assert(node->content_mark + node->content_mark_count == (int)parser->line_marks_size);
    }

    mark = &parser->line_marks[parser->line_marks_size++];
    mark->content_offset = node->content.size;
    mark->line = parser->line_number;
    mark->column = (int)column;
    node->content_mark_count++;
}

static void add_line(markdown_core_node *node, markdown_core_chunk *ch, markdown_core_parser *parser) {
    int chars_to_tab;
    int i;
    assert(node->flags & MARKDOWN_CORE_NODE__OPEN);
    markdown_core_parser_touch(parser, node);
    /* Indentation stripped ahead of the content belongs to the CONTAINER that
     * stripped it, not to the block being written into -- the same rule the
     * block openers follow, and for the same reason: a block begins at its own
     * first non-space byte, so a region of its own that started earlier would
     * start before its own scope. The bytes that ARE copied are its content,
     * and the tab below is one of them, because its expansion is what lands in
     * the buffer. */
    if (parser->partially_consumed_tab) {
        /* The spaces below stand for the tail of the tab at parser->offset and
         * have no source bytes of their own, so they are marked against the
         * tab itself and the copied bytes get a mark of their own. */
        S_record_content_mark(parser, node, parser->offset + 1);
        parser->offset += 1; // skip over tab
        // add space characters:
        chars_to_tab = TAB_STOP - (parser->column % TAB_STOP);
        for (i = 0; i < chars_to_tab; i++) {
            markdown_core_strbuf_putc(&node->content, ' ');
        }
    }
    S_record_content_mark(parser, node, parser->offset + 1);
    markdown_core_strbuf_put(&node->content, ch->data + parser->offset, ch->len - parser->offset);
    if (node->content.oom) {
        parser->oom = true;
    }
}

/* Declare that `node`'s content, which was SET rather than fed, begins at
 * (line, column) in the source -- and that it runs on from there without a
 * break.
 *
 * A block whose content the parser copied in line by line gets its marks from
 * `add_line`. A block whose content an extension HANDED it -- a table cell cut
 * out of a row, a directive's label -- has none, and every position inside it
 * then falls back to arithmetic on the block's own start column. One mark is
 * the whole answer for content that is one line long, which is what all of
 * those are.
 *
 * Returns false only when the mark could not be recorded, and the parse is
 * marked lost when that happens: a block with a WRONG map is worse than one
 * with none, because the fallback at least knows it is guessing. */
int markdown_core_parser_mark_content(markdown_core_parser *parser, markdown_core_node *node, int line, int column) {
    markdown_core_line_mark *mark;

    if (!parser || !node) {
        return 0;
    }
    if (parser->line_marks_size == parser->line_marks_alloc) {
        bufsize_t alloc = parser->line_marks_alloc ? parser->line_marks_alloc * 2 : 64;
        markdown_core_line_mark *grown;
        if (parser->line_marks_alloc > (bufsize_t)(INT32_MAX / 2)) {
            parser->oom = true;
            return 0;
        }
        grown = (markdown_core_line_mark *)
                    parser->mem->realloc(parser->line_marks, (size_t)alloc * sizeof(markdown_core_line_mark));
        if (!grown) {
            parser->oom = true;
            return 0;
        }
        parser->line_marks = grown;
        parser->line_marks_alloc = alloc;
    }
    mark = &parser->line_marks[parser->line_marks_size];
    mark->content_offset = 0;
    mark->line = line;
    mark->column = column;
    node->content_mark = (int)parser->line_marks_size++;
    node->content_mark_count = 1;
    return 1;
}

/* Copy the marks covering [from, from + length) of `owner`'s content onto
 * `node`, rebased so the first covers `node`'s own offset zero.
 *
 * For content that is a SLICE of another block's content and more than one line
 * long -- the paragraph a table was split out of -- where one mark would put
 * every line of it on the first line's row. The marks are COPIED and not
 * shared: two nodes naming one run is an alias, and an alias between two trees
 * is the shape §1 records six times.
 */
int markdown_core_parser_adopt_content_marks(
    markdown_core_parser *parser,
    markdown_core_node *owner,
    markdown_core_node *node,
    bufsize_t from,
    bufsize_t length
) {
    int first;
    int last;
    int i;
    int count;

    if (!parser || !owner || !node || owner->content_mark_count <= 0) {
        return 0;
    }
    first = owner->content_mark;
    last = owner->content_mark + owner->content_mark_count - 1;
    while (first < last && parser->line_marks[first + 1].content_offset <= from) {
        first++;
    }
    while (last > first && parser->line_marks[last].content_offset >= from + length) {
        last--;
    }
    count = last - first + 1;

    while (parser->line_marks_size + count > parser->line_marks_alloc) {
        bufsize_t alloc = parser->line_marks_alloc ? parser->line_marks_alloc * 2 : 64;
        markdown_core_line_mark *grown;
        if (parser->line_marks_alloc > (bufsize_t)(INT32_MAX / 2)) {
            parser->oom = true;
            return 0;
        }
        grown = (markdown_core_line_mark *)
                    parser->mem->realloc(parser->line_marks, (size_t)alloc * sizeof(markdown_core_line_mark));
        if (!grown) {
            parser->oom = true;
            return 0;
        }
        parser->line_marks = grown;
        parser->line_marks_alloc = alloc;
    }

    node->content_mark = (int)parser->line_marks_size;
    node->content_mark_count = count;
    for (i = 0; i < count; i++) {
        markdown_core_line_mark copy = parser->line_marks[first + i];
        if (copy.content_offset <= from) {
            copy.column += (int)(from - copy.content_offset);
            copy.content_offset = 0;
        } else {
            copy.content_offset -= from;
        }
        parser->line_marks[parser->line_marks_size++] = copy;
    }
    return 1;
}

/* Requirement 10: for any block with a content buffer and any byte offset
 * within it, name the source line and column of that byte.
 *
 * The answer is a projection of the block's mark run, not a counter anyone
 * maintains: find the slice the offset falls in and add the distance from its
 * start. Binary search, so a caller that asks once per inline node pays
 * log(lines in the block) rather than re-walking it. */
int markdown_core_parser_content_place(
    markdown_core_parser *parser,
    markdown_core_node *node,
    bufsize_t content_offset,
    int *line,
    int *column
) {
    const markdown_core_line_mark *mark;
    int lo, hi;

    if (!parser || !node || node->content_mark_count <= 0 || content_offset < 0) {
        return 0;
    }

    lo = node->content_mark;
    hi = lo + node->content_mark_count - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (parser->line_marks[mid].content_offset <= content_offset) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }

    mark = &parser->line_marks[lo];
    *line = mark->line;
    *column = mark->column + (int)(content_offset - mark->content_offset);
    return 1;
}

/* Drop `dropped` bytes off the FRONT of `node`'s content, leaving `remaining`
 * bytes, and keep the map describing what is left. The marks stay where they are in the vector: the
 * run's head moves past the slices that went away, and the slice the cut
 * landed inside keeps its line with its column advanced to the cut. */
static void S_rebase_content_marks(
    markdown_core_parser *parser,
    markdown_core_node *node,
    bufsize_t dropped,
    bufsize_t remaining
) {
    int i;
    int first = node->content_mark;
    int last = node->content_mark + node->content_mark_count - 1;

    if (node->content_mark_count <= 0 || dropped <= 0) {
        return;
    }

    if (remaining <= 0) {
        /* The cut took everything recorded so far, so no slice survives it and
         * the run is EMPTY rather than one mark advanced past the end of its
         * own line. Keeping the last mark here read as "the block starts on
         * the last line it consumed", which is how a paragraph of nothing but
         * reference definitions came to report a start_line four lines below
         * where it was written -- and, through that, how the region set came
         * to name a node it had already freed. */
        node->content_mark_count = 0;
        return;
    }

    while (first < last && parser->line_marks[first + 1].content_offset <= dropped) {
        first++;
    }

    parser->line_marks[first].column += (int)(dropped - parser->line_marks[first].content_offset);
    parser->line_marks[first].content_offset = 0;
    for (i = first + 1; i <= last; i++) {
        parser->line_marks[i].content_offset -= dropped;
    }
    node->content_mark = first;
    node->content_mark_count = last - first + 1;
}

static void remove_trailing_blank_lines(markdown_core_strbuf *ln) {
    bufsize_t i;
    unsigned char c;

    for (i = ln->size - 1; i >= 0; --i) {
        c = ln->ptr[i];

        if (c != ' ' && c != '\t' && !S_is_line_end_char(c)) {
            break;
        }
    }

    if (i < 0) {
        markdown_core_strbuf_clear(ln);
        return;
    }

    for (; i < ln->size; ++i) {
        c = ln->ptr[i];

        if (!S_is_line_end_char(c)) {
            continue;
        }

        markdown_core_strbuf_truncate(ln, i);
        break;
    }
}

// Check to see if a node ends with a blank line, descending
// if needed into lists and sublists.
static bool S_ends_with_blank_line(markdown_core_node *node) {
    if (S_last_line_checked(node)) {
        return (S_last_line_blank(node));
    } else if ((S_type(node) == MARKDOWN_CORE_NODE_LIST || S_type(node) == MARKDOWN_CORE_NODE_LIST_ITEM) &&
               node->last_child) {
        S_set_last_line_checked(node);
        return (S_ends_with_blank_line(node->last_child));
    } else {
        S_set_last_line_checked(node);
        return (S_last_line_blank(node));
    }
}

/* THE DEFINITION IS A NODE (the rule above `markdown_core_definition`).
 *
 * A link reference definition read off the front of `b`'s content becomes a
 * `ReferenceDefinition` spliced in ahead of `b`, at the byte where its opening
 * bracket was written, owning every byte it read. Upstream drops those bytes
 * into a parser-private map and frees the paragraph that held them; keeping
 * them is what makes the block partition total for a definition-bearing
 * document, and it is why nothing here has to remember that a node was
 * destroyed.
 *
 * `from` and `upto` are offsets into `b`'s content, read BEFORE the harvest
 * drops it, so the content-to-source map still describes them.
 *
 * Q7 and Q26: the destination is REQUIRED. An allocation that loses it fails
 * the parse rather than producing a definition that lies about where it points.
 */
static markdown_core_node *S_new_reference_definition(
    markdown_core_parser *parser,
    markdown_core_node *b,
    bufsize_t from,
    bufsize_t upto,
    const markdown_core_reference_parts *parts
) {
    markdown_core_node *node;
    markdown_core_definition *definition;
    markdown_core_chunk url = parts->url;
    markdown_core_chunk title = parts->title;
    markdown_core_map_key label_key = {NULL, 0};
    int start_line, start_column, end_line, end_column;
    bufsize_t last = upto;
    int lost = 0;

    /* The scope ends at the last byte the definition read that is not a line
     * ending: a definition consumes the line ending that terminates it, and a
     * block's end names its last byte the way every other block's does. */
    while (last > from && S_is_line_end_char(b->content.ptr[last - 1])) {
        last--;
    }
    /* Both refusals below FAIL THE PARSE rather than dropping the definition
     * quietly. The harvest consumes these bytes either way, so a definition
     * that could not be placed is a document missing source the author wrote
     * while the reference map still resolves the label -- which is D30's shape
     * exactly: a wrong document with the failure bit clear. Neither is
     * reachable except through a lost content mark, and that already sets the
     * bit; saying so here is what keeps it true when the map changes. */
    if (last == from) {
        parser->oom = true;
        return NULL;
    }
    if (!markdown_core_parser_content_place(parser, b, from, &start_line, &start_column) ||
        !markdown_core_parser_content_place(parser, b, last - 1, &end_line, &end_column)) {
        parser->oom = true;
        return NULL;
    }

    node = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_REFERENCE_DEFINITION, parser->mem);
    if (!node) {
        parser->oom = true;
        return NULL;
    }
    markdown_core_parser_touch(parser, node);
    /* Born outside `add_child`, so minted here (T2). When the harvest empties
     * the paragraph, the caller hands the firstborn the paragraph's identity
     * instead (§4 D4); this mint is then the id that dies unobserved. */
    markdown_core_parser_mint_block_id(parser, node);
    definition = (markdown_core_definition *)parser->mem->calloc(1, sizeof(*definition));
    if (!definition) {
        parser->oom = true;
        markdown_core_node_free(node);
        return NULL;
    }
    node->as.definition = definition;
    node->start_line = start_line;
    node->start_column = start_column;
    node->end_line = end_line;
    node->end_column = end_column;

    /* The one key construction (#125): normalized here, adopted by the
     * association, and the map registration below copies the association's
     * identifier -- nothing normalizes twice. */
    if (!markdown_core_map_key_init(parser->mem, &label_key, &parts->label, 0, &lost) ||
        !markdown_core_association_init(parser->mem, &definition->association, &parts->label, &label_key)) {
        /* The label would keep borrowing the content buffer the harvest drops. */
        parser->oom = true;
        markdown_core_node_free(node);
        return NULL;
    }
    definition->url = markdown_core_clean_url(parser->mem, &url, &lost);
    definition->title = markdown_core_clean_title(parser->mem, &title, &lost);
    if (lost) {
        parser->oom = true;
        markdown_core_node_free(node);
        return NULL;
    }

    if (!markdown_core_node_insert_before(b, node)) {
        parser->oom = true;
        markdown_core_node_free(node);
        return NULL;
    }
    return node;
}

// returns true if content remains after link defs are resolved.
static bool resolve_reference_link_definitions(markdown_core_parser *parser, markdown_core_node *b) {
    bufsize_t pos;
    markdown_core_strbuf *node_content = &b->content;
    markdown_core_chunk chunk = {node_content->ptr, node_content->size, 0, NULL};
    markdown_core_reference_parts parts;
    markdown_core_node *first_definition = NULL;
    markdown_core_node *registered;
    bufsize_t consumed = 0;
    while (chunk.len && chunk.data[0] == '[' &&
           (pos = markdown_core_parse_reference_inline(parser->mem, &chunk, parser->refmap, &parts))) {
        markdown_core_node *definition = S_new_reference_definition(parser, b, consumed, consumed + pos, &parts);
        if (definition && !first_definition) {
            first_definition = definition;
        }
        consumed += pos;
        chunk.data += pos;
        chunk.len -= pos;
    }
    // The definitions are dropped off the FRONT of the block's content, so what
    // is left starts further down the source than the block was told it did.
    // Without this a paragraph whose leading definitions were consumed keeps the
    // DEFINITION's position, and so does every inline in it, because
    // markdown_core_parse_inlines seeds the subject from b->start_line and
    // b->start_column.
    //
    // D18 corrected the LINE here by counting the line endings in the prefix
    // that goes away, and left the column alone with the note that it was
    // right wherever the remaining first line has the same stripped prefix as
    // the definition's line. Requirement 10 removes both the count and the
    // caveat: the map says where the surviving first byte was written, so the
    // column is answered rather than assumed, and the marks are rebased so the
    // inline phase reads the same map against the shortened buffer.
    bufsize_t dropped = node_content->size - chunk.len;
    int line, column;
    S_rebase_content_marks(parser, b, dropped, chunk.len);
    markdown_core_strbuf_drop(node_content, dropped);
    /* The block now begins where its FIRST SURVIVING line was written, and
     * that is asked of the map rather than derived: this function can be
     * reached twice on one paragraph -- once at the setext-underline check and
     * again at finalize -- and the first call can consume everything recorded
     * so far, leaving the line that carries what is left still unread. Taking
     * the answer from the surviving run rather than from the size of the cut
     * is what makes both arrivals give the same result. On a block with no
     * definitions in front of it this is what the block already said. */
    if (markdown_core_parser_content_place(parser, b, 0, &line, &column)) {
        b->start_line = line;
        b->start_column = column;
    }
    bool has_content = !is_blank(&b->content, 0);
    /* D4's fork 3 (§4): a harvest that empties the paragraph is the reader's
     * text BECOMING the definition, so the firstborn definition takes the
     * paragraph's identity -- the paragraph the consumer watched grow does not
     * die and come back as a stranger. The ids are SWAPPED, not copied: the
     * paragraph leaves with the definition's fresh mint, which nothing has
     * observed -- it is either freed by the caller or, when the line that
     * emptied it here becomes its next content (a `===` after nothing but
     * definitions), it carries on as what it then is: new content. A paragraph
     * that keeps content keeps its id -- the visible text is the element the
     * consumer is tracking -- and its definitions stay fresh births. */
    if (!has_content && first_definition) {
        uint32_t fresh = first_definition->identifier;
        first_definition->identifier = b->identifier;
        b->identifier = fresh;
    }
    /* THE MAP LEARNS THE LABELS LAST, after the identity handoff above, so the
     * identity each entry carries is the one the tree keeps -- registering
     * inside the scan would have stamped the firstborn's unobserved mint. The
     * walk is exactly this call's harvest: `insert_before(b, ...)` placed its
     * definitions in source order, ending at `b`, and an earlier arrival's
     * definitions sit before this call's firstborn. Registration order still
     * matches document order, which is what the entry's `age` tiebreak reads. */
    for (registered = first_definition; registered && registered != b; registered = registered->next) {
        markdown_core_reference_create(
            parser->refmap,
            &registered->as.definition->association.identifier,
            registered->identifier
        );
    }
    return has_content;
}

static markdown_core_node *finalize(markdown_core_parser *parser, markdown_core_node *b) {
    bufsize_t pos;
    markdown_core_node *item;
    markdown_core_node *subitem;
    markdown_core_node *parent;
    bool has_content;

    parent = b->parent;
    assert(b->flags & MARKDOWN_CORE_NODE__OPEN); // shouldn't call finalize on closed blocks
    markdown_core_parser_touch(parser, b);
    b->flags &= ~MARKDOWN_CORE_NODE__OPEN;

    if (parser->curline.size == 0) {
        // end of input - line number has not been incremented
        b->end_line = parser->line_number;
        b->end_column = parser->last_line_length;
    } else if (S_type(b) == MARKDOWN_CORE_NODE_DOCUMENT ||
               (S_type(b) == MARKDOWN_CORE_NODE_CODE_BLOCK && b->as.code.fenced) ||
               (S_type(b) == MARKDOWN_CORE_NODE_HEADING && b->as.heading.setext) ||
               /* D35: a block finalized on the line it OPENED did not end on
                * the previous one. `line_number - 1` below assumes the block
                * was closed by a later line, which is true of every block that
                * needs a following line to end it -- and false of an HTML block
                * of type 2 to 5, whose terminator can be on its own first line.
                * Measured: `<!-- c -->` alone on line 3 gave
                * `HTMLBlock scope=3:1..2:0` for a literal whose last byte is at
                * 3:10, and `last_line_length` there is the length of the BLANK
                * line before it. Four of the eleven negative rows in
                * `specs/scope-sanity/ledger.json` were this. */
               parser->line_number == b->start_line) {
        S_set_end_to_current_line(parser, b);
    } else {
        b->end_line = parser->line_number - 1;
        b->end_column = parser->last_line_length;
    }

    /* The extension's one chance to read its own block as a finished thing.
     * Placed after the scope is settled and before the switch, because what a
     * close hook has to say is about the whole block. */
    if (b->extension && b->extension->close_block_func) {
        b->extension->close_block_func(b->extension, parser, b);
    }

    markdown_core_strbuf *node_content = &b->content;

    switch (S_type(b)) {
    case MARKDOWN_CORE_NODE_PARAGRAPH: {
        has_content = resolve_reference_link_definitions(parser, b);
        if (!has_content) {
            // remove blank node (former reference def)
            markdown_core_node_free(b);
            b = NULL;
        }
        break;
    }

    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        if (!b->as.code.fenced) { // indented code
            remove_trailing_blank_lines(node_content);
            markdown_core_strbuf_putc(node_content, '\n');
        } else {
            // first line of contents becomes info
            for (pos = 0; pos < node_content->size; ++pos) {
                if (S_is_line_end_char(node_content->ptr[pos])) {
                    break;
                }
            }
            assert(pos < node_content->size);

            markdown_core_strbuf tmp = MARKDOWN_CORE_BUF_INIT(parser->mem);
            houdini_unescape_html_f(&tmp, node_content->ptr, pos);
            markdown_core_strbuf_trim(&tmp);
            markdown_core_strbuf_unescape(&tmp);
            /* WHETHER THE SOURCE WROTE AN INFO STRING IS DECIDED HERE, ONCE.
             * A fence with nothing but whitespace after it wrote none, and
             * this is the only place that still knows the difference between
             * that and the `js` in ```` ```js ````. The facade used to decide
             * it again by testing the length, which is the fold requirement 14
             * forbids. */
            if (tmp.oom) {
                /* THE SWEEP FOUND THIS. A buffer that could not be grown has
                 * `size == 0` and it is NOT an absent info string -- it is an
                 * info string the parse lost, and reporting absence here made
                 * `fallback_runner` see a lossy document returned as a
                 * success. `markdown_core_chunk_buf_detach` used to carry the
                 * distinction for free by answering NULL; splitting the
                 * length test out from it dropped that, so the length test
                 * has to ask about the loss first. */
                parser->oom = true;
                markdown_core_strbuf_free(&tmp);
                b->as.code.info = markdown_core_optional_chunk_absent();
            } else if (tmp.size == 0) {
                markdown_core_strbuf_free(&tmp);
                b->as.code.info = markdown_core_optional_chunk_absent();
            } else {
                markdown_core_chunk info = markdown_core_chunk_buf_detach(&tmp);
                if (!info.data) {
                    parser->oom = true;
                }
                b->as.code.info = markdown_core_optional_chunk_present(info);
            }

            if (node_content->ptr[pos] == '\r') {
                pos += 1;
            }
            if (node_content->ptr[pos] == '\n') {
                pos += 1;
            }
            markdown_core_strbuf_drop(node_content, pos);
        }
        b->as.code.literal = markdown_core_chunk_buf_detach(node_content);
        if (!b->as.code.literal.data) {
            parser->oom = true;
        }
        break;

    case MARKDOWN_CORE_NODE_HTML_BLOCK:
        b->as.literal = markdown_core_chunk_buf_detach(node_content);
        if (!b->as.literal.data) {
            parser->oom = true;
        }
        break;

    case MARKDOWN_CORE_NODE_LIST: // determine tight/loose status
        b->as.list.tight = true;  // tight by default
        item = b->first_child;

        while (item) {
            // check for non-final non-empty list item ending with blank line:
            if (S_last_line_blank(item) && item->next) {
                b->as.list.tight = false;
                break;
            }
            // recurse into children of list item, to see if there are
            // spaces between them:
            subitem = item->first_child;
            while (subitem) {
                if ((item->next || subitem->next) && S_ends_with_blank_line(subitem)) {
                    b->as.list.tight = false;
                    break;
                }
                subitem = subitem->next;
            }
            if (!(b->as.list.tight)) {
                break;
            }
            item = item->next;
        }

        break;

    default:
        break;
    }

    /* No freeze here (#153, revised by measurement): freezing every block at
     * finalize taxed the one-shot path a header allocation per block and an
     * atomic per literal for a benefit only derivations see -- about +1.5%
     * on deep_nesting@32768 with the allocator's arena policy held fixed.
     * (The +54% wall-clock spike that first flagged this line was glibc's
     * dynamic trim heuristic refaulting the arena, not the freeze; see
     * ts_bench_pin_allocator.) The freeze is LAZY instead: the first
     * derivation that clones a closed block freezes its content there, so a
     * session pays exactly when it shares and `finish` parses in place over
     * plain strbufs, as it always did. */

    return parent;
}

// Add a node as child of another.  Return pointer to child.
static markdown_core_node *add_child(
    markdown_core_parser *parser,
    markdown_core_node *parent,
    markdown_core_node_type block_type,
    int start_column
) {
    assert(parent);

    // if 'parent' isn't the kind of node that can accept this child,
    // then back up til we hit a node that can.
    while (!markdown_core_node_can_contain_type(parent, block_type)) {
        parent = finalize(parser, parent);
    }
    /* The climb can close `parser->current` (a leaf container -- a
     * paragraph -- being interrupted IS the current block). In normal flow
     * `add_text_to_container` re-anchors it at the end of the line, and its
     * identity comparisons need the stale pointer, so it must not be
     * touched here. But since #153 `finalize` allocates (the content
     * freeze), an OOM raised inside the climb makes `S_process_line` bail
     * before that tail runs -- leaving `current` on a closed block for the
     * finish walk to re-finalize. Only then anchor it at the still-open
     * ancestor the climb stopped at, exactly as the make_block failure
     * below does. */
    if (parser->oom && !(parser->current->flags & MARKDOWN_CORE_NODE__OPEN)) {
        parser->current = parent;
    }

    markdown_core_node *child = make_block(parser->mem, block_type, parser->line_number, start_column);
    if (!child || child->content.oom) {
        parser->oom = true;
        if (child) {
            markdown_core_node_free(child);
        }
        /* The loop above may have finalized blocks; keep the parser anchored
         * at a still-open ancestor so the finish path stays consistent. */
        parser->current = parent;
        return NULL;
    }
    child->parent = parent;
    markdown_core_parser_touch(parser, parent);
    markdown_core_parser_touch(parser, child);
    markdown_core_parser_mint_block_id(parser, child);

    if (parent->last_child) {
        parent->last_child->next = child;
        child->prev = parent->last_child;
    } else {
        parent->first_child = child;
        child->prev = NULL;
    }
    parent->last_child = child;
    return child;
}

/* Two of the three byte sets are folded into parser tables here; the third,
 * `dispatch`, is asked directly because it also answers ownership questions
 * that a merged table cannot. The two folds are now independent, which is the
 * whole point: before this, one list fed both tables and whether it fed the
 * second was a single `emphasis` bool covering every byte the extension named.
 * That is D1. */
void markdown_core_manage_extensions_special_characters(markdown_core_parser *parser, int add) {
    markdown_core_llist *tmp_ext;

    for (tmp_ext = parser->inline_syntax_extensions; tmp_ext; tmp_ext = tmp_ext->next) {
        const markdown_core_syntax_extension *ext = (const markdown_core_syntax_extension *)tmp_ext->data;
        const unsigned char *c;

        for (c = (const unsigned char *)ext->terminates_text; c && *c; c++) {
            if (add) {
                markdown_core_inlines_add_text_terminator(parser, *c);
            } else {
                markdown_core_inlines_remove_text_terminator(parser, *c);
            }
        }
        for (c = (const unsigned char *)ext->flanking_transparent; c && *c; c++) {
            if (add) {
                markdown_core_inlines_add_flanking_transparent(parser, *c);
            } else {
                markdown_core_inlines_remove_flanking_transparent(parser, *c);
            }
        }
    }
}

// Walk through node and all children, recursively, parsing
/* THE PER-BLOCK TAIL (T18). What `finish` always ran over the whole tree
 * after the inline parse -- consolidation, the extension postprocessors, the
 * comment strip -- runs here over ONE block, in the order the whole-tree
 * passes had: parse -> consolidate -> each declared hook in attach order ->
 * strip. The order across blocks is free, because no pass moves a node to
 * another parent (F15); the order within a block is not -- stripping before
 * autolink turns `a<!-- x -->@b.com` into a link, and stripping before
 * formula promotes `$$x$$<!-- c -->`.
 *
 * WHICH HALF A CACHE HIT SKIPS -- F15 rule 2, re-resolved under retention
 * (D9, F24): a DERIVE hit skips the WHOLE tail. The retained node itself is
 * served, every pass's effect -- consolidation, the `"*inlines"` hooks, the
 * comment strip, and a name hook's node-level work -- baked in at the
 * projection that RECORDED it, and the node a name hook would be offered is
 * frozen for every tree at once. The SEAL's hit runs the NAME hooks once
 * more (review-found): finish hands back the CST shell borrowing the
 * stored children, and only the hooks can reproduce their node-level work
 * on that shell -- the children they must not touch are frozen. What keeps
 * a replacing hook per-projection is the STORE, not the dispatch: a
 * replacement carries no ORIGIN, so a `PARAGRAPH` around a standalone
 * formula is a fresh paragraph on every projection, and only the hook makes
 * it the `FormulaBlock` five gates pin. `syntax_extension.h` states the
 * contract. */

static const char S_INLINES_MEMBER[] = "*inlines";

/* The projection cache (T9), defined beside the clone that takes its hits. */
static bool S_cache_fresh(markdown_core_parser *parser, const markdown_core_node *block, markdown_core_map *refmap);
static void S_cache_store(markdown_core_parser *parser, markdown_core_node *node);

/* The inline ordinals (T2), defined beside the tail that assigns them. */
static bool S_has_inline_child(markdown_core_node *block);

static bool S_set_names(const char *set, const char *name) {
    const char *p;
    for (p = set; *p; p += strlen(p) + 1) {
        if (strcmp(p, name) == 0) {
            return true;
        }
    }
    return false;
}

/* Rebuild the tail filter's rows (parser.h) for the extension set as it now
 * stands: bit `i` -- word i/64, bit i%64 -- speaks for the `i`th attached
 * extension, in list order, the order the tail offers blocks (F15 rule 1).
 * The name-keyed rows are cleared and refilled lazily by `S_names_row`;
 * only the fixed `"*inlines"` row (pool row 0) is computed here. An
 * extension with no hook or no declared set gets no bit anywhere: it is
 * never offered, exactly as `S_set_names` never matched it. A rebuild that
 * cannot allocate poisons the parse -- the same answer every other lost
 * allocation gives -- and leaves the table empty, so nothing dereferences
 * a half-built pool. */
static void S_tail_masks_rebuild(markdown_core_parser *parser) {
    markdown_core_llist *extensions;
    size_t count = 0;
    size_t words;
    size_t idx = 0;
    for (extensions = parser->syntax_extensions; extensions; extensions = extensions->next) {
        count++;
    }
    words = count ? (count + 63) / 64 : 1;
    if (words != parser->tail_mask_words || !parser->tail_mask_pool) {
        size_t row_alloc = parser->tail_name_row_alloc ? parser->tail_name_row_alloc : 8;
        uint64_t *pool =
            (uint64_t *)parser->mem->realloc(parser->tail_mask_pool, (1 + row_alloc) * words * sizeof(*pool));
        const char **names = parser->tail_name_rows
                                 ? parser->tail_name_rows
                                 : (const char **)parser->mem->realloc(NULL, row_alloc * sizeof(*names));
        if (!pool || !names) {
            parser->mem->free(pool ? pool : parser->tail_mask_pool);
            if (names && !parser->tail_name_rows) {
                parser->mem->free((void *)names);
            }
            parser->tail_mask_pool = NULL;
            parser->tail_name_row_size = 0;
            parser->tail_mask_generation = parser->extension_generation + 1;
            parser->oom = true;
            return;
        }
        parser->tail_mask_pool = pool;
        parser->tail_name_rows = names;
        parser->tail_name_row_alloc = row_alloc;
        parser->tail_mask_words = words;
    }
    memset(parser->tail_mask_pool, 0, parser->tail_mask_words * sizeof(*parser->tail_mask_pool));
    parser->tail_name_row_size = 0;
    for (extensions = parser->syntax_extensions; extensions; extensions = extensions->next, idx++) {
        const markdown_core_syntax_extension *ext = (const markdown_core_syntax_extension *)extensions->data;
        if (ext->postprocess_block_func && ext->postprocess_blocks &&
            S_set_names(ext->postprocess_blocks, S_INLINES_MEMBER)) {
            parser->tail_mask_pool[idx >> 6] |= (uint64_t)1 << (idx & 63);
        }
    }
    parser->tail_mask_generation = parser->extension_generation + 1;
}

static MARKDOWN_CORE_INLINE void S_tail_masks_fresh(markdown_core_parser *parser) {
    if (parser->tail_mask_generation != parser->extension_generation + 1) {
        S_tail_masks_rebuild(parser);
    }
}

static MARKDOWN_CORE_INLINE const uint64_t *S_inlines_row(markdown_core_parser *parser) {
    return parser->tail_mask_pool;
}

static MARKDOWN_CORE_INLINE bool S_row_test(const uint64_t *row, size_t idx) {
    return row != NULL && (row[idx >> 6] >> (idx & 63) & 1) != 0;
}

/* Which extensions declared `name`? The rows are keyed on the name's
 * POINTER, and the name is a function of the NODE rather than of its type:
 * a `LIST_ITEM` carrying tasklist answers "tasklist" and a plain one
 * "list_item"; a `TABLE_ROW` answers "table_header" or "table_row". Keyed
 * on the type both would be wrong; keyed on the name there is nothing to
 * special-case. A literal with the same bytes at another address misses
 * once and takes its own row. The table GROWS on demand -- pool and names
 * together -- so every name and every extension follows this one path;
 * NULL only when an allocation was lost, which poisoned the parse. */
static const uint64_t *S_names_row(markdown_core_parser *parser, const char *name) {
    markdown_core_llist *extensions;
    size_t words = parser->tail_mask_words;
    uint64_t *row;
    size_t idx = 0;
    size_t i;
    if (!parser->tail_mask_pool) {
        return NULL;
    }
    for (i = 0; i < parser->tail_name_row_size; i++) {
        if (parser->tail_name_rows[i] == name) {
            /* Move-to-front by one: a projection asks about the same few
             * names in runs, so the steady state is an early probe. */
            if (i > 0) {
                uint64_t *above = parser->tail_mask_pool + i * words;
                uint64_t *here = above + words;
                const char *swap_name = parser->tail_name_rows[i - 1];
                size_t w;
                for (w = 0; w < words; w++) {
                    uint64_t swap_word = above[w];
                    above[w] = here[w];
                    here[w] = swap_word;
                }
                parser->tail_name_rows[i - 1] = name;
                parser->tail_name_rows[i] = swap_name;
                i--;
            }
            return parser->tail_mask_pool + (1 + i) * words;
        }
    }
    if (parser->tail_name_row_size == parser->tail_name_row_alloc) {
        size_t grown = parser->tail_name_row_alloc * 2;
        uint64_t *pool = (uint64_t *)parser->mem->realloc(parser->tail_mask_pool, (1 + grown) * words * sizeof(*pool));
        const char **names;
        if (!pool) {
            parser->oom = true;
            return NULL;
        }
        parser->tail_mask_pool = pool;
        names = (const char **)parser->mem->realloc((void *)parser->tail_name_rows, grown * sizeof(*names));
        if (!names) {
            parser->oom = true;
            return NULL;
        }
        parser->tail_name_rows = names;
        parser->tail_name_row_alloc = grown;
    }
    i = parser->tail_name_row_size;
    row = parser->tail_mask_pool + (1 + i) * words;
    memset(row, 0, words * sizeof(*row));
    for (extensions = parser->syntax_extensions; extensions; extensions = extensions->next, idx++) {
        const markdown_core_syntax_extension *ext = (const markdown_core_syntax_extension *)extensions->data;
        if (ext->postprocess_block_func && ext->postprocess_blocks && S_set_names(ext->postprocess_blocks, name)) {
            row[idx >> 6] |= (uint64_t)1 << (idx & 63);
        }
    }
    parser->tail_name_rows[i] = name;
    parser->tail_name_row_size++;
    return row;
}

static MARKDOWN_CORE_INLINE bool S_row_any(const markdown_core_parser *parser, const uint64_t *row) {
    size_t w;
    if (!row) {
        return false;
    }
    for (w = 0; w < parser->tail_mask_words; w++) {
        if (row[w]) {
            return true;
        }
    }
    return false;
}

/* Asked at the block's EXIT, so a block nothing will touch is never queued. */
static bool S_block_has_tail_work(markdown_core_parser *parser, markdown_core_node *block) {
    if (contains_inlines(block)) {
        return true;
    }
    /* A block that is not an inline container can still OWN inline-class
     * children -- a directive block's CST-resident label -- and their
     * ordinals (T2) are assigned in the tail. */
    if (S_has_inline_child(block)) {
        return true;
    }
    if ((parser->options & MARKDOWN_CORE_OPT_STRIP_HTML_COMMENTS) && S_type(block) == MARKDOWN_CORE_NODE_HTML_BLOCK) {
        return true;
    }
    /* Only the name clause is left to ask: the `"*inlines"` offer needs
     * inline content, and a block with any already answered above. */
    S_tail_masks_fresh(parser);
    return S_row_any(parser, S_names_row(parser, markdown_core_node_get_type_string(block)));
}

static bool S_tail_queue_push(markdown_core_parser *parser, markdown_core_node *block) {
    if (parser->tail_queue_size == parser->tail_queue_alloc) {
        size_t grown = parser->tail_queue_alloc ? parser->tail_queue_alloc * 2 : 64;
        markdown_core_node **queue =
            (markdown_core_node **)parser->mem->realloc(parser->tail_queue, grown * sizeof(*queue));
        if (!queue) {
            return false;
        }
        parser->tail_queue = queue;
        parser->tail_queue_alloc = grown;
    }
    parser->tail_queue[parser->tail_queue_size++] = block;
    return true;
}

static bool S_fresh_queue_push(markdown_core_parser *parser, markdown_core_node *block) {
    if (parser->fresh_queue_size == parser->fresh_queue_alloc) {
        size_t grown = parser->fresh_queue_alloc ? parser->fresh_queue_alloc * 2 : 64;
        markdown_core_node **queue =
            (markdown_core_node **)parser->mem->realloc(parser->fresh_queue, grown * sizeof(*queue));
        if (!queue) {
            return false;
        }
        parser->fresh_queue = queue;
        parser->fresh_queue_alloc = grown;
    }
    parser->fresh_queue[parser->fresh_queue_size++] = block;
    return true;
}

/* THE INLINE ORDINALS (T2, §4 D4). An inline's identity is its pre-order
 * ordinal among the owning block's inline descendants: unique within the
 * block -- so unique within any sibling list a consumer iterates -- and the
 * pair (block's identity, ordinal) is unique in the document. Assigned at the
 * end of the block's tail, after every pass that creates, merges or removes
 * inline nodes (consolidation, the hooks, the strip), so the numbering is a
 * function of the finished list; the parse is deterministic, so two
 * projections of one unwritten CST number every inline identically, and the
 * cache stores the list numbered, so a hit serves the same identities without
 * a write. Only an OWNED list is numbered -- a borrowed one already carries
 * its numbers and must not be written (F22). */
static void S_number_inline_descendants(markdown_core_node *block) {
    markdown_core_child_cursor cursor;
    markdown_core_node *top = markdown_core_child_first(block, &cursor);
    uint32_t ordinal = 0;
    /* TWO LOOPS, ONE PER TRUST LEVEL (#161, D9). A TOP-LEVEL child is
     * stepped through the cursor and never dereferenced for its links: a
     * block sibling may already be the stored, parentless SHARED node --
     * its own tail ran first, post-order -- and asking it for a parent or
     * a next is exactly the per-tree question it no longer answers. An
     * INTERIOR node is parse-built under an inline top child this walk
     * owns, so its parents are whole and the climb stops at `top`. */
    while (top) {
        /* A nested BLOCK owns its own namespace: its identity is its mint
         * and its inlines are numbered at its own tail. The one block that
         * mixes child classes is a directive block, whose label is
         * inline-class and CST-resident; the label and its parsed content
         * are what this skip leaves in THIS block's namespace. */
        if (!MARKDOWN_CORE_NODE_BLOCK_P(top)) {
            markdown_core_node *cur = top;
            for (;;) {
                cur->identifier = ++ordinal;
                /* The pair's other half, stamped here because this is the
                 * one moment anything stands inside the block and beside
                 * the inline at once: a shared child list carries no parent
                 * to climb (T19), so an inline that did not learn its owner
                 * here could never answer it. */
                cur->owner = block->identifier;
                if (cur->first_child) {
                    cur = cur->first_child;
                    continue;
                }
                while (cur != top && !cur->next) {
                    cur = cur->parent;
                }
                if (cur == top) {
                    break;
                }
                cur = cur->next;
            }
        }
        top = markdown_core_child_after(block, top, &cursor);
    }
}

/* Does the block hold any inline-class child of its own -- a parsed inline
 * list, or a CST-resident inline construct like a directive's label? This is
 * what obliges a tail: the inline ordinals above are assigned there. */
static bool S_has_inline_child(markdown_core_node *block) {
    markdown_core_child_cursor cursor;
    markdown_core_node *child;
    for (child = markdown_core_child_first(block, &cursor); child;
        child = markdown_core_child_after(block, child, &cursor)) {
        if (!MARKDOWN_CORE_NODE_BLOCK_P(child)) {
            return true;
        }
    }
    return false;
}

/* One block's tail. `*block` comes back reseated or NULL exactly as a hook
 * leaves it, so the caller can tell a replaced root from a removed one. */
static void S_run_block_tail(markdown_core_parser *parser, markdown_core_node **block) {
    markdown_core_llist *extensions;
    markdown_core_node *node = *block;
    bool children_own = !MARKDOWN_CORE_NODE_BORROWED_P(node);
    const uint64_t *name_row;
    bool offer_inlines;
    size_t idx;

    S_tail_masks_fresh(parser);

    if (children_own && contains_inlines(node)) {
        if (!markdown_core_consolidate_text_nodes(node)) {
            parser->oom = true;
        }
    }

    /* The offer set is a function of the node that STANDS there (F15 rule
     * 1), so it is recomputed after every hook that ran -- a hook may
     * replace the node -- and left alone across the extensions that were
     * not offered, which touched nothing. */
    name_row = S_names_row(parser, markdown_core_node_get_type_string(node));
    offer_inlines = children_own && contains_inlines(node);
    for (extensions = parser->syntax_extensions, idx = 0; extensions; extensions = extensions->next, idx++) {
        const markdown_core_syntax_extension *ext = (const markdown_core_syntax_extension *)extensions->data;
        if (!S_row_test(name_row, idx) && !(offer_inlines && S_row_test(S_inlines_row(parser), idx))) {
            continue;
        }
        ext->postprocess_block_func(ext, parser, &node);
        if (!node) {
            *block = NULL;
            return;
        }
        children_own = !MARKDOWN_CORE_NODE_BORROWED_P(node);
        name_row = S_names_row(parser, markdown_core_node_get_type_string(node));
        offer_inlines = children_own && contains_inlines(node);
    }

    if (parser->options & MARKDOWN_CORE_OPT_STRIP_HTML_COMMENTS) {
        if (S_type(node) == MARKDOWN_CORE_NODE_HTML_BLOCK) {
            /* The predicate, then the removal -- never a strip rooted at a
             * block that is itself the comment (F13 requirement 3). */
            if (S_html_literal_starts_with_comment(node)) {
                markdown_core_node_free(node);
                node = NULL;
            }
        } else if (children_own && contains_inlines(node)) {
            if (!S_strip_inline_comments(node)) {
                parser->oom = true;
            }
        }
    }

    /* THE CST-RESIDENT INLINE CHILD -- a directive's label. Inline-class, so
     * the walk never queues it, and its list was silently missing every
     * content pass the whole-tree tail used to give it: consolidation, the
     * "*inlines" hooks, the strip (found on the landing review -- an
     * unmatched `*` stayed three TEXT nodes, a `www.` never became a link).
     * The passes run HERE, from the block that owns it, in the order the
     * block's own list gets them and before the shared numbering. A label is
     * never enrolled in the cache -- the clone leaves it unmarked -- so its
     * children are always its own. */
    if (node && children_own && !contains_inlines(node) && S_has_inline_child(node)) {
        markdown_core_child_cursor label_cursor;
        markdown_core_node *child;
        for (child = markdown_core_child_first(node, &label_cursor); child;
            child = markdown_core_child_after(node, child, &label_cursor)) {
            markdown_core_node *content = child;
            if (MARKDOWN_CORE_NODE_BLOCK_P(child) || !contains_inlines(child)) {
                continue;
            }
            if (!markdown_core_consolidate_text_nodes(child)) {
                parser->oom = true;
            }
            for (extensions = parser->syntax_extensions, idx = 0; extensions; extensions = extensions->next, idx++) {
                const markdown_core_syntax_extension *ext = (const markdown_core_syntax_extension *)extensions->data;
                if (!S_row_test(S_inlines_row(parser), idx)) {
                    continue;
                }
                /* An "*inlines" pass rewrites CHILDREN and neither replaces
                 * nor removes the node it is rooted at (F15 rule 2). */
                ext->postprocess_block_func(ext, parser, &content);
            }
            if ((parser->options & MARKDOWN_CORE_OPT_STRIP_HTML_COMMENTS) && !S_strip_inline_comments(child)) {
                parser->oom = true;
            }
        }
    }

    if (node && children_own && S_has_inline_child(node)) {
        S_number_inline_descendants(node);
    }

    /* THE STORE, last, so the list the cache keeps is the one the tail
     * finished with -- and the numbering above rides into it. A derived block
     * that was replaced took its origin with it, and a block with no inline
     * content never had one. */
    if (node && (node->flags & MARKDOWN_CORE_NODE__ORIGIN)) {
        if (children_own && contains_inlines(node)) {
            S_cache_store(parser, node);
        } else {
            node->link.origin = NULL;
            node->flags &= ~MARKDOWN_CORE_NODE__ORIGIN;
        }
    }
    *block = node;
}

/* Drain the queue in the order the walk filled it. A hook may replace or
 * remove the block it is handed and nothing else, so no entry behind it
 * dangles. The root is written back if a hook reseated it; a hook that
 * removed the root leaves NULL, which the projection then answers. */
static void S_run_block_tails(markdown_core_parser *parser, markdown_core_node **root) {
    size_t i;
    for (i = 0; i < parser->tail_queue_size; i++) {
        markdown_core_node *block = parser->tail_queue[i];
        bool is_root = block == *root;
        S_run_block_tail(parser, &block);
        if (is_root) {
            *root = block;
        }
    }
    parser->tail_queue_size = 0;
}

// string content into inline content where appropriate. `root` is a stated
// CST: a DERIVED block skeleton for a re-projection, or `parser->root` itself
// for the one projection that ends the parser's life (`finish`, T1). Every
// block with tail work is queued at its EXIT for `S_run_block_tails`.
static void process_inlines(
    markdown_core_parser *parser,
    markdown_core_node *root,
    markdown_core_map *refmap,
    int options
) {
    markdown_core_iter walk;
    markdown_core_iter *iter = &walk;
    markdown_core_node *cur;
    markdown_core_event_type ev_type;

    markdown_core_iter_init(iter, root);

    markdown_core_manage_extensions_special_characters(parser, true);

    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        cur = markdown_core_iter_get_node(iter);
        if (ev_type == MARKDOWN_CORE_EVENT_ENTER) {
            if (cur->flags & MARKDOWN_CORE_NODE__SHARED) {
                /* THE RETAINED NODE IS FINISHED WORK (#161, D9): parse,
                 * consolidation, hooks, strip and numbering all ran before
                 * its store, so the walk neither parses, queues, nor enters
                 * it -- F22's rule, upgraded from "never write" to "never
                 * even look". */
                markdown_core_iter_skip_children(iter);
                continue;
            }
            if (!contains_inlines(cur)) {
                continue;
            }
            if (!MARKDOWN_CORE_NODE_BORROWED_P(cur) && S_cache_fresh(parser, cur, refmap)) {
                /* `finish` projects the CST in place (T1): the block IS its
                 * own origin, and the cache's hold becomes the borrow's.
                 * The content settles FIRST (#153): a borrower's bytes alias
                 * a frozen buffer (node_check), and a block whose recording
                 * projection ran while it was still open -- a setext
                 * heading's, say -- reaches this hit with a live arena the
                 * lazy freeze never saw. Finalize has run, so the arena is
                 * settled and the freeze is the usual O(1) detach; a failed
                 * freeze loses the bytes and poisons the parse, and finish
                 * answers NULL for it below -- no fallback persists. */
                if (!cur->frozen_content && !(cur->flags & MARKDOWN_CORE_NODE__OPEN)) {
                    if (cur->content.size) {
                        cur->frozen_content = markdown_core_buf_freeze(&cur->content);
                        if (cur->frozen_content) {
                            cur->content.ptr = cur->frozen_content->bytes;
                            cur->content.size = cur->frozen_content->size;
                            cur->content.asize = 0;
                        } else {
                            parser->oom = true;
                        }
                    } else if (cur->content.asize) {
                        /* An EMPTY heading's trim left an allocated scratch
                         * arena behind; a borrower owns no arena, and the
                         * reset points the strbuf at the static empty
                         * buffer, so readers still see "". */
                        markdown_core_strbuf_free(&cur->content);
                    }
                }
                cur->first_child = cur->link.holder->first_child;
                cur->last_child = cur->link.holder->last_child;
                cur->flags &= ~MARKDOWN_CORE_NODE__CACHE_OWNER;
                parser->cache_hits++;
            }
            if (MARKDOWN_CORE_NODE_BORROWED_P(cur)) {
                /* A BORROWER'S LIST IS NEITHER PARSED NOR ENTERED (F22). The
                 * list is the cache's, and it is already there at this ENTER
                 * -- unlike a block's own inlines, which are parsed below
                 * after the lookahead was taken -- so the walk would descend
                 * into it and meet a directive's label, an inline-class node
                 * that `contains_inlines` claims, and parse it AGAIN into the
                 * shared list.
                 *
                 * The block IS queued, exactly once (review-found, twice
                 * over): the seal is the one projection that cannot serve a
                 * hit by identity -- it hands back this CST shell, whose
                 * node-level state a name hook may have changed on the
                 * DERIVED clone before the store (a retype, a level) --
                 * so the name hooks run here once to reproduce that state
                 * on the shell. Suppressing them lost the cached node-level
                 * mutation at the seal; queueing here AND at the EXIT ran
                 * them twice. The children stay the stored list either way:
                 * every node in it carries SHARED, so a hook that reaches
                 * into them meets the frozen-projection surface. Derive
                 * hits stay hook-free: there the retained node itself is
                 * the answer, mutation baked in. */
                if (S_block_has_tail_work(parser, cur) && !S_tail_queue_push(parser, cur)) {
                    parser->oom = true;
                }
                markdown_core_iter_skip_children(iter);
                continue;
            }
            markdown_core_parse_inlines(parser, cur, refmap, options);
        } else if (MARKDOWN_CORE_NODE_BLOCK_P(cur) && !(cur->flags & MARKDOWN_CORE_NODE__SHARED) &&
                   !MARKDOWN_CORE_NODE_BORROWED_P(cur) && S_block_has_tail_work(parser, cur)) {
            /* COLLECTED, NOT ACTED ON: a hook may replace or remove the block
             * and the walk is standing on it (F13 requirement 2). The walk
             * never descends into a block's own inlines -- its lookahead was
             * taken before they were parsed -- and is reset past a borrowed
             * list above, so this EXIT follows the ENTER directly and the
             * queue is the blocks in post-order.
             *
             * A SHARED block never queues (measured, 2026-09-01), and a
             * BORROWER never queues either (review-found, the hook_once
             * gate): skip_children above still delivers each one's EXIT,
             * and without the two tests every hit re-answered the name
             * rows and re-ran its hooks -- the retained blocks on every
             * feed of a derive, half the Ir of a width-heavy stream, and
             * the borrowers again at the seal, where a counting hook
             * watched finish disagree with derive_tree. The no-op is
             * provable, not incidental: a block a name hook would have
             * replaced was never stored (the replacement carries no
             * ORIGIN), so a stored block's hooks have nothing to say, and
             * a hook that would write anyway meets the frozen-projection
             * surface. */
            if (!S_tail_queue_push(parser, cur)) {
                parser->oom = true;
            }
        }
    }

    if (walk.oom) {
        /* A refused spill truncated the walk (iterator.h): blocks were
         * skipped unparsed and unqueued, so the projection is not all
         * there and the parse says so. */
        parser->oom = true;
    }
    markdown_core_manage_extensions_special_characters(parser, false);
}

// Attempts to parse a list item marker (bullet or enumerated).
// On success, returns length of the marker, and populates
// data with the details.  On failure, returns 0.
static bufsize_t parse_list_marker(
    markdown_core_parser *parser,
    markdown_core_chunk *input,
    bufsize_t pos,
    bool interrupts_paragraph,
    markdown_core_list **dataptr
) {
    markdown_core_mem *mem = parser->mem;
    unsigned char c;
    bufsize_t startpos;
    markdown_core_list *data;
    bufsize_t i;

    startpos = pos;
    c = peek_at(input, pos);

    if (c == '*' || c == '-' || c == '+') {
        pos++;
        if (!markdown_core_isspace(peek_at(input, pos))) {
            return 0;
        }

        if (interrupts_paragraph) {
            i = pos;
            // require non-blank content after list marker:
            while (S_is_space_or_tab(peek_at(input, i))) {
                i++;
            }
            if (peek_at(input, i) == '\n') {
                return 0;
            }
        }

        data = (markdown_core_list *)mem->calloc(1, sizeof(*data));
        if (!data) {
            /* Allocation loss, not an invalid marker. */
            parser->oom = true;
            return 0;
        }
        data->marker_offset = 0; // will be adjusted later
        data->list_type = MARKDOWN_CORE_BULLET_LIST;
        data->bullet_char = c;
        data->start = 0;
        data->delimiter = MARKDOWN_CORE_NO_DELIM;
        data->tight = false;
    } else if (markdown_core_isdigit(c)) {
        int start = 0;
        int digits = 0;

        do {
            start = (10 * start) + (peek_at(input, pos) - '0');
            pos++;
            digits++;
            // We limit to 9 digits to avoid overflow,
            // assuming max int is 2^31 - 1
            // This also seems to be the limit for 'start' in some browsers.
        } while (digits < 9 && markdown_core_isdigit(peek_at(input, pos)));

        if (interrupts_paragraph && start != 1) {
            return 0;
        }
        c = peek_at(input, pos);
        if (c == '.' || c == ')') {
            pos++;
            if (!markdown_core_isspace(peek_at(input, pos))) {
                return 0;
            }
            if (interrupts_paragraph) {
                // require non-blank content after list marker:
                i = pos;
                while (S_is_space_or_tab(peek_at(input, i))) {
                    i++;
                }
                if (S_is_line_end_char(peek_at(input, i))) {
                    return 0;
                }
            }

            data = (markdown_core_list *)mem->calloc(1, sizeof(*data));
            if (!data) {
                parser->oom = true;
                return 0;
            }
            data->marker_offset = 0; // will be adjusted later
            data->list_type = MARKDOWN_CORE_ORDERED_LIST;
            data->bullet_char = 0;
            data->start = start;
            data->delimiter = (c == '.' ? MARKDOWN_CORE_PERIOD_DELIM : MARKDOWN_CORE_PAREN_DELIM);
            data->tight = false;
        } else {
            return 0;
        }
    } else {
        return 0;
    }

    *dataptr = data;
    return (pos - startpos);
}

// Return 1 if list item belongs in list, else 0.
static int lists_match(markdown_core_list *list_data, markdown_core_list *item_data) {
    return (
        list_data->list_type == item_data->list_type && list_data->delimiter == item_data->delimiter &&
        // list_data->marker_offset == item_data.marker_offset &&
        list_data->bullet_char == item_data->bullet_char
    );
}

/* Close the open spine. What used to follow -- `process_inlines` -- is the
 * projection, and it no longer runs here: the CST is final after this, and the
 * AST is derived from it (§12.1, §12.5). */
static markdown_core_node *finalize_document(markdown_core_parser *parser) {
    while (parser->current != parser->root) {
        parser->current = finalize(parser, parser->current);
    }

    finalize(parser, parser->root);

    return parser->root;
}

/* An OWNED copy of a chunk's bytes into a zeroed destination; a chunk with no
 * data stays empty rather than owning a zero byte. Returns 0 on loss. */
static int S_chunk_copy(markdown_core_mem *mem, markdown_core_chunk *dst, const markdown_core_chunk *src) {
    unsigned char *copy;
    if (!src->data) {
        return 1;
    }
    copy = (unsigned char *)mem->calloc((size_t)src->len + 1, 1);
    if (!copy) {
        return 0;
    }
    if (src->len > 0) {
        memcpy(copy, src->data, (size_t)src->len);
    }
    dst->data = copy;
    dst->len = src->len;
    dst->alloc = 1;
    dst->owner = NULL;
    return 1;
}

static int S_optional_chunk_copy(
    markdown_core_mem *mem,
    markdown_core_optional_chunk *dst,
    const markdown_core_optional_chunk *src
) {
    if (!src->has_value) {
        *dst = markdown_core_optional_chunk_absent();
        return 1;
    }
    if (!S_chunk_copy(mem, &dst->value, &src->value)) {
        return 0;
    }
    dst->has_value = true;
    return 1;
}

/* One cloned block. Everything the CST states about a block is copied: its
 * content bytes, its place, its flags, its `as` arm, its extension payload,
 * and its run in the content-to-source map (the map itself stays on the
 * parser, which outlives the derivation). */
/* THE PROJECTION CACHE (docs/STREAMING.md T9). A CST block with inline
 * content keeps the list its last projection produced, on a holder hung
 * from the block (`link.holder`, CACHE_OWNER) and keyed by the block's write
 * stamp (T3), both map generations (T4) and the extension set's generation
 * -- an attach re-projects every block. A projection that finds the key
 * unchanged aliases the list into the derived block -- shares it, never
 * copies it (F12: copying costs more than the parse it replaces) -- and the
 * per-block tail (T18) leaves an aliased list alone. A hit also skips the
 * block-content copy, and the store frees a miss's arena once its chunks
 * are owned: content exists to back an owned inline list, and a borrowed
 * list is backed by its holder (#152 Stage 1; `markdown_core_node_check`
 * asserts exactly that). A projection that finds
 * it stale or absent parses, runs the block's tail, and then MOVES the
 * children into a fresh holder and borrows them straight back: with the tail
 * per block nothing touches them after the store, which is what the PoC's
 * copy-per-miss existed to survive (F12). Only a projection against the
 * parser's own map takes part: a generation names a map, not a set of
 * definitions, and `refmap_independence` projects against another. */
static bool S_cache_fresh(markdown_core_parser *parser, const markdown_core_node *block, markdown_core_map *refmap) {
    const markdown_core_holder *holder = block->link.holder;
    /* A map's generation takes part only when the stored projection had
     * something to ask that map (#163): a definition arriving anywhere used
     * to re-key every block in the document, and F19 measured 86.2% of those
     * key changes spurious. A block whose parse held no reference-form label
     * and no footnote call cannot be changed by an insert, so its hit
     * survives the bump; the failure direction is unchanged -- a bit set
     * without a lookup is a slow feed, never a wrong tree. */
    return refmap == parser->refmap && !parser->no_projection_cache &&
           (block->flags & MARKDOWN_CORE_NODE__CACHE_OWNER) && holder->stamp == block->stamp &&
           (!(holder->consulted & MARKDOWN_CORE_NODE__CONSULTED_REFMAP) ||
               holder->refgen == parser->refmap->generation) &&
           (!(holder->consulted & MARKDOWN_CORE_NODE__CONSULTED_FOOTNOTES) ||
               holder->footgen == parser->footnote_defs->generation) &&
           holder->extgen == parser->extension_generation;
}

static void S_cache_store(markdown_core_parser *parser, markdown_core_node *node) {
    markdown_core_node *origin = node->link.origin;
    markdown_core_holder *holder;

    node->link.origin = NULL;
    node->flags &= ~MARKDOWN_CORE_NODE__ORIGIN;
    /* THE STORE MOVES AN INTRUSIVE LIST and nothing else (review-found):
     * `take_children` reads `first_child/last_child`, which on a
     * vector-shaped container are the vector pointer and the count. The
     * enrolled predicate keeps that shape out of here; if any future path
     * lets one through, the block goes unstored -- a slow feed, never a
     * corrupted holder. */
    if (MARKDOWN_CORE_NODE_ARRAY_P(node)) {
        return;
    }
    holder = markdown_core_holder_new(parser->mem);
    if (!holder) {
        /* Nothing is lost: the tree keeps its own children. */
        return;
    }
    markdown_core_holder_take_children(holder, node);
    holder->stamp = origin->stamp;
    holder->refgen = parser->refmap->generation;
    holder->footgen = parser->footnote_defs->generation;
    holder->extgen = parser->extension_generation;
    holder->consulted = node->flags & (MARKDOWN_CORE_NODE__CONSULTED_REFMAP | MARKDOWN_CORE_NODE__CONSULTED_FOOTNOTES);
    /* The creation hold is the cache's hold (holders are born held). */
    if (origin->flags & MARKDOWN_CORE_NODE__CACHE_OWNER) {
        markdown_core_holder_release(origin->link.holder);
    }
    origin->link.holder = holder;
    origin->flags |= MARKDOWN_CORE_NODE__CACHE_OWNER;
    markdown_core_node_borrow_children(node, holder);
    /* THE NODE IS RETAINED WITH ITS LIST (#161, D9): from here it belongs
     * to every tree at once, so the facts that named THIS tree -- parent,
     * siblings -- come off. The borrow above already counts this tree's
     * hold; later trees take their own at the clone. The shell is malloc's
     * (the clone gave every enrolled miss one), so it outlives any arena. */
    holder->node = node;
    /* THE WHOLE PROJECTION FREEZES, not just its root (review-found): a
     * consumer can walk from a fresh container into the retained block,
     * and an interior node without the flag would answer free, unlink,
     * and adoption as if it were one tree's alone. Every node under the
     * stored block carries SHARED, so the node-local gates hold at any
     * depth. Once per store, O(subtree), allocation-free: the list's top
     * level chains through `next` (take_children left those parents
     * NULL), and below it the intact `parent` pointers climb back. */
    node->flags |= MARKDOWN_CORE_NODE__SHARED;
    {
        markdown_core_node *top;
        for (top = node->first_child; top; top = top->next) {
            markdown_core_node *cur = top;
            for (;;) {
                cur->flags |= MARKDOWN_CORE_NODE__SHARED;
                if (cur->first_child) {
                    cur = cur->first_child;
                    continue;
                }
                while (cur != top && !cur->next) {
                    cur = cur->parent;
                }
                if (cur == top) {
                    break;
                }
                cur = cur->next;
            }
        }
    }
    node->parent = NULL;
    node->next = NULL;
    node->prev = NULL;
    /* Under #153 the stored list's literals hold retained slices of the
     * frozen content, so the block that just became a borrower keeps only
     * its own reference (released at node free) -- there is no private
     * arena left to drop, which is what #152 Stage 1's free here used to
     * do for the copied one. */
}

/* A chunk coming to rest in a clone: an owner-backed source shares by
 * retaining, and an alloc'd source is PROMOTED first -- its allocation is
 * wrapped in a buffer once, on the first derivation that copies it, so the
 * one-shot path that never clones never pays the header (#153, lazy
 * freeze). A promotion that cannot allocate falls back to the byte copy:
 * the source keeps its private allocation either way. */
static int S_chunk_share(markdown_core_mem *mem, markdown_core_chunk *dst, markdown_core_chunk *src) {
    if (!src->owner && src->alloc && src->data) {
        markdown_core_buf *adopted = markdown_core_buf_adopt(mem, src->data, src->len);
        if (adopted) {
            src->alloc = 0;
            src->owner = adopted;
        }
    }
    if (src->owner) {
        *dst = markdown_core_chunk_retain_copy(src);
        return 1;
    }
    return S_chunk_copy(mem, dst, src);
}

static int S_optional_chunk_share(
    markdown_core_mem *mem,
    markdown_core_optional_chunk *dst,
    markdown_core_optional_chunk *src
) {
    if (!src->has_value) {
        *dst = markdown_core_optional_chunk_absent();
        return 1;
    }
    if (S_chunk_share(mem, &dst->value, &src->value)) {
        dst->has_value = true;
        return 1;
    }
    return 0;
}

static markdown_core_node *S_clone_block_node(
    markdown_core_parser *parser,
    const markdown_core_node *src,
    markdown_core_map *refmap
) {
    markdown_core_mem *mem = parser->mem;
    markdown_core_node *dst;
    bool enrolled;
    bool hit;
    /* THE HIT IS DECIDED FIRST (#152 Stage 1), now before any allocation:
     * a hit is THE RETAINED NODE ITSELF (#161, D9) -- the projection the
     * store kept, promotion, strip, numbering and consulted bits baked in
     * -- handed back under one holder hold for the requesting tree. No
     * clone, no content retain, no tail: what F15 rule 2 re-ran per
     * projection to reproduce, retention reproduces by identity. */
    /* A block with SKELETON CHILDREN never enrolls (review-found): the
     * store's `take_children` moves an intrusive list, so a block that
     * holds node children when the clone sees it cannot be stored. Since
     * the adoption law (node.c, `can_contain_type`) refuses BLOCK
     * children under any `contains_inlines` parent, an enrollable block
     * cannot carry skeleton children at all any more; this term is the
     * backstop that keeps a slipped shape merely uncached rather than
     * corrupting at the store. */
    enrolled = MARKDOWN_CORE_NODE_BLOCK_P((markdown_core_node *)src) && contains_inlines((markdown_core_node *)src) &&
               src->first_child == NULL && refmap == parser->refmap && !parser->no_projection_cache;
    hit = enrolled && S_cache_fresh(parser, src, refmap) && src->link.holder->node != NULL;
    if (hit) {
        markdown_core_holder *holder = src->link.holder;
        markdown_core_holder_hold(holder);
        parser->cache_hits++;
        return holder->node;
    }
    /* Skeleton nodes live and die with the derived tree, so they come out
     * of the derivation's arena (#161) -- zeroed, like the calloc this was;
     * the flag is set at birth, before anything can fail, so every disposal
     * path below can ask it. The one exception is an enrolled MISS: its
     * tail will store it, the holder will hand it to later trees, and a
     * node that outlives this tree cannot live in this tree's arena, so it
     * takes a malloc shell. */
    bool arena_shell = parser->derive_arena && !enrolled;
    if (arena_shell) {
        dst = markdown_core_node_arena_calloc(parser->derive_arena);
        if (!dst) {
            return NULL;
        }
        dst->flags = MARKDOWN_CORE_NODE__ARENA;
    } else {
        dst = (markdown_core_node *)mem->calloc(1, sizeof(*dst));
        if (!dst) {
            return NULL;
        }
    }
    markdown_core_strbuf_init(mem, &dst->content, 0);
    if (src->content.size) {
        /* THE LAZY FREEZE (#153): a closed block's content freezes on the
         * first derivation that shares it -- allocation and bytes
         * untouched, `content.ptr/size` repointed at the same bytes -- so
         * the one-shot path never pays for sharing it never does. A failed
         * freeze loses the bytes with the strbuf; no fallback. */
        if (!src->frozen_content && !(src->flags & MARKDOWN_CORE_NODE__OPEN)) {
            markdown_core_node *cst = (markdown_core_node *)src;
            cst->frozen_content = markdown_core_buf_freeze(&cst->content);
            if (!cst->frozen_content) {
                parser->oom = true;
                if (dst->flags & MARKDOWN_CORE_NODE__ARENA) {
                    markdown_core_node_arena_forget(dst);
                } else {
                    mem->free(dst);
                }
                return NULL;
            }
            cst->content.ptr = cst->frozen_content->bytes;
            cst->content.size = cst->frozen_content->size;
            cst->content.asize = 0;
        }
        if (src->frozen_content) {
            /* Closed content is frozen: the derivation shares the bytes,
             * never copies them. */
            markdown_core_buf_retain(src->frozen_content);
            dst->frozen_content = src->frozen_content;
            dst->content.ptr = src->content.ptr;
            dst->content.size = src->content.size;
        } else {
            /* An open block's tail is still a live accumulator; copy the
             * tail-sized bytes and freeze the private copy, so the parse's
             * literals hold slices here too and every derived block is one
             * shape. */
            markdown_core_strbuf_put(&dst->content, src->content.ptr, src->content.size);
            if (dst->content.oom) {
                markdown_core_strbuf_free(&dst->content);
                if (dst->flags & MARKDOWN_CORE_NODE__ARENA) {
                    markdown_core_node_arena_forget(dst);
                } else {
                    mem->free(dst);
                }
                return NULL;
            }
            dst->frozen_content = markdown_core_buf_freeze(&dst->content);
            if (!dst->frozen_content) {
                if (dst->flags & MARKDOWN_CORE_NODE__ARENA) {
                    markdown_core_node_arena_forget(dst);
                } else {
                    mem->free(dst);
                }
                return NULL;
            }
            dst->content.ptr = dst->frozen_content->bytes;
            dst->content.size = dst->frozen_content->size;
        }
    }
    dst->start_line = src->start_line;
    dst->start_column = src->start_column;
    dst->end_line = src->end_line;
    dst->end_column = src->end_column;
    dst->internal_offset = src->internal_offset;
    dst->content_mark = src->content_mark;
    dst->content_mark_count = src->content_mark_count;
    /* THE CARRY (T2): the derived block IS the CST block to a consumer, and
     * this line is what makes two projections of one CST name every block
     * identically (F11). A clone is calloc'd, so losing this fails closed.
     * The owner rides along for the one inline-class subtree the skeleton
     * carries -- a directive's CST-resident label. */
    dst->identifier = src->identifier;
    dst->owner = src->owner;
    dst->type = src->type;
    /* The carry overwrites the birth flags, so the ARENA bit -- this
     * clone's own fact, never the CST's -- is re-asserted after it (#161). */
    dst->flags = src->flags & ~(MARKDOWN_CORE_NODE__CACHE_OWNER | MARKDOWN_CORE_NODE__ORIGIN |
                                  MARKDOWN_CORE_NODE__CONSULTED_REFMAP | MARKDOWN_CORE_NODE__CONSULTED_FOOTNOTES);
    if (arena_shell) {
        dst->flags |= MARKDOWN_CORE_NODE__ARENA;
    }
    dst->extension = src->extension;

    switch (S_type(dst)) {
    case MARKDOWN_CORE_NODE_HEADING:
        dst->as.heading = src->as.heading;
        break;
    case MARKDOWN_CORE_NODE_LIST:
    case MARKDOWN_CORE_NODE_LIST_ITEM:
        dst->as.list = src->as.list;
        break;
    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        dst->as.code = src->as.code;
        dst->as.code.info = markdown_core_optional_chunk_absent();
        dst->as.code.literal.data = NULL;
        dst->as.code.literal.len = 0;
        dst->as.code.literal.alloc = 0;
        dst->as.code.literal.owner = NULL;
        if (!S_chunk_share(mem, &dst->as.code.literal, &((markdown_core_node *)src)->as.code.literal) ||
            !S_optional_chunk_share(mem, &dst->as.code.info, &((markdown_core_node *)src)->as.code.info)) {
            goto lost;
        }
        break;
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
        /* The union arm depends on the block's life stage: open, it holds the
         * matched block TYPE; closed, `finalize` detached the content into the
         * literal. The flag this engine now maintains says which (§12.8 Q3). */
        if (dst->flags & MARKDOWN_CORE_NODE__OPEN) {
            dst->as.html_block_type = src->as.html_block_type;
        } else if (!S_chunk_share(mem, &dst->as.literal, &((markdown_core_node *)src)->as.literal)) {
            goto lost;
        }
        break;
    case MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION:
        if (!S_chunk_copy(mem, &dst->as.association.label, &src->as.association.label) ||
            !S_chunk_copy(mem, &dst->as.association.identifier, &src->as.association.identifier)) {
            goto lost;
        }
        break;
    case MARKDOWN_CORE_NODE_REFERENCE_DEFINITION:
        if (src->as.definition) {
            markdown_core_definition *def = (markdown_core_definition *)mem->calloc(1, sizeof(*def));
            if (!def) {
                goto lost;
            }
            dst->as.definition = def;
            def->title = markdown_core_optional_chunk_absent();
            if (!S_chunk_copy(mem, &def->association.label, &src->as.definition->association.label) ||
                !S_chunk_copy(mem, &def->association.identifier, &src->as.definition->association.identifier) ||
                !S_chunk_copy(mem, &def->url, &src->as.definition->url) ||
                !S_optional_chunk_copy(mem, &def->title, &src->as.definition->title)) {
                goto lost;
            }
        }
        break;
    default:
        /* An extension-minted type keeps its `as` state behind the extension's
         * own copy hook below; a core type with no arm copies nothing. */
        break;
    }

    if (dst->extension) {
        if (dst->extension->opaque_copy_func) {
            if (!dst->extension->opaque_copy_func(dst->extension, mem, dst, src)) {
                goto lost;
            }
        } else if (src->as.opaque && dst->extension->opaque_free_func) {
            /* The node owns a payload the core cannot name and the extension
             * did not say how to copy. Losing it silently would ship a tree
             * that dumps differently from the one `finish` used to build. */
            goto lost;
        }
    }

    /* A HIT RETURNED AT THE TOP (#161, D9); what reaches here enrolled is a
     * MISS, and it remembers where it came from so its tail can store. A
     * projection against another map never stores: what it resolves is that
     * map's answer, and the key names this parser's. Only BLOCK-class nodes
     * with inline content take part: they are the ones the re-parse costs
     * -- a CST-resident inline construct (a directive's label) is never
     * queued, so enrolling it left ORIGIN and a CST pointer on a node the
     * caller holds (found on the landing review). Its passes run from the
     * block that owns it instead. */
    if (enrolled) {
        dst->link.origin = (markdown_core_node *)src;
        dst->flags |= MARKDOWN_CORE_NODE__ORIGIN;
        parser->cache_misses++;
    }
    /* Every node the clone BUILDS enters the fresh list (#161): the
     * projection serves exactly this set, so the retained nodes -- which
     * returned at the top -- cost the walk nothing. A refused push poisons
     * the parse the way a truncated walk did: a projection that missed a
     * block must not look whole. */
    if (parser->fresh_queue_armed && !S_fresh_queue_push(parser, dst)) {
        parser->oom = true;
    }
    return dst;

lost:
    markdown_core_node_free(dst);
    return NULL;
}

/* Clone the block skeleton. Iterative, because block nesting is input-shaped.
 * Children are linked raw: the source tree already proved containment. */
/* Turn a freshly cloned `parent` into a CHILD_ARRAY container sized for its
 * origin's children (#161, D9): the vector is the parent's own statement of
 * sibling order, so a closed child's NODE can later be shared between trees
 * without carrying any per-tree fact. Zero children still flag the shape --
 * an empty document is a container, not a leaf. */
static bool S_vec_open(markdown_core_parser *parser, markdown_core_node *parent, const markdown_core_node *src) {
    const markdown_core_node *child;
    size_t total = 0;
    for (child = src->first_child; child; child = child->next) {
        total++;
    }
    parent->children.vec = NULL;
    parent->children.count = 0;
    if (total) {
        /* The vector's allocator FOLLOWS THE SHELL'S (review-found), never
         * the derivation's mood: a derivation whose arena was refused
         * shells every node from malloc, and an arena vector under a
         * malloc shell would outlive nothing it can name. The free walk
         * relies on this one rule. */
        parent->children.vec =
            (parent->flags & MARKDOWN_CORE_NODE__ARENA)
                ? (
                      markdown_core_node **
                  )markdown_core_node_arena_bytes(parser->derive_arena, total * sizeof(*parent->children.vec))
                : (markdown_core_node **)parser->mem->realloc(NULL, total * sizeof(*parent->children.vec));
        if (!parent->children.vec) {
            return false;
        }
    }
    parent->flags |= MARKDOWN_CORE_NODE__CHILD_ARRAY;
    return true;
}

/* Append into the vector. A FRESH child also learns its parent -- the
 * ancestor guard, the numbering climb and the hooks read it -- and keeps
 * enough of a sibling chain for the free walk's splice. A SHARED child
 * learns NOTHING (#161, D9): every one of these facts is per-tree, the
 * vector carries them all, and the node belongs to every tree at once.
 *
 * Every parent the clone hands in is vec-opened: an enrolled block never
 * carries skeleton children -- the enrolled PREDICATE refuses the shape,
 * not just the built-in grammar -- and an enrolled block's inline
 * children materialize at projection through `node_append_child`, whose
 * intrusive list is what the store's `take_children` moves. The
 * intrusive arm that used to sit here for "enrolled parents" served a
 * shape nothing can now build (review-found: a nested hit would have
 * been linked into a retained list and freed under its own holder), and
 * container retention (#161 phase 2) stores vectors of shared children,
 * not intrusive lists, so it was no future's groundwork either. */
static void S_vec_append(markdown_core_node *parent, markdown_core_node *child) {
    assert(MARKDOWN_CORE_NODE_ARRAY_P(parent));
    parent->children.vec[parent->children.count++] = child;
    if (!(child->flags & MARKDOWN_CORE_NODE__SHARED)) {
        child->parent = parent;
        child->prev = NULL;
        child->next = NULL;
    }
}

static markdown_core_node *S_clone_block_tree(
    markdown_core_parser *parser,
    const markdown_core_node *src_root,
    markdown_core_map *refmap
) {
    markdown_core_node *dst_root = S_clone_block_node(parser, src_root, refmap);
    markdown_core_node *dst_parent = dst_root;
    const markdown_core_node *src = src_root->first_child;

    if (!dst_root) {
        return NULL;
    }
    if (!S_vec_open(parser, dst_root, src_root)) {
        markdown_core_node_free(dst_root);
        return NULL;
    }
    while (src) {
        markdown_core_node *dst = S_clone_block_node(parser, src, refmap);
        if (!dst) {
            markdown_core_node_free(dst_root);
            return NULL;
        }
        S_vec_append(dst_parent, dst);

        if (src->first_child && !(dst->flags & MARKDOWN_CORE_NODE__SHARED)) {
            /* A HIT never reaches this branch (review-found): the retained
             * node IS the projection of the source's whole subtree, and
             * descending would clone raw CST children over the shared
             * projection every other live tree is reading. An enrolled
             * MISS never reaches it either, by the enrolled predicate
             * itself now -- a block with skeleton children never enrolls
             * -- so ORIGIN here is provably impossible, and the assert
             * holds the door on the predicate ever loosening. */
            assert(!(dst->flags & MARKDOWN_CORE_NODE__ORIGIN));
            if (!S_vec_open(parser, dst, src)) {
                markdown_core_node_free(dst_root);
                return NULL;
            }
            src = src->first_child;
            dst_parent = dst;
        } else {
            while (!src->next && src != src_root) {
                src = src->parent;
                dst_parent = dst_parent->parent;
            }
            if (src == src_root) {
                break;
            }
            src = src->next;
        }
    }
    return dst_root;
}

/* THE PROJECTION's work, on whichever block skeleton it is handed: parse
 * each content-bearing block's inlines against the map as it now stands, then
 * run the tail `finish` always ran -- consolidation, the extension
 * postprocessors, the comment strip -- block by block (T18). Returns the
 * projected root, which a hook may have replaced. The skeleton is CONSUMED:
 * after this it is an AST, and projecting it again would not give the same
 * answer. That is why a re-projection hands in a clone and `finish` hands in
 * the CST itself. */
/* The derive-path projection serves EXACTLY the set the clone built (#161):
 * the fresh list, in clone order. The walk that stepped past every retained
 * block to find these -- ENTER, flag test, skip, EXIT per shared child, a
 * quarter of a width-heavy stream's instructions -- is the finish path's
 * alone now (T1 hands in the CST itself, where borrowers need the walk's
 * treatment). Parses run forward; the tail queue fills BACKWARD, so a child
 * still precedes its parent in the drain exactly as the walk's post-order
 * had it, and the drain's "no entry behind a replacement dangles" rule
 * keeps holding. Sibling order flips, which F15 states is free: no pass
 * moves a node to another parent. */
static void S_project_fresh(markdown_core_parser *parser, markdown_core_map *refmap) {
    size_t i;
    markdown_core_manage_extensions_special_characters(parser, true);
    for (i = 0; i < parser->fresh_queue_size; i++) {
        markdown_core_node *block = parser->fresh_queue[i];
        /* The SHAPE test beside the classification (review-found): a
         * stateful contains_inlines_func can answer true at projection
         * for a block adopted under false, and the inline parser appends
         * through the intrusive overlay -- on a vector-shaped container
         * that write lands in the vector pointer and the count. A flipped
         * hybrid projects with its children and its content unparsed:
         * degraded, never corrupted. Every legitimate inline block is
         * intrusive (it had no skeleton children to vectorize), so the
         * test costs one bit on a word already loaded. */
        if (contains_inlines(block) && !MARKDOWN_CORE_NODE_ARRAY_P(block)) {
            markdown_core_parse_inlines(parser, block, refmap, parser->options);
        }
    }
    for (i = parser->fresh_queue_size; i > 0; i--) {
        markdown_core_node *block = parser->fresh_queue[i - 1];
        if (MARKDOWN_CORE_NODE_BLOCK_P(block) && S_block_has_tail_work(parser, block)) {
            if (!S_tail_queue_push(parser, block)) {
                parser->oom = true;
            }
        }
    }
    parser->fresh_queue_size = 0;
    markdown_core_manage_extensions_special_characters(parser, false);
}

static markdown_core_node *S_project(
    markdown_core_parser *parser,
    markdown_core_node *skeleton,
    markdown_core_map *refmap
) {
    /* A PROJECTION READS THE CST AND LEAVES NOTHING BEHIND THAT THE PARSE
     * WILL READ BACK (docs/STREAMING.md F21, correcting F10). `parse_inlines`
     * mints a mark for a block that has none -- an empty ATX heading, a
     * directive's label -- into the parser's own vector; an open block that
     * takes its next line after that finds its run no longer contiguous with
     * the vector's end, its later marks land outside the run, and
     * `content_place` answers from a foreign mark. Every position the
     * projection needs is written into the nodes it builds while it runs, so
     * the minted marks are scratch, and the vector is cut back to where the
     * parse left it. */
    bufsize_t marks_before = parser->line_marks_size;

    if (parser->fresh_queue_armed) {
        S_project_fresh(parser, refmap);
    } else {
        process_inlines(parser, skeleton, refmap, parser->options);
    }

    S_run_block_tails(parser, &skeleton);

    parser->line_marks_size = marks_before;
    return skeleton;
}

/* THE PROJECTION: AST = project(CST, refmap) (§12.1), as a NEW tree. Clone
 * the block skeleton and project onto the clone. The CST is not written: a
 * later derivation, against this map or another, starts from the same bytes.
 * This is the RE-projection path -- `finish`, whose CST has no later, skips
 * the clone and projects in place (T1). */
markdown_core_node *markdown_core_parser_derive_tree(markdown_core_parser *parser, markdown_core_map *refmap) {
    markdown_core_node *derived;
    markdown_core_node_arena *arena;

    /* A parse that lost an allocation may hold a tree that is not all there --
     * the sweep's witness is a footnote definition whose label still borrows a
     * freed temporary -- and `finish` answers NULL for it regardless, so there
     * is nothing to derive. */
    if (parser->oom) {
        return NULL;
    }

    /* THE SKELETON'S ARENA (#161): every node the clone builds lives
     * exactly as long as the derived tree, so they share one allocation,
     * sized by the mint -- an upper bound on the CST's block count, so the
     * one page covers the derivation. No handle rides out: the tree is
     * self-contained (node.h), each node knowing its arena through
     * `content.mem`, and this call's own hold is released before the return
     * -- after the projection, whose hooks may free skeleton nodes -- so
     * from here the nodes alone keep the pages alive, however the caller
     * frees, borrows against, or replaces what it was handed. */
    /* ONE allocation model at every size (review-found): the byte threshold
     * that once spared small documents the page-and-slop churn earned its
     * keep only while every feed rebuilt every node -- with hits sharing
     * the retained node, the arena serves the spine, the vectors and the
     * misses, and the 12 KB stream measures the same to the digit with the
     * threshold gone. */
    arena = markdown_core_node_arena_new(parser->mem, (size_t)parser->block_ids_minted + 2);
    if (!arena) {
        parser->oom = true;
        return NULL;
    }
    parser->derive_arena = arena;
    parser->fresh_queue_size = 0;
    parser->fresh_queue_armed = true;
    derived = S_clone_block_tree(parser, parser->root, refmap);
    parser->derive_arena = NULL;
    if (!derived) {
        parser->fresh_queue_armed = false;
        if (arena) {
            markdown_core_node_arena_release(arena);
        }
        parser->oom = true;
        return NULL;
    }

    derived = S_project(parser, derived, refmap);
    parser->fresh_queue_armed = false;
    if (arena) {
        markdown_core_node_arena_release(arena);
    }
    if (derived && parser->oom) {
        /* Poisoned mid-projection (review-found): a lost mask pool or a
         * refused spill left hooks unrun or blocks unvisited, and a tree
         * that LOOKS whole must not outlive the flag that says it is not. */
        markdown_core_node_free(derived);
        return NULL;
    }
    return derived;
}

markdown_core_node *markdown_core_parse_document(const char *buffer, size_t len, int options) {
    markdown_core_parser *parser = markdown_core_parser_new(options);
    markdown_core_node *document;

    S_parser_feed(parser, (const unsigned char *)buffer, len, true);

    document = markdown_core_parser_finish(parser);
    markdown_core_parser_free(parser);
    return document;
}

void markdown_core_parser_feed(markdown_core_parser *parser, const char *buffer, size_t len) {
    S_parser_feed(parser, (const unsigned char *)buffer, len, false);
}

/* One reservation for the whole of this chunk's contribution to the held
 * partial line, and then a test.
 *
 * `parser->linebuf.oom` was written at six sites and read at NONE (D27). A
 * refused growth made `markdown_core_strbuf_put` a no-op, and the accumulated
 * PREFIX was then handed to `S_process_line` as though it were a whole line and
 * committed -- with `parser->oom` clear, so `finish` returned a document.
 * Measured on a 279-byte document fed in 32-byte chunks: refusing allocation 6
 * of 25 leaves 55 of 275 text bytes and reports success.
 *
 * Reserving first is what makes the refusal atomic: the NUL path writes twice,
 * and a failure between the two writes leaves a line that is neither the old
 * one nor the new one. The arithmetic is done in 64 bits because `bufsize_t` is
 * int32_t and `size + add` is exactly the overflow A4 closed one level down. */
static bool S_linebuf_reserve(markdown_core_parser *parser, int64_t add) {
    int64_t target = (int64_t)parser->linebuf.size + add;

    if (add < 0 || target > (int64_t)(INT32_MAX / 2)) {
        parser->linebuf.oom = 1;
    } else if (add > 0) {
        markdown_core_strbuf_grow(&parser->linebuf, (bufsize_t)target);
    }
    if (parser->linebuf.oom) {
        parser->oom = true;
        return false;
    }
    return true;
}

static void S_parser_feed(markdown_core_parser *parser, const unsigned char *buffer, size_t len, bool eof) {
    const unsigned char *end = buffer + len;
    static const uint8_t repl[] = {239, 191, 189};

    if (parser->last_buffer_ended_with_cr && *buffer == '\n') {
        // skip NL if last buffer ended with CR ; see #117
        buffer++;
    }
    parser->last_buffer_ended_with_cr = false;
    while (buffer < end) {
        const unsigned char *eol;
        bufsize_t chunk_len;
        bool process = false;
        for (eol = buffer; eol < end; ++eol) {
            if (S_is_line_end_char(*eol)) {
                process = true;
                break;
            }
            if (*eol == '\0' && eol < end) {
                break;
            }
        }
        if (eol >= end && eof) {
            process = true;
        }

        chunk_len = (bufsize_t)(eol - buffer);
        if (process) {
            if (parser->linebuf.size > 0) {
                if (!S_linebuf_reserve(parser, chunk_len)) {
                    return;
                }
                markdown_core_strbuf_put(&parser->linebuf, buffer, chunk_len);
                S_process_line(parser, parser->linebuf.ptr, parser->linebuf.size);
                markdown_core_strbuf_clear(&parser->linebuf);
            } else {
                S_process_line(parser, buffer, chunk_len);
            }
        } else {
            if (eol < end && *eol == '\0') {
                // omit NULL byte, add replacement character
                if (!S_linebuf_reserve(parser, (int64_t)chunk_len + 3)) {
                    return;
                }
                markdown_core_strbuf_put(&parser->linebuf, buffer, chunk_len);
                markdown_core_strbuf_put(&parser->linebuf, repl, 3);
            } else {
                if (!S_linebuf_reserve(parser, chunk_len)) {
                    return;
                }
                markdown_core_strbuf_put(&parser->linebuf, buffer, chunk_len);
            }
        }

        buffer += chunk_len;
        if (buffer < end) {
            if (*buffer == '\0') {
                // skip over NULL
                buffer++;
            } else {
                // skip over line ending characters
                if (*buffer == '\r') {
                    buffer++;
                    if (buffer == end) {
                        parser->last_buffer_ended_with_cr = true;
                    }
                }
                if (buffer < end && *buffer == '\n') {
                    buffer++;
                }
            }
        }
    }
}

static void chop_trailing_hashtags(markdown_core_chunk *ch) {
    bufsize_t n, orig_n;

    markdown_core_chunk_rtrim(ch);
    orig_n = n = ch->len - 1;

    // if string ends in space followed by #s, remove these:
    while (n >= 0 && peek_at(ch, n) == '#') {
        n--;
    }

    // Check for a space before the final #s:
    if (n != orig_n && n >= 0 && S_is_space_or_tab(peek_at(ch, n))) {
        ch->len = n;
        markdown_core_chunk_rtrim(ch);
    }
}

// Check for thematic break.  On failure, return 0 and update
// thematic_break_kill_pos with the index at which the
// parse fails.  On success, return length of match.
// "...three or more hyphens, asterisks,
// or underscores on a line by themselves. If you wish, you may use
// spaces between the hyphens or asterisks."
static int S_scan_thematic_break(markdown_core_parser *parser, markdown_core_chunk *input, bufsize_t offset) {
    bufsize_t i;
    char c;
    char nextc = '\0';
    int count;
    i = offset;
    c = peek_at(input, i);
    if (!(c == '*' || c == '_' || c == '-')) {
        parser->thematic_break_kill_pos = i;
        return 0;
    }
    count = 1;
    while ((nextc = peek_at(input, ++i))) {
        if (nextc == c) {
            count++;
        } else if (nextc != ' ' && nextc != '\t') {
            break;
        }
    }
    if (count >= 3 && (nextc == '\r' || nextc == '\n')) {
        return (i - offset) + 1;
    } else {
        parser->thematic_break_kill_pos = i;
        return 0;
    }
}

// Find first nonspace character from current offset, setting
// parser->first_nonspace, parser->first_nonspace_column,
// parser->indent, and parser->blank. Does not advance parser->offset.
static void S_find_first_nonspace(markdown_core_parser *parser, markdown_core_chunk *input) {
    char c;
    int chars_to_tab = TAB_STOP - (parser->column % TAB_STOP);

    if (parser->first_nonspace <= parser->offset) {
        parser->first_nonspace = parser->offset;
        parser->first_nonspace_column = parser->column;
        while ((c = peek_at(input, parser->first_nonspace))) {
            if (c == ' ') {
                parser->first_nonspace += 1;
                parser->first_nonspace_column += 1;
                chars_to_tab = chars_to_tab - 1;
                if (chars_to_tab == 0) {
                    chars_to_tab = TAB_STOP;
                }
            } else if (c == '\t') {
                parser->first_nonspace += 1;
                parser->first_nonspace_column += chars_to_tab;
                chars_to_tab = TAB_STOP;
            } else {
                break;
            }
        }
    }

    parser->indent = parser->first_nonspace_column - parser->column;
    parser->blank = S_is_line_end_char(peek_at(input, parser->first_nonspace));
}

// Advance parser->offset and parser->column.  parser->offset is the
// byte position in input; parser->column is a virtual column number
// that takes into account tabs. (Multibyte characters are not taken
// into account, because the Markdown line prefixes we are interested in
// analyzing are entirely ASCII.)  The count parameter indicates
// how far to advance the offset.  If columns is true, then count
// indicates a number of columns; otherwise, a number of bytes.
// If advancing a certain number of columns partially consumes
// a tab character, parser->partially_consumed_tab is set to true.
static void S_advance_offset(markdown_core_parser *parser, markdown_core_chunk *input, bufsize_t count, bool columns) {
    char c;
    int chars_to_tab;
    int chars_to_advance;
    while (count > 0 && (c = peek_at(input, parser->offset))) {
        if (c == '\t') {
            chars_to_tab = TAB_STOP - (parser->column % TAB_STOP);
            if (columns) {
                parser->partially_consumed_tab = chars_to_tab > count;
                chars_to_advance = MIN(count, chars_to_tab);
                parser->column += chars_to_advance;
                parser->offset += (parser->partially_consumed_tab ? 0 : 1);
                count -= chars_to_advance;
            } else {
                parser->partially_consumed_tab = false;
                parser->column += chars_to_tab;
                parser->offset += 1;
                count -= 1;
            }
        } else {
            parser->partially_consumed_tab = false;
            parser->offset += 1;
            parser->column += 1; // assume ascii; block starts are ascii
            count -= 1;
        }
    }
}

static bool S_last_child_is_open(markdown_core_node *container) {
    return container->last_child && (container->last_child->flags & MARKDOWN_CORE_NODE__OPEN);
}

static bool parse_block_quote_prefix(markdown_core_parser *parser, markdown_core_chunk *input) {
    bool res = false;
    bufsize_t matched = 0;

    matched = parser->indent <= 3 && peek_at(input, parser->first_nonspace) == '>';
    if (matched) {

        S_advance_offset(parser, input, parser->indent + 1, true);

        if (S_is_space_or_tab(peek_at(input, parser->offset))) {
            S_advance_offset(parser, input, 1, true);
        }

        res = true;
    }
    return res;
}

static bool parse_footnote_definition_block_prefix(
    markdown_core_parser *parser,
    markdown_core_chunk *input,
    markdown_core_node *container
) {
    if (parser->indent >= 4) {
        S_advance_offset(parser, input, 4, true);
        return true;
    } else if (input->len > 0 && (input->data[0] == '\n' || (input->data[0] == '\r' && input->data[1] == '\n'))) {
        return true;
    }

    return false;
}

static bool parse_node_item_prefix(
    markdown_core_parser *parser,
    markdown_core_chunk *input,
    markdown_core_node *container
) {
    bool res = false;

    if (parser->indent >= container->as.list.marker_offset + container->as.list.padding) {
        S_advance_offset(parser, input, container->as.list.marker_offset + container->as.list.padding, true);
        res = true;
    } else if (parser->blank && container->first_child != NULL) {
        // if container->first_child is NULL, then the opening line
        // of the list item was blank after the list marker; in this
        // case, we are done with the list item.
        S_advance_offset(parser, input, parser->first_nonspace - parser->offset, false);
        res = true;
    }
    return res;
}

static bool parse_code_block_prefix(
    markdown_core_parser *parser,
    markdown_core_chunk *input,
    markdown_core_node *container,
    bool *should_continue
) {
    bool res = false;

    if (!container->as.code.fenced) { // indented
        if (parser->indent >= CODE_INDENT) {
            S_advance_offset(parser, input, CODE_INDENT, true);
            res = true;
        } else if (parser->blank) {
            S_advance_offset(parser, input, parser->first_nonspace - parser->offset, false);
            res = true;
        }
    } else { // fenced
        bufsize_t matched = 0;

        if (parser->indent <= 3 && (peek_at(input, parser->first_nonspace) == container->as.code.fence_char)) {
            matched = scan_close_code_fence(input, parser->first_nonspace);
        }

        if (matched >= container->as.code.fence_length) {
            // closing fence - and since we're at
            // the end of a line, we can stop processing it:
            *should_continue = false;
            container->as.code.fence_closed = true;
            S_advance_offset(parser, input, matched, false);
            parser->current = finalize(parser, container);
        } else {
            // skip opt. spaces of fence parser->offset
            int i = container->as.code.fence_offset;

            while (i > 0 && S_is_space_or_tab(peek_at(input, parser->offset))) {
                S_advance_offset(parser, input, 1, true);
                i--;
            }
            res = true;
        }
    }

    return res;
}

static bool parse_html_block_prefix(markdown_core_parser *parser, markdown_core_node *container) {
    bool res = false;
    int html_block_type = container->as.html_block_type;

    assert(html_block_type >= 1 && html_block_type <= 7);
    switch (html_block_type) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
        // these types of blocks can accept blanks
        res = true;
        break;
    case 6:
    case 7:
        res = !parser->blank;
        break;
    }

    return res;
}

static bool parse_extension_block(
    markdown_core_parser *parser,
    markdown_core_node *container,
    markdown_core_chunk *input,
    bool *should_continue
) {
    int matched;

    if (!container->extension->last_block_matches) {
        return false;
    }

    matched =
        container->extension->last_block_matches(container->extension, parser, input->data, input->len, container);
    if (matched != MARKDOWN_CORE_BLOCK_CLOSED) {
        return matched != 0;
    }

    /* The container's own closing line. Everything still open inside it ended
     * on the line before, and the container ends here.
     *
     * `parser->current` is the deepest open block and `container` is on the
     * path from the root to it, so walking up through `finalize` reaches it.
     * `finalize` frees a node only in its PARAGRAPH case and returns the
     * parent either way, so the loop is safe across a paragraph that was
     * nothing but reference definitions. */
    *should_continue = false;
    while (parser->current != container) {
        parser->current = finalize(parser, parser->current);
        assert(parser->current != NULL);
    }
    /* `container` carries an extension-minted type, never PARAGRAPH, so it
     * survives its own finalize and can still be positioned. That is the one
     * lifetime invariant this path rests on. */
    assert(S_type(container) != MARKDOWN_CORE_NODE_PARAGRAPH);
    parser->current = finalize(parser, container);
    S_set_end_to_current_line(parser, container);
    return false;
}

/**
 * For each containing node, try to parse the associated line start.
 *
 * Will not close unmatched blocks, as we may have a lazy continuation
 * line -> http://spec.commonmark.org/0.24/#lazy-continuation-line
 *
 * Returns: The last matching node, or NULL
 */
static markdown_core_node *check_open_blocks(
    markdown_core_parser *parser,
    markdown_core_chunk *input,
    bool *all_matched
) {
    bool should_continue = true;
    *all_matched = false;
    markdown_core_node *container = parser->root;
    markdown_core_node_type cont_type;

    while (S_last_child_is_open(container)) {
        container = container->last_child;
        cont_type = S_type(container);

        S_find_first_nonspace(parser, input);

        if (container->extension) {
            if (!parse_extension_block(parser, container, input, &should_continue)) {
                goto done;
            }
            continue;
        }

        switch (cont_type) {
        case MARKDOWN_CORE_NODE_BLOCK_QUOTE:
            if (!parse_block_quote_prefix(parser, input)) {
                goto done;
            }
            break;
        case MARKDOWN_CORE_NODE_LIST_ITEM:
            if (!parse_node_item_prefix(parser, input, container)) {
                goto done;
            }
            break;
        case MARKDOWN_CORE_NODE_CODE_BLOCK:
            if (!parse_code_block_prefix(parser, input, container, &should_continue)) {
                goto done;
            }
            break;
        case MARKDOWN_CORE_NODE_HEADING:
            // a heading can never contain more than one line
            goto done;
        case MARKDOWN_CORE_NODE_HTML_BLOCK:
            if (!parse_html_block_prefix(parser, container)) {
                goto done;
            }
            break;
        case MARKDOWN_CORE_NODE_PARAGRAPH:
            if (parser->blank) {
                goto done;
            }
            break;
        case MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION:
            if (!parse_footnote_definition_block_prefix(parser, input, container)) {
                goto done;
            }
            break;
        default:
            break;
        }

        /* Whatever this container's prefix consumed is that container's
         * MARKER: `> ` belongs to the block quote, the item's indent to the
         * list item. One claim per container, walking down the spine. */
    }

    *all_matched = true;

done:
    /* A container whose prefix consumed bytes and then declined still read
     * them; they are its marker up to the point it gave up. */
    if (!*all_matched) {
        container = container->parent; // back up to last matching node
    }

    if (!should_continue) {
        container = NULL;
    }

    return container;
}

static void open_new_blocks(
    markdown_core_parser *parser,
    markdown_core_node **container,
    markdown_core_chunk *input,
    bool all_matched
) {
    bool indented;
    markdown_core_list *data = NULL;
    bool maybe_lazy = S_type(parser->current) == MARKDOWN_CORE_NODE_PARAGRAPH;
    markdown_core_node_type cont_type = S_type(*container);
    bufsize_t matched = 0;
    int lev = 0;
    bool save_partially_consumed_tab;
    bool has_content;
    int save_offset;
    int save_column;
    size_t depth = 0;

    while (cont_type != MARKDOWN_CORE_NODE_CODE_BLOCK && cont_type != MARKDOWN_CORE_NODE_HTML_BLOCK &&
           !extension_accepts_lines(*container)) {
        depth++;
        S_find_first_nonspace(parser, input);
        /* Indentation ahead of whatever opens here is the CONTAINER's, not the
         * new block's: a block begins at its own first non-space byte, so
         * giving the spaces to the block being opened would make its first
         * region start before its own scope. Measured before it was fixed --
         * 52 rows of an indented code block's four spaces alone. */
        indented = parser->indent >= CODE_INDENT;

        if (!indented && peek_at(input, parser->first_nonspace) == '>') {

            bufsize_t blockquote_startpos = parser->first_nonspace;

            S_advance_offset(parser, input, parser->first_nonspace + 1 - parser->offset, false);
            // optional following character
            if (S_is_space_or_tab(peek_at(input, parser->offset))) {
                S_advance_offset(parser, input, 1, true);
            }
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_BLOCK_QUOTE, blockquote_startpos + 1);
            if (!*container) {
                return;
            }

        } else if (!indented && (matched = scan_atx_heading_start(input, parser->first_nonspace))) {
            bufsize_t hashpos;
            int level = 0;
            bufsize_t heading_startpos = parser->first_nonspace;

            S_advance_offset(parser, input, parser->first_nonspace + matched - parser->offset, false);
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_HEADING, heading_startpos + 1);
            if (!*container) {
                return;
            }

            hashpos = markdown_core_chunk_strchr(input, '#', parser->first_nonspace);

            while (peek_at(input, hashpos) == '#') {
                level++;
                hashpos++;
            }

            (*container)->as.heading.level = level;
            (*container)->as.heading.setext = false;
            (*container)->internal_offset = matched;

        } else if (!indented && (matched = scan_open_code_fence(input, parser->first_nonspace))) {
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_CODE_BLOCK, parser->first_nonspace + 1);
            if (!*container) {
                return;
            }
            (*container)->as.code.fenced = true;
            (*container)->as.code.fence_char = peek_at(input, parser->first_nonspace);
            (*container)->as.code.fence_length = (matched > 255) ? 255 : (uint8_t)matched;
            (*container)->as.code.fence_offset = (int8_t)(parser->first_nonspace - parser->offset);
            (*container)->as.code.fence_closed = false;
            /* Nothing is known about an info string until the fence line is
             * read; ABSENT is the honest state, and the close either replaces
             * it or leaves it. It used to open as an empty STRING, which said
             * the source had written one. */
            (*container)->as.code.info = markdown_core_optional_chunk_absent();
            S_advance_offset(parser, input, parser->first_nonspace + matched - parser->offset, false);

        } else if (!indented && ((matched = scan_html_block_start(input, parser->first_nonspace)) ||
                                    (cont_type != MARKDOWN_CORE_NODE_PARAGRAPH &&
                                        (matched = scan_html_block_start_7(input, parser->first_nonspace))))) {
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_HTML_BLOCK, parser->first_nonspace + 1);
            if (!*container) {
                return;
            }
            (*container)->as.html_block_type = matched;
            // note, we don't adjust parser->offset because the tag is part of the
            // text
        } else if (!indented && cont_type == MARKDOWN_CORE_NODE_PARAGRAPH &&
                   (lev = scan_setext_heading_line(input, parser->first_nonspace))) {
            // finalize paragraph, resolving reference links
            has_content = resolve_reference_link_definitions(parser, *container);

            if (has_content) {

                markdown_core_parser_touch(parser, *container);
                (*container)->type = (uint16_t)MARKDOWN_CORE_NODE_HEADING;
                (*container)->as.heading.level = lev;
                (*container)->as.heading.setext = true;
                S_advance_offset(parser, input, input->len - 1 - parser->offset, false);
            }
        } else if (!indented && !(cont_type == MARKDOWN_CORE_NODE_PARAGRAPH && !all_matched) &&
                   (parser->thematic_break_kill_pos <= parser->first_nonspace) &&
                   (matched = S_scan_thematic_break(parser, input, parser->first_nonspace))) {
            // it's only now that we know the line is not part of a setext heading:
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_THEMATIC_BREAK, parser->first_nonspace + 1);
            if (!*container) {
                return;
            }
            S_advance_offset(parser, input, input->len - 1 - parser->offset, false);
        } else if (!indented && (parser->options & MARKDOWN_CORE_OPT_FOOTNOTES) && depth < MAX_LIST_DEPTH &&
                   (matched = scan_footnote_definition(input, parser->first_nonspace))) {
            markdown_core_chunk c = markdown_core_chunk_dup(input, parser->first_nonspace + 2, matched - 2);

            while (c.data[c.len - 1] != ']') {
                --c.len;
            }
            --c.len;

            if (!markdown_core_chunk_to_cstr(parser->mem, &c)) {
                /* The label would keep borrowing the transient line buffer. */
                parser->oom = true;
                return;
            }

            S_advance_offset(parser, input, parser->first_nonspace + matched - parser->offset, false);
            /* THE ANCHOR RULE (§5.1): a definition is a block node at the byte
             * where its OPENING BRACKET was written. It used to start at the
             * byte after `[^label]:`, which is a column that need not exist --
             * `[^footnote]:` alone on a line is twelve bytes and the definition
             * began at column 13. Every other block in this engine starts at
             * its own first byte and the marker is inside it; a footnote
             * definition was the one that started after its own marker. */
            *container =
                add_child(parser, *container, MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION, parser->first_nonspace + 1);
            if (!*container) {
                markdown_core_chunk_free(parser->mem, &c);
                return;
            }
            /* The identifier KEEPS the caret the label does not carry
             * (markdown_core_association): one key construction carries it,
             * the association adopts it, and the map registration below
             * copies the association's identifier (#125). */
            {
                markdown_core_map_key label_key = {NULL, 0};
                int label_lost = 0;
                if (!markdown_core_map_key_init(parser->mem, &label_key, &c, '^', &label_lost) ||
                    !markdown_core_association_init(parser->mem, &(*container)->as.association, &c, &label_key)) {
                    parser->oom = true;
                    markdown_core_chunk_free(parser->mem, &c);
                    return;
                }
            }
            markdown_core_chunk_free(parser->mem, &c);

            /* The document defines this label from here on.
             *
             * Registered where the label is READ, which is here. Whether it is
             * registered at open or at close is NOT observable and that was
             * measured, not assumed: moving this call into `finalize` leaves
             * every suite and every oracle green. It used to matter, and the
             * reason it stopped is the shape rather than the timing -- the map
             * this replaced held a NODE per entry and used registration order
             * as the tie-break for a repeated label, so on EXIT a definition
             * nested inside another closed first, won the label, and the outer
             * one was freed with everything written in it (D11). An entry owns
             * no node -- the identity beside the label is a value -- and the
             * winner is decided by entry age at preparation, so registration
             * order within a block still decides nothing.
             *
             * The identity registered is the mint `add_child` just made; a
             * footnote definition is never retyped and never harvested, so
             * this is the id the block keeps. */
            markdown_core_footnote_definition_create(
                parser->footnote_defs,
                &(*container)->as.association.identifier,
                (*container)->identifier
            );

            (*container)->internal_offset = matched;
        } else if ((!indented || cont_type == MARKDOWN_CORE_NODE_LIST) && parser->indent < 4 &&
                   depth < MAX_LIST_DEPTH &&
                   (matched = parse_list_marker(
                        parser,
                        input,
                        parser->first_nonspace,
                        (*container)->type == MARKDOWN_CORE_NODE_PARAGRAPH,
                        &data
                    ))) {

            // Note that we can have new list items starting with >= 4
            // spaces indent, as long as the list container is still open.
            int i = 0;

            // compute padding:
            S_advance_offset(parser, input, parser->first_nonspace + matched - parser->offset, false);

            save_partially_consumed_tab = parser->partially_consumed_tab;
            save_offset = parser->offset;
            save_column = parser->column;

            while (parser->column - save_column <= 5 && S_is_space_or_tab(peek_at(input, parser->offset))) {
                S_advance_offset(parser, input, 1, true);
            }

            i = parser->column - save_column;
            if (i >= 5 || i < 1 ||
                // only spaces after list marker:
                S_is_line_end_char(peek_at(input, parser->offset))) {
                data->padding = matched + 1;
                parser->offset = save_offset;
                parser->column = save_column;
                parser->partially_consumed_tab = save_partially_consumed_tab;
                if (i > 0) {
                    S_advance_offset(parser, input, 1, true);
                }
            } else {
                data->padding = matched + i;
            }

            // check container; if it's a list, see if this list item
            // can continue the list; otherwise, create a list container.

            data->marker_offset = parser->indent;

            if (cont_type != MARKDOWN_CORE_NODE_LIST || !lists_match(&((*container)->as.list), data)) {
                *container = add_child(parser, *container, MARKDOWN_CORE_NODE_LIST, parser->first_nonspace + 1);
                if (!*container) {
                    parser->mem->free(data);
                    return;
                }

                memcpy(&((*container)->as.list), data, sizeof(*data));
            }

            // add the list item
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_LIST_ITEM, parser->first_nonspace + 1);
            if (!*container) {
                parser->mem->free(data);
                return;
            }
            memcpy(&((*container)->as.list), data, sizeof(*data));
            parser->mem->free(data);
        } else if (indented && !maybe_lazy && !parser->blank) {
            S_advance_offset(parser, input, CODE_INDENT, true);
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_CODE_BLOCK, parser->offset + 1);
            if (!*container) {
                return;
            }
            (*container)->as.code.fenced = false;
            (*container)->as.code.fence_char = 0;
            (*container)->as.code.fence_length = 0;
            (*container)->as.code.fence_offset = 0;
            (*container)->as.code.fence_closed = false;
            /* An indented code block has no fence and therefore no info
             * string, ever. */
            (*container)->as.code.info = markdown_core_optional_chunk_absent();
        } else {
            markdown_core_llist *tmp;
            markdown_core_node *new_container = NULL;

            for (tmp = parser->syntax_extensions; tmp; tmp = tmp->next) {
                const markdown_core_syntax_extension *ext = (const markdown_core_syntax_extension *)tmp->data;

                if (ext->try_opening_block) {
                    new_container = ext->try_opening_block(ext, indented, parser, *container, input->data, input->len);

                    if (new_container) {
                        *container = new_container;
                        break;
                    }
                }
            }

            if (!new_container) {
                break;
            }
        }

        /* What this opener consumed made the block it just opened, so the
         * block owns it: `> `, `- `, the `#`s of a heading, the opening fence,
         * `[^label]:`. Claimed once per turn of the loop -- once per block
         * opened -- and before `accepts_lines` breaks out. */

        if (accepts_lines(*container)) {
            // if it's a line container, it can't contain other containers
            break;
        }

        cont_type = S_type(*container);
        maybe_lazy = false;
    }
}

static void add_text_to_container(
    markdown_core_parser *parser,
    markdown_core_node *container,
    markdown_core_node *last_matched_container,
    markdown_core_chunk *input
) {
    markdown_core_node *tmp;
    // what remains at parser->offset is a text line.  add the text to the
    // appropriate container.

    S_find_first_nonspace(parser, input);

    if (parser->blank && container->last_child) {
        S_set_last_line_blank(container->last_child, true);
    }

    // block quote lines are never blank as they start with >
    // and we don't count blanks in fenced code for purposes of tight/loose
    // lists or breaking out of lists.  we also don't set last_line_blank
    // on an empty list item.
    const markdown_core_node_type ctype = S_type(container);
    const bool last_line_blank =
        (parser->blank && ctype != MARKDOWN_CORE_NODE_BLOCK_QUOTE && ctype != MARKDOWN_CORE_NODE_HEADING &&
            ctype != MARKDOWN_CORE_NODE_THEMATIC_BREAK && !extension_accepts_lines(container) &&
            !(ctype == MARKDOWN_CORE_NODE_CODE_BLOCK && container->as.code.fenced) &&
            !(ctype == MARKDOWN_CORE_NODE_LIST_ITEM && container->first_child == NULL &&
                container->start_line == parser->line_number));

    S_set_last_line_blank(container, last_line_blank);

    tmp = container;
    while (tmp->parent) {
        S_set_last_line_blank(tmp->parent, false);
        tmp = tmp->parent;
    }

    // If the last line processed belonged to a paragraph node,
    // and we didn't match all of the line prefixes for the open containers,
    // and we didn't start any new containers,
    // and the line isn't blank,
    // then treat this as a "lazy continuation line" and add it to
    // the open paragraph.
    if (parser->current != last_matched_container && container == last_matched_container && !parser->blank &&
        S_type(parser->current) == MARKDOWN_CORE_NODE_PARAGRAPH) {
        add_line(parser->current, input, parser);
    } else { // not a lazy continuation
        // Finalize any blocks that were not matched and set cur to container:
        while (parser->current != last_matched_container) {
            parser->current = finalize(parser, parser->current);
            assert(parser->current != NULL);
        }

        if (S_type(container) == MARKDOWN_CORE_NODE_CODE_BLOCK) {
            add_line(container, input, parser);
        } else if (S_type(container) == MARKDOWN_CORE_NODE_HTML_BLOCK) {
            add_line(container, input, parser);

            int matches_end_condition;
            switch (container->as.html_block_type) {
            case 1:
                // </script>, </style>, </pre>
                matches_end_condition = scan_html_block_end_1(input, parser->first_nonspace);
                break;
            case 2:
                // -->
                matches_end_condition = scan_html_block_end_2(input, parser->first_nonspace);
                break;
            case 3:
                // ?>
                matches_end_condition = scan_html_block_end_3(input, parser->first_nonspace);
                break;
            case 4:
                // >
                matches_end_condition = scan_html_block_end_4(input, parser->first_nonspace);
                break;
            case 5:
                // ]]>
                matches_end_condition = scan_html_block_end_5(input, parser->first_nonspace);
                break;
            default:
                matches_end_condition = 0;
                break;
            }

            if (matches_end_condition) {
                container = finalize(parser, container);
                assert(parser->current != NULL);
            }
        } else if (extension_accepts_lines(container)) {
            add_line(container, input, parser);
        } else if (parser->blank) {
            // ??? do nothing
        } else if (accepts_lines(container)) {
            if (S_type(container) == MARKDOWN_CORE_NODE_HEADING && container->as.heading.setext == false) {
                chop_trailing_hashtags(input);
            }
            S_advance_offset(parser, input, parser->first_nonspace - parser->offset, false);
            add_line(container, input, parser);
        } else {
            // create paragraph container for line
            container = add_child(parser, container, MARKDOWN_CORE_NODE_PARAGRAPH, parser->first_nonspace + 1);
            if (!container) {
                return;
            }
            S_advance_offset(parser, input, parser->first_nonspace - parser->offset, false);
            add_line(container, input, parser);
        }

        parser->current = container;
    }
}

/* See http://spec.commonmark.org/0.24/#phase-1-block-structure */
static void S_process_line(markdown_core_parser *parser, const unsigned char *buffer, bufsize_t bytes) {
    markdown_core_node *last_matched_container;
    bool all_matched = true;
    markdown_core_node *container;
    markdown_core_chunk input;

    if (parser->oom || parser->root == NULL) {
        return;
    }

    markdown_core_strbuf_clear(&parser->curline);

    if (parser->options & MARKDOWN_CORE_OPT_VALIDATE_UTF8) {
        markdown_core_utf8proc_check(&parser->curline, buffer, bytes);
    } else {
        markdown_core_strbuf_put(&parser->curline, buffer, bytes);
    }

    bytes = parser->curline.size;

    // ensure line ends with a newline:
    if (bytes == 0 || !S_is_line_end_char(parser->curline.ptr[bytes - 1])) {
        markdown_core_strbuf_putc(&parser->curline, '\n');
    }

    if (parser->curline.oom) {
        parser->oom = true;
        return;
    }

    parser->offset = 0;
    parser->column = 0;
    parser->first_nonspace = 0;
    parser->first_nonspace_column = 0;
    parser->thematic_break_kill_pos = 0;
    parser->indent = 0;
    parser->blank = false;
    parser->partially_consumed_tab = false;

    input.data = parser->curline.ptr;
    input.len = parser->curline.size;
    input.alloc = 0;
    input.owner = NULL;

    // Skip UTF-8 BOM.
    if (parser->line_number == 0 && input.len >= 3 && memcmp(input.data, "\xef\xbb\xbf", 3) == 0) {
        parser->offset += 3;
    }

    parser->line_number++;

    last_matched_container = check_open_blocks(parser, &input, &all_matched);

    if (!last_matched_container) {
        goto finished;
    }

    container = last_matched_container;

    open_new_blocks(parser, &container, &input, all_matched);

    if (container == NULL || parser->oom) {
        goto finished;
    }

    add_text_to_container(parser, container, last_matched_container, &input);

finished:
    parser->last_line_length = input.len;
    if (parser->last_line_length && input.data[parser->last_line_length - 1] == '\n') {
        parser->last_line_length -= 1;
    }
    if (parser->last_line_length && input.data[parser->last_line_length - 1] == '\r') {
        parser->last_line_length -= 1;
    }

    /* THE SPINE IS STAMPED ONCE PER LINE (T3). A CST block is written only
     * while it is open, and it is open only on this spine -- so every write
     * the core cannot see, an extension's opaque state above all (formula's
     * `closed`, directive's `consume_line`), is covered here whether or not
     * its own site remembered to touch. A block that closed on this line was
     * stamped by `finalize` and is no longer on the spine. */
    {
        markdown_core_node *spine;
        for (spine = parser->current; spine; spine = spine->parent) {
            markdown_core_parser_touch(parser, spine);
        }
    }

    markdown_core_strbuf_clear(&parser->curline);
}

markdown_core_node *markdown_core_parser_finish(markdown_core_parser *parser) {
    markdown_core_node *res;

    /* Parser was already finished once */
    if (parser->root == NULL) {
        return NULL;
    }

    /* The held partial line is the last thing the stream said. If its buffer
     * lost bytes, what is here is a PREFIX, and processing it would commit a
     * line the author did not write. */
    if (parser->linebuf.oom) {
        parser->oom = true;
    } else if (parser->linebuf.size) {
        S_process_line(parser, parser->linebuf.ptr, parser->linebuf.size);
        markdown_core_strbuf_clear(&parser->linebuf);
    }

    /* Close the spine -- the CST is final -- then run the LAST PROJECTION,
     * IN PLACE (T1). `finish` is that projection plus the seal (§12.10 A).
     * Nothing observes the CST after `finish`, so cloning it first was work
     * with no reader, and the one-shot path paid for it in proportion to its
     * block count (docs/STREAMING.md F1). And this is the first and only
     * projection of this CST, so the projection's non-idempotence over the
     * tree it writes cannot bite. The tree the caller gets IS `parser->root`:
     * ownership flips here, and everything below, the reset included, must
     * treat the CST as already gone. `oom` is tested first for the reason
     * `derive_tree` tests it: a lost allocation leaves a tree that is not all
     * there, and there is nothing to project. */
    finalize_document(parser);

    if (parser->oom) {
        res = NULL;
    } else {
        res = S_project(parser, parser->root, parser->refmap);
        parser->root = NULL;
        parser->current = NULL;
    }

    markdown_core_strbuf_free(&parser->curline);
    markdown_core_strbuf_free(&parser->linebuf);

#if MARKDOWN_CORE_DEBUG_NODES
    if (res && markdown_core_node_check(res, stderr)) {
        abort();
    }
#endif

    /* All allocation-loss routes converge here: block/inline structures set
     * parser->oom directly, definition maps carry their own sticky flag. */
    if (parser->refmap && parser->refmap->oom) {
        parser->oom = true;
    }
    /* The definition set is the second such map and it converges here for the
     * same reason: a normalization it could not allocate answers "this label is
     * not defined", which degrades a footnote call to text and looks exactly
     * like a document that never had one. The allocation-failure sweep caught
     * it -- `quote with footnote[^fn] and ` came back as prose with the parse
     * reporting success. */
    if (parser->footnote_defs && parser->footnote_defs->oom) {
        parser->oom = true;
    }
    if (parser->oom) {
        /* Exactly one of these holds the tree: `res` once the projection ran
         * and took it from `parser->root`, `parser->root` when the parse was
         * lost before the projection could run. */
        if (res) {
            markdown_core_node_free(res);
        }
        if (parser->root) {
            markdown_core_node_free(parser->root);
            parser->root = NULL;
        }
        markdown_core_parser_reset(parser);
        return NULL;
    }

    /* The parser's life ends here (ruling A): the reset disposes what is
     * still the parser's. The CST is not among it -- `parser->root` was
     * cleared at the projection because the CST left as the result, and
     * `dispose` frees only what it finds set. */
    markdown_core_parser_reset(parser);

    return res;
}

int markdown_core_parser_get_line_number(markdown_core_parser *parser) { return parser->line_number; }

bufsize_t markdown_core_parser_get_offset(markdown_core_parser *parser) { return parser->offset; }

int markdown_core_parser_get_first_nonspace(markdown_core_parser *parser) { return parser->first_nonspace; }

int markdown_core_parser_get_indent(markdown_core_parser *parser) { return parser->indent; }

int markdown_core_parser_is_blank(markdown_core_parser *parser) { return parser->blank; }

markdown_core_node *markdown_core_parser_add_child(
    markdown_core_parser *parser,
    markdown_core_node *parent,
    markdown_core_node_type block_type,
    int start_column
) {
    return add_child(parser, parent, block_type, start_column);
}

void markdown_core_parser_advance_offset(markdown_core_parser *parser, const char *input, int count, int columns) {
    markdown_core_chunk input_chunk = markdown_core_chunk_literal(input);

    S_advance_offset(parser, &input_chunk, count, columns != 0);
}
