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
#include "extension.h"
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
 * cmark-gfm limits nested footnote definitions to bound its extension path.
 * Lists are not capped: CommonMark permits arbitrary nesting and the core
 * traversal below carries cmark's linear-time blank-line optimization.
 */
#define MAX_FOOTNOTE_DEPTH 100

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

static bool S_strip_html_comments(markdown_core_node *root) {
    bool stripped = false;
    markdown_core_iter *iter = markdown_core_iter_new(root);
    markdown_core_event_type ev_type;

    if (!iter) {
        return false;
    }

    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = markdown_core_iter_get_node(iter);
        /* EXIT, not ENTER: the mutation rule names the node whose EXIT is
         * current, and it is the only moment the iterator's lookahead is
         * outside this node's subtree. `HTML` and `HTML_BLOCK` were both in
         * the old `S_is_leaf` list, so their EXIT was suppressed and freeing
         * at ENTER happened to be safe; with the contract total it is a
         * use-after-free on the very next `markdown_core_iter_next`. */
        if (ev_type == MARKDOWN_CORE_EVENT_EXIT && S_html_literal_starts_with_comment(node)) {
            markdown_core_node_free(node);
            stripped = true;
        }
    }

    markdown_core_iter_free(iter);

    if (stripped) {
        return markdown_core_consolidate_text_nodes(root) != 0;
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

static void S_parse_source(markdown_core_parser *parser, const unsigned char *source, size_t length);
static markdown_core_node *S_finish_parse(markdown_core_parser *parser);

static void S_process_line(markdown_core_parser *parser, const unsigned char *buffer, bufsize_t bytes);

static markdown_core_node *make_block(markdown_core_mem *mem, markdown_core_node_type tag, int start_line,
                                      int start_column) {
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

/* Both extension lists hold pointers to `static const` descriptors, and every
 * reader casts `data` straight back to a
 * `const markdown_core_extension *`. The const is discarded here and
 * nowhere else because markdown_core_llist is a generic list that cannot
 * carry it; typing the parameter keeps the cast to this one line. */
static int S_extension_list_append(markdown_core_mem *mem, markdown_core_llist **head,
                                   const markdown_core_extension *extension) {
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

int markdown_core_parser_attach_extension(markdown_core_parser *parser, const markdown_core_extension *extension) {
    if (!S_extension_list_append(parser->mem, &parser->extensions, extension)) {
        return 0;
    }
    if (extension->match_inline || extension->insert_inline_from_delim) {
        if (!S_extension_list_append(parser->mem, &parser->inline_extensions, extension)) {
            return 0;
        }
    }

    return 1;
}

static void S_parser_dispose(markdown_core_parser *parser) {
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

    /* The content-to-source map outlives every block that indexes it and
     * nothing else does, so it is released here rather than with the node. */
    parser->mem->free(parser->line_marks);
    parser->line_marks = NULL;
    parser->line_marks_size = 0;
    parser->line_marks_alloc = 0;
}

static markdown_core_parser *S_parser_new(int options, markdown_core_mem *mem) {
    markdown_core_parser *parser;
    markdown_core_node *document;

    if (!mem) {
        return NULL;
    }
    parser = (markdown_core_parser *)mem->calloc(1, sizeof(*parser));
    if (!parser) {
        return NULL;
    }
    parser->mem = mem;
    parser->options = options;
    markdown_core_strbuf_init(parser->mem, &parser->curline, 256);
    markdown_core_strbuf_init(parser->mem, &parser->line_scratch, 0);

    document = make_document(parser->mem);
    parser->refmap = markdown_core_reference_map_new(parser->mem);
    parser->footnote_defs = markdown_core_footnote_definition_map_new(parser->mem);
    parser->root = document;
    parser->current = document;

    /* A transaction that could not build its initial structures is poisoned:
     * source processing becomes a no-op and the parse reports failure. */
    if (!parser->root || !parser->refmap || !parser->footnote_defs || parser->curline.oom || parser->line_scratch.oom ||
        parser->root->content.oom) {
        parser->oom = true;
    }

    markdown_core_inlines_reset_special_chars(parser);
    return parser;
}

static void S_parser_free(markdown_core_parser *parser) {
    markdown_core_mem *mem;
    if (!parser) {
        return;
    }
    mem = parser->mem;
    S_parser_dispose(parser);
    markdown_core_strbuf_free(&parser->curline);
    markdown_core_strbuf_free(&parser->line_scratch);
    markdown_core_llist_free(parser->mem, parser->extensions);
    markdown_core_llist_free(parser->mem, parser->inline_extensions);
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

    return (block_type == MARKDOWN_CORE_NODE_PARAGRAPH || block_type == MARKDOWN_CORE_NODE_HEADING ||
            block_type == MARKDOWN_CORE_NODE_CODE_BLOCK);
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
        grown = (markdown_core_line_mark *)parser->mem->realloc(parser->line_marks,
                                                                (size_t)alloc * sizeof(markdown_core_line_mark));
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
        grown = (markdown_core_line_mark *)parser->mem->realloc(parser->line_marks,
                                                                (size_t)alloc * sizeof(markdown_core_line_mark));
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
int markdown_core_parser_adopt_content_marks(markdown_core_parser *parser, markdown_core_node *owner,
                                             markdown_core_node *node, bufsize_t from, bufsize_t length) {
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
        grown = (markdown_core_line_mark *)parser->mem->realloc(parser->line_marks,
                                                                (size_t)alloc * sizeof(markdown_core_line_mark));
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
int markdown_core_parser_content_place(markdown_core_parser *parser, markdown_core_node *node, bufsize_t content_offset,
                                       int *line, int *column) {
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
static void S_rebase_content_marks(markdown_core_parser *parser, markdown_core_node *node, bufsize_t dropped,
                                   bufsize_t remaining) {
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
static markdown_core_node *S_new_reference_definition(markdown_core_parser *parser, markdown_core_node *b,
                                                      bufsize_t from, bufsize_t upto,
                                                      const markdown_core_reference_parts *parts) {
    markdown_core_node *node;
    markdown_core_definition *definition;
    markdown_core_chunk url = parts->url;
    markdown_core_chunk title = parts->title;
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

    if (!markdown_core_association_init(parser->mem, &definition->association, &parts->label, 0)) {
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
    markdown_core_chunk chunk = {node_content->ptr, node_content->size, 0};
    markdown_core_reference_parts parts;
    bufsize_t consumed = 0;
    while (chunk.len && chunk.data[0] == '[' &&
           (pos = markdown_core_parse_reference_inline(parser->mem, &chunk, parser->refmap, &parts))) {
        S_new_reference_definition(parser, b, consumed, consumed + pos, &parts);
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
    return !is_blank(&b->content, 0);
}

static markdown_core_node *finalize(markdown_core_parser *parser, markdown_core_node *b) {
    bufsize_t pos;
    markdown_core_node *item;
    markdown_core_node *subitem;
    markdown_core_node *parent;
    bool has_content;

    parent = b->parent;
    assert(b->flags & MARKDOWN_CORE_NODE__OPEN); // shouldn't call finalize on closed blocks
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
                * line before it. Four of the eleven observed negative rows
                * were this. */
               parser->line_number == b->start_line) {
        S_set_end_to_current_line(parser, b);
    } else {
        b->end_line = parser->line_number - 1;
        b->end_column = parser->last_line_length;
    }

    /* The extension's one chance to read its own block as a finished thing.
     * Placed after the scope is settled and before the switch, because what a
     * close hook has to say is about the whole block. */

    markdown_core_strbuf *node_content = &b->content;

    switch (S_type(b)) {
    case MARKDOWN_CORE_NODE_PARAGRAPH: {
        has_content = resolve_reference_link_definitions(parser, b);
        if (!has_content) {
            // remove blank node (former reference def)
            markdown_core_node_free(b);
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
                /* A buffer that could not be grown has `size == 0` and it is
                 * NOT an absent info string -- it is an info string the parse
                 * lost. The strict OOM sweep requires that loss to terminate
                 * the parse. */
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

    return parent;
}

// Add a node as child of another.  Return pointer to child.
static markdown_core_node *add_child(markdown_core_parser *parser, markdown_core_node *parent,
                                     markdown_core_node_type block_type, int start_column) {
    assert(parent);

    // if 'parent' isn't the kind of node that can accept this child,
    // then back up til we hit a node that can.
    while (!markdown_core_node_can_contain_type(parent, block_type)) {
        parent = finalize(parser, parent);
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

    for (tmp_ext = parser->inline_extensions; tmp_ext; tmp_ext = tmp_ext->next) {
        const markdown_core_extension *ext = (const markdown_core_extension *)tmp_ext->data;
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

// Parse inline content in one child tree. Node-valued fields are separate
// roots and are handed to this function independently by process_inlines.
static void process_inline_tree(markdown_core_parser *parser, markdown_core_node *root, markdown_core_map *refmap,
                                int options) {
    markdown_core_iter *iter = markdown_core_iter_new(root);
    markdown_core_node *cur;
    markdown_core_event_type ev_type;

    if (!iter) {
        parser->oom = true;
        return;
    }

    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        cur = markdown_core_iter_get_node(iter);
        if (ev_type == MARKDOWN_CORE_EVENT_ENTER) {
            if (contains_inlines(cur)) {
                markdown_core_parse_inlines(parser, cur, refmap, options);
            }
        }
    }

    markdown_core_iter_free(iter);
}

typedef struct {
    markdown_core_parser *parser;
    markdown_core_map *refmap;
    int options;
} inline_field_context;

static int process_inline_fields(markdown_core_parser *parser, markdown_core_node *root, markdown_core_map *refmap,
                                 int options);

static int process_inline_field(markdown_core_node **root_slot, void *context) {
    inline_field_context *fields = (inline_field_context *)context;
    markdown_core_node *root = root_slot ? *root_slot : NULL;
    if (!root || fields->parser->oom) {
        return !fields->parser->oom;
    }
    process_inline_tree(fields->parser, root, fields->refmap, fields->options);
    if (fields->parser->oom) {
        return 0;
    }
    return process_inline_fields(fields->parser, root, fields->refmap, fields->options);
}

/* Find node-valued fields from the completed child tree. The owning extension
 * decides which slots exist; each field is parsed as an independent child
 * tree, then scanned for nested fields of its own. */
static int process_inline_fields(markdown_core_parser *parser, markdown_core_node *root, markdown_core_map *refmap,
                                 int options) {
    markdown_core_iter *iter = markdown_core_iter_new(root);
    markdown_core_event_type event;
    inline_field_context context = {parser, refmap, options};

    if (!iter) {
        parser->oom = true;
        return 0;
    }
    while (!parser->oom && (event = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node;
        const markdown_core_extension *extension;
        if (event != MARKDOWN_CORE_EVENT_ENTER) {
            continue;
        }
        node = markdown_core_iter_get_node(iter);
        extension = node->extension;
        if (extension && extension->visit_owned_subtrees_func &&
            !extension->visit_owned_subtrees_func(extension, node, process_inline_field, &context)) {
            parser->oom = true;
        }
    }
    markdown_core_iter_free(iter);
    return !parser->oom;
}

// Parse the structural document tree first, then every detached field tree.
// All individual walks retain ordinary cmark child-only iterator semantics.
static void process_inlines(markdown_core_parser *parser, markdown_core_map *refmap, int options) {
    markdown_core_manage_extensions_special_characters(parser, true);

    process_inline_tree(parser, parser->root, refmap, options);
    if (!parser->oom) {
        process_inline_fields(parser, parser->root, refmap, options);
    }

    markdown_core_manage_extensions_special_characters(parser, false);
}

// Attempts to parse a list item marker (bullet or enumerated).
// On success, returns length of the marker, and populates
// data with the details.  On failure, returns 0.
static bufsize_t parse_list_marker(markdown_core_parser *parser, markdown_core_chunk *input, bufsize_t pos,
                                   bool interrupts_paragraph, markdown_core_list **dataptr) {
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
    return (list_data->list_type == item_data->list_type && list_data->delimiter == item_data->delimiter &&
            // list_data->marker_offset == item_data.marker_offset &&
            list_data->bullet_char == item_data->bullet_char);
}

static markdown_core_node *finalize_document(markdown_core_parser *parser) {
    while (parser->current != parser->root) {
        parser->current = finalize(parser, parser->current);
    }

    finalize(parser, parser->root);

    process_inlines(parser, parser->refmap, parser->options);

    return parser->root;
}

markdown_core_node *markdown_core_parse_document(const char *buffer, size_t len, int options) {
    return markdown_core_parse_document_with_mem(buffer, len, options, markdown_core_get_default_mem_allocator(), NULL,
                                                 NULL);
}

markdown_core_node *markdown_core_parse_document_with_mem(const char *source, size_t length, int options,
                                                          markdown_core_mem *mem, markdown_core_parser_setup_func setup,
                                                          void *context) {
    static const unsigned char empty[] = "";
    markdown_core_parser *parser;
    markdown_core_node *document;

    if ((!source && length != 0) || length > (size_t)(INT32_MAX / 2)) {
        return NULL;
    }
    parser = S_parser_new(options, mem);
    if (!parser) {
        return NULL;
    }
    if (setup && !setup(parser, context)) {
        S_parser_free(parser);
        return NULL;
    }

    S_parse_source(parser, source ? (const unsigned char *)source : empty, length);
    document = S_finish_parse(parser);
    S_parser_free(parser);
    return document;
}

/* One reservation for the whole contribution to a normalized line, and then
 * a test.
 *
 * Reserving first is what makes the refusal atomic: the NUL path writes twice,
 * and a failure between the two writes leaves a line that is neither the old
 * one nor the new one. The arithmetic is done in 64 bits because `bufsize_t` is
 * int32_t and `size + add` is exactly the overflow A4 closed one level down. */
static bool S_line_scratch_reserve(markdown_core_parser *parser, int64_t add) {
    int64_t target = (int64_t)parser->line_scratch.size + add;

    if (add < 0 || target > (int64_t)(INT32_MAX / 2)) {
        parser->line_scratch.oom = 1;
    } else if (add > 0) {
        markdown_core_strbuf_grow(&parser->line_scratch, (bufsize_t)target);
    }
    if (parser->line_scratch.oom) {
        parser->oom = true;
        return false;
    }
    return true;
}

static void S_parse_source(markdown_core_parser *parser, const unsigned char *source, size_t length) {
    const unsigned char *cursor = source;
    const unsigned char *end = source + length;
    static const uint8_t repl[] = {239, 191, 189};

    while (cursor < end && !parser->oom) {
        const unsigned char *eol;
        bufsize_t segment_length;
        bool line_complete;

        for (eol = cursor; eol < end; ++eol) {
            if (S_is_line_end_char(*eol) || *eol == '\0') {
                break;
            }
        }
        line_complete = eol == end || S_is_line_end_char(*eol);
        segment_length = (bufsize_t)(eol - cursor);
        if (line_complete) {
            if (parser->line_scratch.size > 0) {
                if (!S_line_scratch_reserve(parser, segment_length)) {
                    return;
                }
                markdown_core_strbuf_put(&parser->line_scratch, cursor, segment_length);
                S_process_line(parser, parser->line_scratch.ptr, parser->line_scratch.size);
                markdown_core_strbuf_clear(&parser->line_scratch);
            } else {
                S_process_line(parser, cursor, segment_length);
            }
        } else {
            /* Omit the NUL byte and put U+FFFD in its place. */
            if (!S_line_scratch_reserve(parser, (int64_t)segment_length + 3)) {
                return;
            }
            markdown_core_strbuf_put(&parser->line_scratch, cursor, segment_length);
            markdown_core_strbuf_put(&parser->line_scratch, repl, 3);
        }

        cursor += segment_length;
        if (cursor < end) {
            if (*cursor == '\0') {
                cursor++;
            } else {
                if (*cursor == '\r') {
                    cursor++;
                }
                if (cursor < end && *cursor == '\n') {
                    cursor++;
                }
            }
        }
    }

    /* A final NUL has no line terminator to trigger the completed line. */
    if (!parser->oom && parser->line_scratch.size > 0) {
        S_process_line(parser, parser->line_scratch.ptr, parser->line_scratch.size);
        markdown_core_strbuf_clear(&parser->line_scratch);
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

static bool parse_footnote_definition_block_prefix(markdown_core_parser *parser, markdown_core_chunk *input,
                                                   markdown_core_node *container) {
    if (parser->indent >= 4) {
        S_advance_offset(parser, input, 4, true);
        return true;
    } else if (input->len > 0 && (input->data[0] == '\n' || (input->data[0] == '\r' && input->data[1] == '\n'))) {
        return true;
    }

    return false;
}

static bool parse_node_item_prefix(markdown_core_parser *parser, markdown_core_chunk *input,
                                   markdown_core_node *container) {
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

static bool parse_code_block_prefix(markdown_core_parser *parser, markdown_core_chunk *input,
                                    markdown_core_node *container, bool *should_continue) {
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

static bool parse_extension_block(markdown_core_parser *parser, markdown_core_node *container,
                                  markdown_core_chunk *input, bool *should_continue) {
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
static markdown_core_node *check_open_blocks(markdown_core_parser *parser, markdown_core_chunk *input,
                                             bool *all_matched) {
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
        case MARKDOWN_CORE_NODE_LIST:
            /* A second consecutive blank line inside a deeply nested list
             * cannot open a block and all closable descendants were already
             * closed by the first. Returning NULL avoids walking the same
             * nesting spine for every remaining blank line. Raw-line leaves
             * still own the blank line, including extension-provided leaves. */
            if (parser->blank) {
                if ((container->flags & MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK) && parser->indent == 0) {
                    if (S_type(parser->current) == MARKDOWN_CORE_NODE_CODE_BLOCK ||
                        S_type(parser->current) == MARKDOWN_CORE_NODE_HTML_BLOCK ||
                        extension_accepts_lines(parser->current)) {
                        add_line(parser->current, input, parser);
                    }
                    return NULL;
                }
                container->flags |= MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK;
            } else {
                container->flags &= ~MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK;
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

static void open_new_blocks(markdown_core_parser *parser, markdown_core_node **container, markdown_core_chunk *input,
                            bool all_matched) {
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
                                 (cont_type != MARKDOWN_CORE_NODE_PARAGRAPH && !maybe_lazy &&
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
        } else if (!indented && (parser->options & MARKDOWN_CORE_OPT_FOOTNOTES) && depth < MAX_FOOTNOTE_DEPTH &&
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
             * (markdown_core_association). */
            if (!markdown_core_association_init(parser->mem, &(*container)->as.association, &c, '^')) {
                parser->oom = true;
                markdown_core_chunk_free(parser->mem, &c);
                return;
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
             * one was freed with everything written in it (D11). A set of
             * labels owns no node and picks no winner, so order decides
             * nothing left to get wrong. */
            markdown_core_footnote_definition_create(parser->footnote_defs, &(*container)->as.literal);

            (*container)->internal_offset = matched;
        } else if ((!indented || cont_type == MARKDOWN_CORE_NODE_LIST) && parser->indent < 4 &&
                   (matched = parse_list_marker(parser, input, parser->first_nonspace,
                                                (*container)->type == MARKDOWN_CORE_NODE_PARAGRAPH, &data))) {

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

            for (tmp = parser->extensions; tmp; tmp = tmp->next) {
                const markdown_core_extension *ext = (const markdown_core_extension *)tmp->data;

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

static void add_text_to_container(markdown_core_parser *parser, markdown_core_node *container,
                                  markdown_core_node *last_matched_container, markdown_core_chunk *input) {
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

    markdown_core_strbuf_put(&parser->curline, buffer, bytes);

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

    markdown_core_strbuf_clear(&parser->curline);
}

typedef int (*tree_phase_func)(markdown_core_parser *parser, markdown_core_node **root_slot, void *context);

typedef struct {
    markdown_core_parser *parser;
    tree_phase_func phase;
    void *phase_context;
} tree_phase_context;

static int S_apply_tree_phase(markdown_core_parser *parser, markdown_core_node **root_slot, tree_phase_func phase,
                              void *context);

static int S_apply_field_phase(markdown_core_node **root_slot, void *context) {
    tree_phase_context *phase = (tree_phase_context *)context;
    return S_apply_tree_phase(phase->parser, root_slot, phase->phase, phase->phase_context);
}

/* Apply one parser phase to every independent child tree, deepest fields
 * first. Field slots are requested from their live owner and consumed
 * immediately, so temporary inline nodes can never leave dangling registry
 * entries and a phase may replace a field root safely. */
static int S_apply_tree_phase(markdown_core_parser *parser, markdown_core_node **root_slot, tree_phase_func phase,
                              void *context) {
    markdown_core_node *root = root_slot ? *root_slot : NULL;
    markdown_core_iter *iter;
    markdown_core_event_type event;
    tree_phase_context fields = {parser, phase, context};

    if (!root || parser->oom) {
        return !parser->oom;
    }
    iter = markdown_core_iter_new(root);
    if (!iter) {
        parser->oom = true;
        return 0;
    }
    while (!parser->oom && (event = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node;
        const markdown_core_extension *extension;
        if (event != MARKDOWN_CORE_EVENT_ENTER) {
            continue;
        }
        node = markdown_core_iter_get_node(iter);
        extension = node->extension;
        if (extension && extension->visit_owned_subtrees_func &&
            !extension->visit_owned_subtrees_func(extension, node, S_apply_field_phase, &fields)) {
            parser->oom = true;
        }
    }
    markdown_core_iter_free(iter);
    if (parser->oom) {
        return 0;
    }
    return phase(parser, root_slot, context);
}

static int S_consolidate_tree(markdown_core_parser *parser, markdown_core_node **root_slot, void *context) {
    (void)context;
    return markdown_core_consolidate_text_nodes_with_parser(parser, *root_slot);
}

#if MARKDOWN_CORE_DEBUG_NODES
static int S_check_tree(markdown_core_parser *parser, markdown_core_node **root_slot, void *context) {
    (void)parser;
    (void)context;
    return markdown_core_node_check(*root_slot, stderr) == 0;
}
#endif

static int S_postprocess_tree(markdown_core_parser *parser, markdown_core_node **root_slot, void *context) {
    const markdown_core_extension *extension = (const markdown_core_extension *)context;
    markdown_core_node *processed = extension->postprocess_func(extension, parser, *root_slot);
    if (processed) {
        *root_slot = processed;
    }
    return !parser->oom;
}

static int S_strip_comments_tree(markdown_core_parser *parser, markdown_core_node **root_slot, void *context) {
    (void)parser;
    (void)context;
    return S_strip_html_comments(*root_slot);
}

static markdown_core_node *S_finish_parse(markdown_core_parser *parser) {
    markdown_core_node *res;
    markdown_core_llist *extensions;

    if (parser->root == NULL || parser->oom) {
        return NULL;
    }

    finalize_document(parser);

    /* Map failures are sticky on the maps because reference resolution owns
     * those allocations. Pull them into the transaction flag at every phase
     * boundary so no later transform runs on a failed parse. */
    if ((parser->refmap && parser->refmap->oom) || (parser->footnote_defs && parser->footnote_defs->oom)) {
        parser->oom = true;
    }
    if (parser->oom) {
        goto failed;
    }

    if (!S_apply_tree_phase(parser, &parser->root, S_consolidate_tree, NULL)) {
        parser->oom = true;
    }
    if (parser->oom) {
        goto failed;
    }

#if MARKDOWN_CORE_DEBUG_NODES
    if (!S_apply_tree_phase(parser, &parser->root, S_check_tree, NULL)) {
        abort();
    }
#endif

    for (extensions = parser->extensions; extensions && !parser->oom; extensions = extensions->next) {
        const markdown_core_extension *ext = (const markdown_core_extension *)extensions->data;
        if (ext->postprocess_func) {
            if (!S_apply_tree_phase(parser, &parser->root, S_postprocess_tree, (void *)ext)) {
                parser->oom = true;
            }
        }
    }
    if (parser->oom) {
        goto failed;
    }

    if (parser->options & MARKDOWN_CORE_OPT_STRIP_HTML_COMMENTS) {
        if (!S_apply_tree_phase(parser, &parser->root, S_strip_comments_tree, NULL)) {
            parser->oom = true;
        }
    }

    if (parser->oom) {
        goto failed;
    }

    res = parser->root;
    parser->root = NULL;
    return res;

failed:
    markdown_core_node_free(parser->root);
    parser->root = NULL;
    return NULL;
}

int markdown_core_parser_get_line_number(markdown_core_parser *parser) { return parser->line_number; }

bufsize_t markdown_core_parser_get_offset(markdown_core_parser *parser) { return parser->offset; }

bufsize_t markdown_core_parser_get_column(markdown_core_parser *parser) { return parser->column; }

int markdown_core_parser_get_first_nonspace(markdown_core_parser *parser) { return parser->first_nonspace; }

int markdown_core_parser_get_first_nonspace_column(markdown_core_parser *parser) {
    return parser->first_nonspace_column;
}

int markdown_core_parser_get_indent(markdown_core_parser *parser) { return parser->indent; }

int markdown_core_parser_is_blank(markdown_core_parser *parser) { return parser->blank; }

int markdown_core_parser_has_partially_consumed_tab(markdown_core_parser *parser) {
    return parser->partially_consumed_tab;
}

int markdown_core_parser_get_last_line_length(markdown_core_parser *parser) { return parser->last_line_length; }

markdown_core_node *markdown_core_parser_add_child(markdown_core_parser *parser, markdown_core_node *parent,
                                                   markdown_core_node_type block_type, int start_column) {
    return add_child(parser, parent, block_type, start_column);
}

void markdown_core_parser_advance_offset(markdown_core_parser *parser, const char *input, int count, int columns) {
    markdown_core_chunk input_chunk = markdown_core_chunk_literal(input);

    S_advance_offset(parser, &input_chunk, count, columns != 0);
}

void markdown_core_parser_set_backslash_ispunct_func(markdown_core_parser *parser, markdown_core_ispunct_func func) {
    parser->backslash_ispunct = func;
}
