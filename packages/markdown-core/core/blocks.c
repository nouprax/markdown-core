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
#include <string.h>

#include "markdown_core_ctype.h"
#include "concrete_records.h"
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
#include "delimiter.h"

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

static void S_process_line(markdown_core_parser *parser, const unsigned char *buffer, markdown_core_bufsize bytes);

static markdown_core_node *make_block(
    markdown_core_mem *mem,
    markdown_core_node_type tag,
    int start_line,
    int start_column
) {
    markdown_core_node *e;

    e = (markdown_core_node *)mem->calloc(mem, 1, sizeof(*e));
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

/* Appends `data`; on allocation failure the list is unchanged and 0 is
 * returned. */
static int S_llist_append_checked(markdown_core_mem *mem, markdown_core_llist **head, void *data) {
    markdown_core_llist *node = (markdown_core_llist *)mem->calloc(mem, 1, sizeof(*node));
    markdown_core_llist *tail;
    if (!node) {
        return 0;
    }
    node->data = data;
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

int markdown_core_parser_attach_extension(markdown_core_parser *parser, markdown_core_extension *extension) {
    markdown_core_llist *existing;
    markdown_core_inline_attachment *attachment = NULL;
    markdown_core_delimiter_result attachment_result;

    if (!parser || !extension || !parser->inline_config || parser->feed_started) {
        return 0;
    }
    for (existing = parser->extensions; existing; existing = existing->next) {
        if (existing->data == extension) {
            return 0;
        }
    }
    if (extension->match_inline || extension->delimiter_rule_count) {
        attachment_result = markdown_core_inline_attachment_prepare(parser->inline_config, extension, &attachment);
        if (attachment_result != MARKDOWN_CORE_DELIMITER_OK) {
            if (attachment_result == MARKDOWN_CORE_DELIMITER_OOM) {
                parser->oom = true;
            }
            return 0;
        }
    }
    if (!S_llist_append_checked(parser->mem, &parser->extensions, extension)) {
        markdown_core_inline_attachment_discard(parser->inline_config, attachment);
        parser->oom = true;
        return 0;
    }
    if (attachment) {
        markdown_core_inline_attachment_commit(parser->inline_config, attachment);
    }
    return 1;
}

/* Both definition maps, asked as one question: a parse missing either cannot
 * be trusted, and reset and renew poison the parser on the same terms. */
static bool S_definition_maps_ready(const markdown_core_parser *parser) {
    return parser->refmap != NULL && parser->footnote_defs != NULL;
}

static void markdown_core_parser_dispose(markdown_core_parser *parser) {
    if (parser->root) {
        markdown_core_node_free(parser->root);
    }

    /* map_free tolerates NULL, which a parser poisoned at reset can hold. */
    markdown_core_map_free(parser->refmap);
    markdown_core_map_free(parser->footnote_defs);

    if (parser->diagnostics) {
        parser->mem->free(parser->mem, parser->diagnostics);
        parser->diagnostics = NULL;
        parser->diagnostic_count = 0;
        parser->diagnostic_capacity = 0;
    }

    if (parser->line_marks) {
        parser->mem->free(parser->mem, parser->line_marks);
        parser->line_marks = NULL;
        parser->line_mark_count = 0;
        parser->line_mark_capacity = 0;
    }
}

static void markdown_core_parser_reset(markdown_core_parser *parser) {
    markdown_core_llist *saved_exts = parser->extensions;
    markdown_core_inline_config *saved_inline_config = parser->inline_config;
    markdown_core_delimiter_engine saved_inline_delimiters = parser->inline_delimiters;
    int saved_options = parser->options;
    markdown_core_mem *saved_mem = parser->mem;

    markdown_core_parser_dispose(parser);

    memset(parser, 0, sizeof(markdown_core_parser));
    parser->mem = saved_mem;

    markdown_core_strbuf_init(parser->mem, &parser->curline, 256);
    markdown_core_strbuf_init(parser->mem, &parser->linebuf, 0);

    markdown_core_node *document = make_document(parser->mem);

    parser->refmap = markdown_core_reference_map_new(parser->mem);
    parser->footnote_defs = markdown_core_footnote_definition_map_new(parser->mem);
    parser->root = document;
    parser->current = document;

    parser->extensions = saved_exts;
    parser->inline_config = saved_inline_config;
    parser->inline_delimiters = saved_inline_delimiters;
    parser->options = saved_options;

    /* A reset that could not rebuild its structures poisons the parser: feed
     * becomes a no-op and finish reports failure. */
    if (!parser->root || !S_definition_maps_ready(parser) || parser->curline.oom) {
        parser->oom = true;
    }
}

markdown_core_parser *markdown_core_parser_new_with_mem(int options, markdown_core_mem *mem) {
    markdown_core_parser *parser = (markdown_core_parser *)mem->calloc(mem, 1, sizeof(markdown_core_parser));
    if (!parser) {
        return NULL;
    }
    parser->mem = mem;
    parser->options = options;
    parser->inline_config = markdown_core_inlines_new_config(mem);
    if (!parser->inline_config) {
        mem->free(mem, parser);
        return NULL;
    }
    markdown_core_delimiter_engine_init(&parser->inline_delimiters, mem);
    markdown_core_parser_reset(parser);
    return parser;
}

markdown_core_parser *markdown_core_parser_new(int options) {
    return markdown_core_parser_new_with_mem(options, markdown_core_mem_default());
}

void markdown_core_parser_free(markdown_core_parser *parser) {
    markdown_core_mem *mem = parser->mem;
    markdown_core_parser_dispose(parser);
    markdown_core_strbuf_free(&parser->curline);
    markdown_core_strbuf_free(&parser->linebuf);
    markdown_core_llist_free(parser->mem, parser->extensions);
    markdown_core_inline_config_free(parser->inline_config);
    markdown_core_delimiter_engine_free(&parser->inline_delimiters);
    mem->free(mem, parser);
}

static markdown_core_node *finalize(markdown_core_parser *parser, markdown_core_node *b);

// Returns true if line has only space characters, else false.
static bool is_blank(markdown_core_strbuf *s, markdown_core_bufsize offset) {
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
    return node->extension && node->extension->accepts_lines &&
           node->extension->accepts_lines(node->extension, node) != 0;
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
    return markdown_core_node_owns_inlines(node);
}

/* Records where the line about to be appended to the open paragraph came
 * from. A lost mark poisons the parse rather than degrading quietly: without
 * it the definitions on that line get a fallback position, so the parse would
 * succeed with a different tree than the same input produces when the
 * allocation succeeds. That is precisely what the OOM sweep compares, and a
 * silently different tree is worse than a reported failure. */
static void S_record_line_mark(markdown_core_parser *parser, const markdown_core_node *node) {
    struct markdown_core_line_mark *grown;
    size_t capacity;

    if (S_type(node) != MARKDOWN_CORE_NODE_PARAGRAPH) {
        return;
    }
    if (parser->line_mark_count == parser->line_mark_capacity) {
        capacity = parser->line_mark_capacity ? parser->line_mark_capacity * 2 : 8;
        grown = (struct markdown_core_line_mark *)
                    parser->mem->realloc(parser->mem, parser->line_marks, capacity * sizeof(*grown));
        if (!grown) {
            parser->oom = true;
            return;
        }
        parser->line_marks = grown;
        parser->line_mark_capacity = capacity;
    }
    parser->line_marks[parser->line_mark_count].content_offset = node->content.size;
    parser->line_marks[parser->line_mark_count].line = parser->line_number;
    parser->line_marks[parser->line_mark_count].column = (int)parser->column + 1;
    parser->line_marks[parser->line_mark_count].byte_offset = parser->offset;
    /* The same count add_line's pad loop is about to write, recorded so the
     * mark can undo it: those spaces stand in for the one tab byte still
     * sitting at parser->offset. */
    parser->line_marks[parser->line_mark_count].pad =
        parser->partially_consumed_tab ? TAB_STOP - (parser->column % TAB_STOP) : 0;
    parser->line_mark_count++;
}

static void add_line(markdown_core_node *node, markdown_core_chunk *ch, markdown_core_parser *parser) {
    int chars_to_tab;
    int i;
    assert(node->flags & MARKDOWN_CORE_NODE__OPEN);
    S_record_line_mark(parser, node);
    if (parser->partially_consumed_tab) {
        parser->offset += 1; // skip over tab
        // add space characters:
        chars_to_tab = TAB_STOP - (parser->column % TAB_STOP);
        for (i = 0; i < chars_to_tab; i++) {
            markdown_core_strbuf_putc(&node->content, ' ');
        }
    }
    markdown_core_strbuf_put(&node->content, ch->data + parser->offset, ch->len - parser->offset);
    if (node->content.oom) {
        parser->oom = true;
    }
}

/* Captures one concrete marker record on the node whose ownership region the
 * marker belongs to (11.1): the `>` on its BlockQuote, the bullet on its
 * ListItem, the fence lines on their CodeBlock. `column` and `length` are
 * byte extents within the current normalized line; the stored line is the
 * offset from the node's own first line, which is what keeps every record
 * region-relative — a record repeats no coordinate the owning node already
 * carries, so a node's placement lives on the node alone and a record
 * resolves through the node it is reached from. Capture runs only while the
 * node's parse is live, when node->start_line is already the absolute
 * opening line the subtraction needs.
 *
 * A lost record poisons the parse on the same terms as a lost line mark: a
 * parse that succeeded with silently thinner concrete material would differ
 * from the same input parsed without the allocation failure, which is
 * precisely what the OOM sweep compares. The poison is deferred to the end
 * of the current line rather than raised here: several capture sites sit
 * between add_child's can-contain finalizes and add_text_to_container's
 * re-anchoring of parser->current, and others precede the content append
 * that establishes a fenced block's info line, so cutting the line short at
 * the S_process_line oom guard would leave block structure violating the
 * invariants finalize asserts. The line runs to completion — every capture
 * failure leaves the tree exactly as a successful capture would — and the
 * loss becomes parser->oom at the line boundary, before the next line
 * parses. */
void markdown_core_parser_capture_marker_at(
    markdown_core_parser *parser,
    markdown_core_node *node,
    uint8_t kind,
    int line,
    markdown_core_bufsize column,
    markdown_core_bufsize length
) {
    if (!markdown_core_concrete_records_append(
            parser->mem,
            &node->concrete,
            kind,
            (uint32_t)(line - node->start_line),
            (uint32_t)column,
            (uint32_t)length
        )) {
        parser->capture_lost = true;
    }
}

void markdown_core_parser_capture_marker(
    markdown_core_parser *parser,
    markdown_core_node *node,
    uint8_t kind,
    markdown_core_bufsize column,
    markdown_core_bufsize length
) {
    markdown_core_parser_capture_marker_at(parser, node, kind, parser->line_number, column, length);
}

/* The pad case in one place: `pad` buffered spaces stand in for the single
 * tab byte at `byte_offset`, so an extent that begins inside them begins on
 * the tab byte, and everything past them sits one byte — not `pad` bytes —
 * after it. `x1` cannot land inside the pad (the header states why), so the
 * end maps unconditionally through the past-pad arm. */
markdown_core_bufsize markdown_core_line_mark_extent(
    const struct markdown_core_line_mark *mark,
    markdown_core_bufsize x0,
    markdown_core_bufsize x1,
    markdown_core_bufsize *column
) {
    markdown_core_bufsize pad = (markdown_core_bufsize)mark->pad;
    markdown_core_bufsize d0 = x0 - mark->content_offset;
    markdown_core_bufsize d1 = x1 - mark->content_offset;
    markdown_core_bufsize start = d0 < pad ? mark->byte_offset : mark->byte_offset + (pad ? 1 : 0) + (d0 - pad);
    markdown_core_bufsize end = mark->byte_offset + (pad ? 1 : 0) + (d1 - pad);

    *column = start;
    return end - start;
}

void markdown_core_parser_record_diagnostic(
    markdown_core_parser *parser,
    int code,
    int start_line,
    int start_column,
    int end_line,
    int end_column
) {
    struct markdown_core_parser_diagnostic *grown;
    size_t capacity;

    if (parser->diagnostic_count == parser->diagnostic_capacity) {
        capacity = parser->diagnostic_capacity ? parser->diagnostic_capacity * 2 : 4;
        grown = (struct markdown_core_parser_diagnostic *)
                    parser->mem->realloc(parser->mem, parser->diagnostics, capacity * sizeof(*grown));
        if (!grown) {
            return;
        }
        parser->diagnostics = grown;
        parser->diagnostic_capacity = capacity;
    }
    parser->diagnostics[parser->diagnostic_count].code = code;
    parser->diagnostics[parser->diagnostic_count].start_line = start_line;
    parser->diagnostics[parser->diagnostic_count].start_column = start_column;
    parser->diagnostics[parser->diagnostic_count].end_line = end_line;
    parser->diagnostics[parser->diagnostic_count].end_column = end_column;
    parser->diagnostic_count++;
}

static void remove_trailing_blank_lines(markdown_core_strbuf *ln) {
    markdown_core_bufsize i;
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
    } else if (
        (S_type(node) == MARKDOWN_CORE_NODE_LIST || S_type(node) == MARKDOWN_CORE_NODE_LIST_ITEM) && node->last_child
    ) {
        S_set_last_line_checked(node);
        return (S_ends_with_blank_line(node->last_child));
    } else {
        S_set_last_line_checked(node);
        return (S_last_line_blank(node));
    }
}

/* Maps an offset in the open paragraph's accumulated content back to the
 * source position those bytes came from. Marks ascend in `content_offset`,
 * and definitions are consumed front to back, so `*cursor` only ever moves
 * forward: the whole harvest costs one pass over the marks rather than a scan
 * per definition, which is what a paragraph of N definitions needs to stay
 * linear. */
static void S_content_position(
    const markdown_core_parser *parser,
    const markdown_core_node *b,
    markdown_core_bufsize offset,
    size_t *cursor,
    int *line,
    int *column
) {
    const struct markdown_core_line_mark *mark;

    while (*cursor + 1 < parser->line_mark_count && parser->line_marks[*cursor + 1].content_offset <= offset) {
        (*cursor)++;
    }
    if (parser->line_mark_count == 0 || parser->line_marks[*cursor].content_offset > offset) {
        *line = b->start_line;
        *column = b->start_column;
        return;
    }
    mark = &parser->line_marks[*cursor];
    *line = mark->line;
    /* Through the extent helper, and NOT through `mark->column`.
     *
     * `column` is the block parser's tab-expanded counter, which cmark calls
     * a VIRTUAL column (src/blocks.c:867) and never turns into a position:
     * every node position in this engine is a byte column, from
     * `add_child(first_nonspace + 1)` to a finalized `end_column`. Writing
     * the expanded one here made a node placed through a mark speak a
     * language no node beside it spoke — on a definition followed by a
     * TAB-indented continuation the paragraph came out `2:5..2:10` around
     * its own Text child at `2:5..2:13`, a child overrunning its parent.
     * Four spaces in place of the tab expand to the same number and hid it,
     * which is why no fixture caught it. The helper is also where the
     * stand-in pad of a mid-tab lazy continuation is undone, and the `+ 1`
     * is the zero-based record column becoming a one-based node position. */
    {
        markdown_core_bufsize record_column;
        markdown_core_line_mark_extent(mark, offset, offset, &record_column);
        *column = (int)record_column + 1;
    }
}

/* Captures one reference-definition spelling as concrete records on `node`:
 * one record per line the content extent [x0, x1) spans, each mapped onto
 * its normalized source line through the paragraph's marks (the trailing
 * newline of every buffered line is excluded — records never cover an EOL).
 * The cursor is the caller's monotone walk over the marks, exactly
 * S_content_position's discipline: spellings are captured front to back, so
 * the whole harvest stays one pass. A paragraph that reached its harvest
 * with no marks at all lost a mark allocation — S_record_line_mark already
 * poisoned that parse, so the spelling has nowhere it could be anchored and
 * the capture is skipped rather than invented. */
static void S_capture_definition_span(
    markdown_core_parser *parser,
    markdown_core_node *node,
    const markdown_core_node *b,
    uint8_t kind,
    markdown_core_bufsize x0,
    markdown_core_bufsize x1,
    size_t *cursor
) {
    if (parser->line_mark_count == 0) {
        return;
    }
    while (x0 < x1) {
        const struct markdown_core_line_mark *mark;
        markdown_core_bufsize next;
        markdown_core_bufsize high;

        while (*cursor + 1 < parser->line_mark_count && parser->line_marks[*cursor + 1].content_offset <= x0) {
            (*cursor)++;
        }
        mark = &parser->line_marks[*cursor];
        next = *cursor + 1 < parser->line_mark_count ? parser->line_marks[*cursor + 1].content_offset
                                                     : (markdown_core_bufsize)b->content.size;
        /* Every buffered line ends in its newline and holds at least one
         * byte before it (a blank line closes the paragraph instead of
         * entering it), and x0 is a spelling byte or a line start — never
         * the newline itself — so the clamped segment is always nonempty
         * and the record length never 0. */
        high = x1 < next - 1 ? x1 : next - 1;
        {
            markdown_core_bufsize column;
            markdown_core_bufsize length = markdown_core_line_mark_extent(mark, x0, high, &column);
            markdown_core_parser_capture_marker_at(parser, node, kind, mark->line, column, length);
        }
        x0 = next;
    }
}

/* Emits the Definition node for the paragraph-content span [start, end),
 * inserted before the paragraph so the tree keeps source order. The span's
 * trailing line ending and spaces are not part of what was written. */
static void S_emit_definition(
    markdown_core_parser *parser,
    markdown_core_node *b,
    markdown_core_bufsize start,
    markdown_core_bufsize end,
    const markdown_core_reference *harvested,
    size_t *cursor,
    const markdown_core_reference_spans *spans
) {
    markdown_core_node *node;
    markdown_core_chunk span;
    markdown_core_bufsize last = end;
    markdown_core_bufsize label_end;

    while (last > start && (S_is_line_end_char(b->content.ptr[last - 1]) || b->content.ptr[last - 1] == ' ' ||
                            b->content.ptr[last - 1] == '\t')) {
        last--;
    }
    node = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_REFERENCE_DEFINITION, parser->mem);
    if (!node) {
        parser->oom = true;
        return;
    }

    /* The label is the source's, exactly as written between `[` and `]`, the
     * rule FootnoteDefinition already follows; the normalized form matching
     * runs on stays the map's. Destination and title are copied from the
     * entry this harvest just created, so they carry the unescaping done
     * there and the node cannot disagree with the map. */
    span.data = b->content.ptr + start;
    span.len = last - start;
    span.alloc = 0;
    label_end = 1;
    while (label_end < span.len && span.data[label_end] != ']') {
        if (span.data[label_end] == '\\' && label_end + 1 < span.len) {
            label_end++;
        }
        label_end++;
    }
    /* chunk_dup borrows; the node outlives both the paragraph's content
     * buffer and the map entry, so each field takes an owned copy. */
    node->as.definition.label = markdown_core_chunk_borrow(&span, 1, label_end > 1 ? label_end - 1 : 0);
    if (!markdown_core_chunk_to_cstr(parser->mem, &node->as.definition.label)) {
        parser->oom = true;
    }
    if (harvested) {
        node->as.definition.url = markdown_core_chunk_borrow(&harvested->url, 0, harvested->url.len);
        if (!markdown_core_chunk_to_cstr(parser->mem, &node->as.definition.url)) {
            parser->oom = true;
        }
        /* The title only when one was written. `markdown_core_clean_title`
         * hands back the empty CHUNK for a definition that has none, and
         * copying that through `to_cstr` would mint an empty STRING —
         * collapsing the distinction the inline forms already keep, where
         * `[t](/u "")` is a written empty title and `[t](/u)` is no title.
         * The node is calloc'd, so leaving it alone is the empty chunk. */
        if (harvested->title.data) {
            node->as.definition.title = markdown_core_chunk_borrow(&harvested->title, 0, harvested->title.len);
            if (!markdown_core_chunk_to_cstr(parser->mem, &node->as.definition.title)) {
                parser->oom = true;
            }
        }
    }
    S_content_position(parser, b, start, cursor, &node->start_line, &node->start_column);
    /* The spellings, front to back with their own cursor copy: the shared
     * cursor must not run ahead before the end position resolves. Capture
     * follows the position assignment because the record encoding is the
     * line delta from start_line. */
    {
        size_t span_cursor = *cursor;
        S_capture_definition_span(
            parser,
            node,
            b,
            MARKDOWN_CORE_CONCRETE_REFDEF_LABEL,
            start,
            start + spans->label_end,
            &span_cursor
        );
        S_capture_definition_span(
            parser,
            node,
            b,
            MARKDOWN_CORE_CONCRETE_REFDEF_DESTINATION,
            start + spans->url_start,
            start + spans->url_end,
            &span_cursor
        );
        if (spans->title_end > spans->title_start) {
            S_capture_definition_span(
                parser,
                node,
                b,
                MARKDOWN_CORE_CONCRETE_REFDEF_TITLE,
                start + spans->title_start,
                start + spans->title_end,
                &span_cursor
            );
        }
    }
    S_content_position(parser, b, last > start ? last - 1 : start, cursor, &node->end_line, &node->end_column);
    node->flags &= ~MARKDOWN_CORE_NODE__OPEN;
    if (!markdown_core_node_insert_before(b, node)) {
        /* Containment is proved by construction here, so a refusal means the
         * insert lost an allocation. Freeing the node keeps the failure from
         * also leaking it. */
        parser->oom = true;
        markdown_core_node_free(node);
        return;
    }
}

// returns true if content remains after link defs are resolved.
static bool resolve_reference_link_definitions(markdown_core_parser *parser, markdown_core_node *b) {
    markdown_core_bufsize pos;
    markdown_core_strbuf *node_content = &b->content;
    markdown_core_chunk chunk = {node_content->ptr, node_content->size, 0};
    markdown_core_bufsize consumed = 0;
    size_t mark_cursor = 0;
    while (chunk.len && chunk.data[0] == '[') {
        /* The harvest pushes its entry at the head of the live chain, so the
         * head is this definition's exactly when the chain moved. A parse
         * that consumed bytes without adding one — a label normalizing to
         * nothing — leaves the node without a destination rather than
         * borrowing the previous definition's. */
        const markdown_core_map_entry *before = parser->refmap ? parser->refmap->refs : NULL;
        const markdown_core_map_entry *after;
        markdown_core_reference_spans spans;

        pos = markdown_core_parse_reference_inline(parser->mem, &chunk, parser->refmap, &spans);
        if (!pos) {
            break;
        }
        after = parser->refmap ? parser->refmap->refs : NULL;
        S_emit_definition(
            parser,
            b,
            consumed,
            consumed + pos,
            after != before ? (const markdown_core_reference *)after : NULL,
            &mark_cursor,
            &spans
        );
        consumed += pos;
        chunk.data += pos;
        chunk.len -= pos;
    }
    /* The harvested bytes left the content, so the paragraph no longer starts
     * where it opened: its first surviving byte does. Without this the
     * paragraph claims source the definitions occupy — and every inline
     * position inside it is computed from this start, so they were all off by
     * the definitions' lines. The defect predates the ReferenceDefinition
     * node; it was invisible while a definition left nothing to overlap with,
     * and neither parity oracle compares positions. The restart
     * reparses from the surviving line and got this right, which is how the
     * equivalence gate found it. */
    if (consumed) {
        S_content_position(parser, b, consumed, &mark_cursor, &b->start_line, &b->start_column);
    }
    markdown_core_strbuf_drop(node_content, (node_content->size - chunk.len));
    return !is_blank(&b->content, 0);
}

static markdown_core_node *finalize(markdown_core_parser *parser, markdown_core_node *b) {
    markdown_core_bufsize pos;
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
    } else if (
        S_type(b) == MARKDOWN_CORE_NODE_DOCUMENT || (S_type(b) == MARKDOWN_CORE_NODE_CODE_BLOCK && b->as.code.fenced) ||
        (S_type(b) == MARKDOWN_CORE_NODE_HEADING && b->as.heading.setext) ||
        (b->flags & MARKDOWN_CORE_NODE__ENDS_ON_CURRENT_LINE)
    ) {
        b->end_line = parser->line_number;
        b->end_column = parser->curline.size;
        if (b->end_column && parser->curline.ptr[b->end_column - 1] == '\n') {
            b->end_column -= 1;
        }
        if (b->end_column && parser->curline.ptr[b->end_column - 1] == '\r') {
            b->end_column -= 1;
        }
    } else {
        b->end_line = parser->line_number - 1;
        b->end_column = parser->last_line_length;
    }

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
            markdown_core_houdini_unescape_html_f(&tmp, node_content->ptr, pos);
            markdown_core_strbuf_trim(&tmp);
            markdown_core_strbuf_unescape(&tmp);
            b->as.code.info = markdown_core_chunk_buf_detach(&tmp);
            if (!b->as.code.info.data) {
                parser->oom = true;
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

static void S_parse_node_inlines(
    markdown_core_parser *parser,
    markdown_core_node *cur,
    markdown_core_map *refmap,
    int options
) {
    markdown_core_parse_inlines(parser, cur, refmap, options);
}

// Walk through node and all children, recursively, parsing
// string content into inline content where appropriate.
static void process_inlines(markdown_core_parser *parser, markdown_core_map *refmap, int options) {
    markdown_core_iter *iter = markdown_core_iter_new(parser->root);
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
                S_parse_node_inlines(parser, cur, refmap, options);
            }
        }
    }

    markdown_core_iter_free(iter);
}

// Attempts to parse a list item marker (bullet or enumerated).
// On success, returns length of the marker, and populates
// data with the details.  On failure, returns 0.
static markdown_core_bufsize parse_list_marker(
    markdown_core_parser *parser,
    markdown_core_chunk *input,
    markdown_core_bufsize pos,
    bool interrupts_paragraph,
    markdown_core_list **dataptr
) {
    markdown_core_mem *mem = parser->mem;
    unsigned char c;
    markdown_core_bufsize startpos;
    markdown_core_list *data;
    markdown_core_bufsize i;

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

        data = (markdown_core_list *)mem->calloc(mem, 1, sizeof(*data));
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

            data = (markdown_core_list *)mem->calloc(mem, 1, sizeof(*data));
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

void markdown_core_parser_finalize_blocks(markdown_core_parser *parser) {
    if (parser->root == NULL) {
        return;
    }

    if (parser->linebuf.size) {
        S_process_line(parser, parser->linebuf.ptr, parser->linebuf.size);
        markdown_core_strbuf_clear(&parser->linebuf);
    }

    while (parser->current != parser->root) {
        parser->current = finalize(parser, parser->current);
    }

    finalize(parser, parser->root);
}

markdown_core_node *markdown_core_node_parse_document(const char *buffer, size_t len, int options) {
    markdown_core_parser *parser = markdown_core_parser_new(options);
    markdown_core_node *document;

    S_parser_feed(parser, (const unsigned char *)buffer, len, true);

    document = markdown_core_parser_finish(parser);
    markdown_core_parser_free(parser);
    return document;
}

/* --- the warm-state fingerprint ------------------------------------------- */

/* Everything a projection may READ but must not CHANGE, folded into one
 * value. It exists so that "the parser is exactly where it was" becomes a
 * decidable question: a tick that closes the open spine to publish a
 * projection and then undoes the close must restore this value bit for bit,
 * and a journal that misses a write site fails deterministically instead of
 * surfacing later as a wrong tree.
 *
 * The field list is explicit on purpose. A digest built from the engine's own
 * subtree hashes would inherit their blind spots — they sample literals and
 * ignore the parser entirely — so this walks the tree and the parser's own
 * state, and anything added to either must be added here.
 *
 * Cost is O(tree + text) per call, which is a gate's budget rather than a
 * tick's: the callers are test runners. */
static uint64_t fp_mix(uint64_t hash, uint64_t value) {
    hash ^= value + UINT64_C(0x9e3779b97f4a7c15) + (hash << 6) + (hash >> 2);
    hash *= UINT64_C(0xff51afd7ed558ccd);
    return hash ^ (hash >> 33);
}

static uint64_t fp_bytes(uint64_t hash, const unsigned char *bytes, size_t length) {
    /* FNV-1a over the bytes, folded in as one value: the fingerprint's job is
     * detecting a journal that missed something, not resisting an adversary. */
    uint64_t inner = UINT64_C(0xcbf29ce484222325);
    size_t i;
    for (i = 0; i < length; i++) {
        inner = (inner ^ bytes[i]) * UINT64_C(0x100000001b3);
    }
    return fp_mix(fp_mix(hash, length), inner);
}

static uint64_t fp_map(uint64_t hash, const struct markdown_core_map *map) {
    const markdown_core_map_entry *entry;
    if (map == NULL) {
        return fp_mix(hash, 0);
    }
    /* Order matters as much as content: a definition table that lost and
     * regained an entry would otherwise fingerprint the same. */
    for (entry = map->refs; entry; entry = entry->next) {
        const markdown_core_reference *reference = (const markdown_core_reference *)entry;
        hash = fp_bytes(hash, entry->label, strlen((const char *)entry->label));
        hash = fp_mix(hash, (uint64_t)entry->order);
        hash = fp_bytes(hash, (const unsigned char *)reference->url.data, (size_t)reference->url.len);
        hash = fp_bytes(hash, (const unsigned char *)reference->title.data, (size_t)reference->title.len);
    }
    return fp_mix(hash, (uint64_t)map->size);
}

uint64_t markdown_core_parser_warm_fingerprint(const markdown_core_parser *parser) {
    uint64_t hash = UINT64_C(0x5eed) ^ (uint64_t)parser->options;
    markdown_core_iter *iter;
    size_t i;

    /* The line counters, the sticky failure bits, and the two pieces of the
     * held partial line: its bytes, and whether a CR is still waiting for the
     * newline that may complete it. */
    hash = fp_mix(hash, (uint64_t)parser->line_number);
    hash = fp_mix(hash, (uint64_t)parser->last_line_length);
    hash = fp_mix(hash, (uint64_t)parser->feed_started);
    hash = fp_mix(hash, (uint64_t)parser->diagnostic_count);
    hash = fp_mix(
        hash,
        (uint64_t)parser->oom | ((uint64_t)parser->internal_error << 1) | ((uint64_t)parser->capture_lost << 2) |
            ((uint64_t)parser->last_buffer_ended_with_cr << 3) | ((uint64_t)parser->linebuf.oom << 4)
    );
    hash = fp_bytes(hash, (const unsigned char *)parser->linebuf.ptr, parser->linebuf.size);
    hash = fp_bytes(hash, (const unsigned char *)parser->curline.ptr, parser->curline.size);

    /* The whole tree, in document order — not just the open spine. A warm
     * tick reaches closed nodes too (a spine flag cleared on an ancestor, a
     * look-back stamp on a paragraph that stayed a paragraph), and a
     * fingerprint that could not see them would pass while the journal
     * leaked. */
    iter = markdown_core_iter_new(parser->root);
    if (!iter) {
        return fp_mix(hash, UINT64_C(0xdead));
    }
    for (;;) {
        markdown_core_event_type event = markdown_core_iter_next(iter);
        markdown_core_node *node;
        if (event == MARKDOWN_CORE_EVENT_DONE) {
            break;
        }
        if (event != MARKDOWN_CORE_EVENT_ENTER) {
            continue;
        }
        node = markdown_core_iter_get_node(iter);
        hash = fp_mix(hash, (uint64_t)S_type(node));
        hash = fp_mix(hash, (uint64_t)node->flags);
        hash = fp_mix(hash, ((uint64_t)(uint32_t)node->start_line << 32) | (uint32_t)node->start_column);
        hash = fp_mix(hash, ((uint64_t)(uint32_t)node->end_line << 32) | (uint32_t)node->end_column);
        hash = fp_bytes(hash, (const unsigned char *)node->content.ptr, node->content.size);
    }
    markdown_core_iter_free(iter);

    hash = fp_map(hash, parser->refmap);
    hash = fp_map(hash, parser->footnote_defs);

    /* The paragraph's line marks: reset to zero when a paragraph opens, so
     * their storage is reused and a journal must copy rows out rather than
     * trust the array. */
    for (i = 0; i < parser->line_mark_count; i++) {
        const struct markdown_core_line_mark *mark = &parser->line_marks[i];
        hash = fp_mix(hash, (uint64_t)mark->content_offset);
        hash = fp_mix(hash, ((uint64_t)(uint32_t)mark->line << 32) | (uint32_t)mark->column);
        hash = fp_mix(hash, (uint64_t)mark->byte_offset);
        hash = fp_mix(hash, (uint64_t)mark->pad);
    }

    return hash;
}

void markdown_core_parser_feed(markdown_core_parser *parser, const char *buffer, size_t len) {
    S_parser_feed(parser, (const unsigned char *)buffer, len, false);
}

static void S_parser_feed(markdown_core_parser *parser, const unsigned char *buffer, size_t len, bool eof) {
    /* `buffer` may be NULL when `len` is zero — the public append takes such
     * a chunk, and it is a mutation like any other. Even a zero offset is
     * undefined on a null pointer, so the end is derived only when there are
     * bytes to end. Apple's UndefinedBehaviorSanitizer does not report this
     * one; the Linux toolchains CI runs do, which is where it was caught. */
    const unsigned char *end = len ? buffer + len : buffer;
    static const uint8_t repl[] = {239, 191, 189};

    /* A feed of no bytes changes nothing — not even the CR seam. Reading
     * *buffer to test the seam would be a read past the end (or of NULL,
     * which the public feed permits alongside a zero length), and clearing
     * the seam would split a CRLF that a later feed still completes. */
    if (len > 0) {
        parser->feed_started = true;
        if (parser->last_buffer_ended_with_cr && *buffer == '\n') {
            // skip NL if last buffer ended with CR ; see #117
            buffer++;
        }
        parser->last_buffer_ended_with_cr = false;
    }
    while (buffer < end) {
        const unsigned char *eol;
        markdown_core_bufsize chunk_len;
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

        chunk_len = (markdown_core_bufsize)(eol - buffer);
        if (process) {
            if (parser->linebuf.size > 0) {
                markdown_core_strbuf_put(&parser->linebuf, buffer, chunk_len);
                S_process_line(parser, parser->linebuf.ptr, parser->linebuf.size);
                markdown_core_strbuf_clear(&parser->linebuf);
            } else {
                S_process_line(parser, buffer, chunk_len);
            }
        } else {
            if (eol < end && *eol == '\0') {
                // omit NULL byte
                markdown_core_strbuf_put(&parser->linebuf, buffer, chunk_len);
                // add replacement character
                markdown_core_strbuf_put(&parser->linebuf, repl, 3);
            } else {
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

/* Removes an ATX heading's closing sequence from the line, reporting the
 * chopped `#` run through the out-parameters (length 0 when the line has no
 * closing sequence) so the caller can record the marker these bytes stop
 * existing anywhere else after this call. */
static void chop_trailing_hashtags(
    markdown_core_chunk *ch,
    markdown_core_bufsize *closer_start,
    markdown_core_bufsize *closer_length
) {
    markdown_core_bufsize n, orig_n;

    *closer_length = 0;
    markdown_core_chunk_rtrim(ch);
    orig_n = n = ch->len - 1;

    // if string ends in space followed by #s, remove these:
    while (n >= 0 && peek_at(ch, n) == '#') {
        n--;
    }

    // Check for a space before the final #s:
    if (n != orig_n && n >= 0 && S_is_space_or_tab(peek_at(ch, n))) {
        *closer_start = n + 1;
        *closer_length = orig_n - n;
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
static int S_scan_thematic_break(
    markdown_core_parser *parser,
    markdown_core_chunk *input,
    markdown_core_bufsize offset
) {
    markdown_core_bufsize i;
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
static void S_advance_offset(
    markdown_core_parser *parser,
    markdown_core_chunk *input,
    markdown_core_bufsize count,
    bool columns
) {
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

static bool parse_block_quote_prefix(
    markdown_core_parser *parser,
    markdown_core_chunk *input,
    markdown_core_node *container
) {
    bool res = false;
    markdown_core_bufsize matched = 0;

    matched = parser->indent <= 3 && peek_at(input, parser->first_nonspace) == '>';
    if (matched) {
        /* This line's `>` belongs to the quote's own region; the optional
         * following space is trivia and stays an implicit gap. */
        markdown_core_parser_capture_marker(
            parser,
            container,
            MARKDOWN_CORE_CONCRETE_BLOCK_QUOTE_MARKER,
            parser->first_nonspace,
            1
        );

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

bool markdown_core_parser_match_list_item_prefix(
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
        markdown_core_bufsize matched = 0;

        if (parser->indent <= 3 && (peek_at(input, parser->first_nonspace) == container->as.code.fence_char)) {
            matched = scan_close_code_fence(input, parser->first_nonspace);
        }

        if (matched >= container->as.code.fence_length) {
            // closing fence - and since we're at
            // the end of a line, we can stop processing it:
            *should_continue = false;
            container->as.code.fence_closed = true;
            markdown_core_parser_capture_marker(
                parser,
                container,
                MARKDOWN_CORE_CONCRETE_FENCE_CLOSE,
                parser->first_nonspace,
                matched
            );
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

/* `last_block_matches` answers with three outcomes, not two:
 *
 *   > 0  the line continues this block;
 *   = 0  the line does not match it — the block's prefix is absent, and a
 *        paragraph inside it may still continue lazily, as it may out of a
 *        block quote; and
 *   < 0  the block ends on this line, having consumed its own terminator.
 *
 * The third is what a fence is: the block is finished here, so it is finalized
 * on the spot and the line stops being processed — the same two steps
 * parse_code_block_prefix takes for a closing code fence. Finalizing at the
 * terminator rather than at the next line that fails to match is what stops
 * the following line from continuing a paragraph the fence already ended. */
static bool parse_extension_block(
    markdown_core_parser *parser,
    markdown_core_node *container,
    markdown_core_chunk *input,
    bool *should_continue
) {
    int matched = 0;

    if (container->extension->last_block_matches) {
        matched =
            container->extension->last_block_matches(container->extension, parser, input->data, input->len, container);
    }
    if (matched < 0) {
        *should_continue = false;
        /* Close from the innermost open block outwards. `finalize` closes one
         * block, and a container that ends here can still hold an open
         * paragraph; closing the container around it would leave that
         * paragraph open inside a closed parent and take its end position from
         * the wrong line. The core's own fenced construct needs no such loop
         * because a code block holds no open children. */
        while (parser->current != container) {
            parser->current = finalize(parser, parser->current);
        }
        container->flags |= (markdown_core_node_internal_flags)MARKDOWN_CORE_NODE__ENDS_ON_CURRENT_LINE;
        parser->current = finalize(parser, container);
        return false;
    }

    return matched != 0;
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
            if (!parse_block_quote_prefix(parser, input, container)) {
                goto done;
            }
            break;
        case MARKDOWN_CORE_NODE_LIST_ITEM:
            if (!markdown_core_parser_match_list_item_prefix(parser, input, container)) {
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
    }

    *all_matched = true;

done:
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
    markdown_core_bufsize matched = 0;
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
        indented = parser->indent >= CODE_INDENT;

        if (!indented && peek_at(input, parser->first_nonspace) == '>') {

            markdown_core_bufsize blockquote_startpos = parser->first_nonspace;

            S_advance_offset(parser, input, parser->first_nonspace + 1 - parser->offset, false);
            // optional following character
            if (S_is_space_or_tab(peek_at(input, parser->offset))) {
                S_advance_offset(parser, input, 1, true);
            }
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_BLOCK_QUOTE, blockquote_startpos + 1);
            if (!*container) {
                return;
            }
            markdown_core_parser_capture_marker(
                parser,
                *container,
                MARKDOWN_CORE_CONCRETE_BLOCK_QUOTE_MARKER,
                blockquote_startpos,
                1
            );

        } else if (!indented && (matched = scan_atx_heading_start(input, parser->first_nonspace))) {
            markdown_core_bufsize hashpos;
            int level = 0;
            markdown_core_bufsize heading_startpos = parser->first_nonspace;

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
            /* The opener is the `#` run alone — level bytes at the start the
             * scanner matched; the spacing `matched` also covers is trivia. */
            markdown_core_parser_capture_marker(
                parser,
                *container,
                MARKDOWN_CORE_CONCRETE_ATX_OPENER,
                heading_startpos,
                level
            );

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
            (*container)->as.code.info = markdown_core_chunk_literal("");
            /* The fence run, at its true length where fence_length clamps. */
            markdown_core_parser_capture_marker(
                parser,
                *container,
                MARKDOWN_CORE_CONCRETE_FENCE_OPEN,
                parser->first_nonspace,
                matched
            );
            S_advance_offset(parser, input, parser->first_nonspace + matched - parser->offset, false);
            {
                /* The rest of the fence line is the raw info spelling,
                 * whitespace-trimmed on the terms finalize trims the decoded
                 * scalar; its bytes ride the content buffer only until
                 * finalize drops them, so the record is captured here. */
                markdown_core_bufsize info_start = parser->offset;
                markdown_core_bufsize info_end = input->len;
                while (info_end > info_start && markdown_core_isspace(peek_at(input, info_end - 1))) {
                    info_end--;
                }
                while (info_start < info_end && markdown_core_isspace(peek_at(input, info_start))) {
                    info_start++;
                }
                if (info_end > info_start) {
                    markdown_core_parser_capture_marker(
                        parser,
                        *container,
                        MARKDOWN_CORE_CONCRETE_FENCE_INFO,
                        info_start,
                        info_end - info_start
                    );
                }
            }

        } else if (
            !indented && ((matched = scan_html_block_start(input, parser->first_nonspace)) ||
                          (cont_type != MARKDOWN_CORE_NODE_PARAGRAPH &&
                           (matched = scan_html_block_start_7(input, parser->first_nonspace))))
        ) {
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_HTML_BLOCK, parser->first_nonspace + 1);
            if (!*container) {
                return;
            }
            (*container)->as.html_block_type = matched;
            // note, we don't adjust parser->offset because the tag is part of the
            // text
        } else if (
            !indented && cont_type == MARKDOWN_CORE_NODE_PARAGRAPH &&
            (lev = scan_setext_heading_line(input, parser->first_nonspace))
        ) {
            // finalize paragraph, resolving reference links
            has_content = resolve_reference_link_definitions(parser, *container);

            if (has_content) {

                (*container)->type = (uint16_t)MARKDOWN_CORE_NODE_HEADING;
                (*container)->as.heading.level = lev;
                (*container)->as.heading.setext = true;
                {
                    /* The full underline run; the scanner reports only the
                     * level, so the run is measured here. Internal spaces
                     * cannot occur (the scanner rejects them) and trailing
                     * whitespace is trivia. */
                    markdown_core_bufsize underline_length = 1;
                    char underline_char = peek_at(input, parser->first_nonspace);
                    while (peek_at(input, parser->first_nonspace + underline_length) == underline_char) {
                        underline_length++;
                    }
                    markdown_core_parser_capture_marker(
                        parser,
                        *container,
                        MARKDOWN_CORE_CONCRETE_SETEXT_UNDERLINE,
                        parser->first_nonspace,
                        underline_length
                    );
                }
                S_advance_offset(parser, input, input->len - 1 - parser->offset, false);
            }
        } else if (
            !indented && !(cont_type == MARKDOWN_CORE_NODE_PARAGRAPH && !all_matched) &&
            (parser->thematic_break_kill_pos <= parser->first_nonspace) &&
            (matched = S_scan_thematic_break(parser, input, parser->first_nonspace))
        ) {
            // it's only now that we know the line is not part of a setext heading:
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_THEMATIC_BREAK, parser->first_nonspace + 1);
            if (!*container) {
                return;
            }
            {
                /* `matched` runs through the line end; the construct ends at
                 * its last marker byte (the composition is marker bytes and
                 * whitespace only, so scanning back to the marker character
                 * strips the EOL and any trailing whitespace in one rule). */
                markdown_core_bufsize break_end = parser->first_nonspace + matched;
                while (peek_at(input, break_end - 1) != peek_at(input, parser->first_nonspace)) {
                    break_end--;
                }
                markdown_core_parser_capture_marker(
                    parser,
                    *container,
                    MARKDOWN_CORE_CONCRETE_THEMATIC_BREAK,
                    parser->first_nonspace,
                    break_end - parser->first_nonspace
                );
            }
            S_advance_offset(parser, input, input->len - 1 - parser->offset, false);
        } else if (
            !indented && (parser->options & MARKDOWN_CORE_OPT_FOOTNOTES) && depth < MAX_LIST_DEPTH &&
            (matched = scan_footnote_definition(input, parser->first_nonspace))
        ) {
            markdown_core_chunk c = markdown_core_chunk_borrow(input, parser->first_nonspace + 2, matched - 2);

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
            *container = add_child(
                parser,
                *container,
                MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION,
                parser->first_nonspace + matched + 1
            );
            if (!*container) {
                markdown_core_chunk_free(parser->mem, &c);
                return;
            }
            (*container)->as.literal = c;

            (*container)->internal_offset = matched;

            /* `[^` + label + `]:` — `matched` also covers trailing spacing,
             * which is trivia; the label chunk measures the marker exactly. */
            markdown_core_parser_capture_marker(
                parser,
                *container,
                MARKDOWN_CORE_CONCRETE_FOOTNOTE_OPENER,
                parser->first_nonspace,
                c.len + 4
            );

            /* Registered here, at the moment the container opens, rather than
             * at finalize like a link reference definition: a footnote's label
             * is settled by the opening line alone, while a link definition is
             * only harvested once its whole paragraph has accumulated. The
             * definition is defined from this point on either way — the inline
             * phase does not start until every block is closed. */
            markdown_core_footnote_definition_create(parser->footnote_defs, &c);
        } else if (
            (!indented || cont_type == MARKDOWN_CORE_NODE_LIST) && parser->indent < 4 && depth < MAX_LIST_DEPTH &&
            (matched = parse_list_marker(
                 parser,
                 input,
                 parser->first_nonspace,
                 (*container)->type == MARKDOWN_CORE_NODE_PARAGRAPH,
                 &data
             ))
        ) {

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
                    parser->mem->free(parser->mem, data);
                    return;
                }

                memcpy(&((*container)->as.list), data, sizeof(*data));
            }

            // add the list item
            *container = add_child(parser, *container, MARKDOWN_CORE_NODE_LIST_ITEM, parser->first_nonspace + 1);
            if (!*container) {
                parser->mem->free(parser->mem, data);
                return;
            }
            memcpy(&((*container)->as.list), data, sizeof(*data));
            parser->mem->free(parser->mem, data);
            /* The bullet or ordinal-plus-delimiter, exactly as written —
             * `matched` is the marker alone. It belongs to the item's region;
             * the List groups items but owns no marker bytes of its own, and
             * the spacing after the marker is trivia the padding field
             * already abstracts. */
            markdown_core_parser_capture_marker(
                parser,
                *container,
                MARKDOWN_CORE_CONCRETE_LIST_MARKER,
                parser->first_nonspace,
                matched
            );
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
            (*container)->as.code.info = markdown_core_chunk_literal("");
        } else {
            markdown_core_llist *tmp;
            markdown_core_node *new_container = NULL;

            /* Attachment order is priority: the first extension to claim the
             * line wins, and a non-null return is that claim. An extension
             * that did not open a block returns null — including when it
             * leaves the container exactly as it found it. */
            for (tmp = parser->extensions; tmp; tmp = tmp->next) {
                markdown_core_extension *ext = (markdown_core_extension *)tmp->data;

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
                markdown_core_bufsize closer_start = 0;
                markdown_core_bufsize closer_length = 0;
                chop_trailing_hashtags(input, &closer_start, &closer_length);
                if (closer_length > 0) {
                    markdown_core_parser_capture_marker(
                        parser,
                        container,
                        MARKDOWN_CORE_CONCRETE_ATX_CLOSER,
                        closer_start,
                        closer_length
                    );
                }
            }
            S_advance_offset(parser, input, parser->first_nonspace - parser->offset, false);
            add_line(container, input, parser);
        } else {
            // create paragraph container for line
            container = add_child(parser, container, MARKDOWN_CORE_NODE_PARAGRAPH, parser->first_nonspace + 1);
            // A paragraph is a leaf: the marks of the one that just closed
            // can go, since no two are ever open at once.
            parser->line_mark_count = 0;
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
static void S_process_line(markdown_core_parser *parser, const unsigned char *buffer, markdown_core_bufsize bytes) {
    markdown_core_node *last_matched_container;
    bool all_matched = true;
    markdown_core_node *container;
    markdown_core_chunk input;

    if (parser->oom || parser->root == NULL) {
        return;
    }

    markdown_core_strbuf_clear(&parser->curline);

    /* The line's bytes, as authored. UTF-8 is ASSUMED AND NEVER VALIDATED
     * (incremental-canonical-ast.md 7.1): there is no scan here, nothing is
     * replaced, and a sequence that is not UTF-8 is opaque payload this engine
     * carries and never interprets. The pass that used to sit here rewrote
     * such a sequence as U+FFFD, which is a lossy parse — it produces not a
     * degraded document but a different one. */
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

    /* The line is structurally complete; a capture loss now becomes the
     * parse's failure before the next line runs (see capture_lost). */
    if (parser->capture_lost) {
        parser->oom = true;
    }

    markdown_core_strbuf_clear(&parser->curline);
}

/* Runs the block-local postprocess pipeline for one unit: text
 * consolidation, extension block postprocess hooks in attachment order,
 * and HTML-comment stripping.
 *
 * RETURNS THE NODE NOW AT THE UNIT'S POSITION, which is the unit itself
 * unless a postprocessor replaced it — and a replacement FREES what it
 * replaced (extensions/formula.c does, in two of its arms). The whole-tree
 * driver below does not need the answer, because it precomputes its
 * traversal successor; a caller that refines ONE unit and then does anything
 * else with it does, and handing it back is the difference between that
 * caller being correct and it holding a pointer to freed memory. */
static markdown_core_node *S_postprocess_unit(
    markdown_core_parser *parser,
    markdown_core_node *unit,
    bool owns_inlines
) {
    markdown_core_llist *extensions;

    if (owns_inlines && !markdown_core_node_consolidate_texts(unit)) {
        parser->oom = true;
    }

    if (owns_inlines) {
        markdown_core_node *node = unit;
        for (;;) {
            if (node->extension && node->extension->materialize_inline &&
                !node->extension->materialize_inline(node->extension, parser, node)) {
                parser->oom = true;
                break;
            }
            if (node->first_child) {
                node = node->first_child;
                continue;
            }
            while (node != unit && !node->next) {
                node = node->parent;
            }
            if (node == unit) {
                break;
            }
            node = node->next;
        }
    }

    for (extensions = parser->extensions; extensions; extensions = extensions->next) {
        markdown_core_extension *ext = (markdown_core_extension *)extensions->data;
        if (ext->postprocess_block) {
            markdown_core_node *processed = ext->postprocess_block(ext, parser, unit);
            /* The hook answers with the node now at this position, and the
             * contract forbids NULL. A hook that spliced the tree and then
             * answered NULL would be the one replacement a caller cannot
             * see — it would leave this walk, and anything the walk hands a
             * unit to, holding a pointer to what was just freed. No bundled
             * hook does it; tolerating it silently is what would make it
             * possible. */
            assert(processed != NULL);
            if (processed) {
                unit = processed;
                owns_inlines = contains_inlines(unit);
            }
        }
    }
    return unit;
}

/* Drives S_postprocess_unit over a bounded subtree in document order.
 * Inline-owning units are pipeline leaves: their inline subtrees are handled
 * by the unit pass itself, so traversal never descends into them. Successors
 * are computed before a unit runs because the pipeline may replace, free, or
 * flatten the unit node; a replacement splices itself into the tree, so no
 * caller has to be told about it.
 *
 * Deliberately does NOT stamp `subtree_hash`. Stamping was folded into this
 * walk once, and it worked and was cheaper (+2.6% against +11.2% on
 * multiple_duplicate_references) -- but it took three shapes to do it: a
 * pipeline leaf stamped as it was processed, a container stamped as its last
 * child completed, and the root stamped by the caller because this walk stops
 * below its boundary. Correctness then depended on those three agreeing about
 * who covers whom, which is the same "some nodes stamped, some not" split that
 * produced the unstamped-inlines bug in the first place. One trailing pass
 * over the settled tree is worth the difference. */
static void S_postprocess_subtree(
    markdown_core_parser *parser,
    markdown_core_node *boundary,
    markdown_core_node *first
) {
    markdown_core_node *node = first;

    while (node) {
        bool owns_inlines = contains_inlines(node);
        markdown_core_node *next = NULL;

        if (!owns_inlines && node->first_child) {
            next = node->first_child;
        } else {
            for (markdown_core_node *up = node; up != boundary; up = up->parent) {
                if (up->next) {
                    next = up->next;
                    break;
                }
            }
        }

        S_postprocess_unit(parser, node, owns_inlines);
        node = next;
    }
}

static void S_postprocess_blocks(markdown_core_parser *parser) {
    if (parser->root->first_child) {
        S_postprocess_subtree(parser, parser->root, parser->root->first_child);
    }
}

/* --- the warm tick: eligibility, settle, publish, retract ------------------ */

/* THE ELIGIBILITY PREDICATE. A publish can be retracted only when the close's
 * side effects stay inside what the undo record restores: the open blocks'
 * flags, end coordinates, content sizes and youngest children; the parser's
 * line counters, current block, CR seam, marks, held line and diagnostics
 * count; and whatever the close APPENDED past each open block's youngest
 * child. Everything a line can do to an open block beyond that — retype it
 * (setext underline, table delimiter row), free it (a definitions-only
 * paragraph, a formula promotion), drop or detach its content (definition
 * harvest, fence info, code literal), write a payload (list tightness) — or
 * do to the parser (grow a definition table) is outside the record, and the
 * predicate is exactly the guard that keeps a close inside it.
 *
 * It is a pure probe over bytes and the open spine's SHAPE, decided before
 * the first write of a tick: the spine must be the document alone or the
 * document over one open paragraph or heading, and every line in play — the
 * held one, each line the chunk begins, and (through its first byte) the
 * paragraph's accumulated content — must be one that can only continue or
 * open a paragraph, or be blank. Coarse on purpose: a byte the predicate is unsure
 * about belongs to the fallback, which is exactly as correct as this and
 * only slower. */

/* A byte that, at the start of a line, may open a block other than a
 * paragraph, close the open one into something else, or start a definition.
 * Each entry names the construct that puts it here. */
static bool warm_byte_opens(unsigned char c) {
    switch (c) {
    case ' ':
    case '\t': /* indentation: an indented code block, or a continuation
                  the block phase must judge */
    case 0x0b:
    case 0x0c: /* the table delimiter-row scanner accepts these as spacing
                  before its markers, and nothing else in the block phase
                  skips them — the one place a byte outside this set could
                  precede a marker */
    case '#':  /* ATX heading */
    case '>':  /* block quote */
    case '*':
    case '-':
    case '+': /* list item, thematic break, setext H2 underline */
    case '_': /* thematic break */
    case '=': /* setext H1 underline */
    case '~':
    case '`': /* fenced code */
    case '<': /* HTML block */
    case '|':
    case ':': /* table delimiter row; `:::` directive */
    case '[': /* link reference definition; footnote definition */
    case '$':
    case '\\': /* formula block: `$$`, `\\[` */
        return true;
    default:
        return c >= '0' && c <= '9'; /* ordered list marker */
    }
}

/* A byte that, anywhere, can make the refine REPLACE the unit it refines: a
 * paragraph that is nothing but a display formula is promoted to a formula
 * block, and the promotion frees the paragraph (extensions/formula.c). The
 * spine leaf is the one unit a retract must find where it left it. */
static bool warm_byte_replaces(unsigned char c) { return c == '$'; }

/* Whether every line a byte run begins is one the warm path may feed:
 * `at_line_start` says whether the first byte begins a line, and a line end
 * inside the run begins the next. `first_line` says the run begins the
 * document's very first line, whose byte-order mark the block phase skips
 * before it judges anything — so the predicate skips it too, or a mark
 * would hide the byte behind it. */
static bool warm_run_eligible(const unsigned char *bytes, size_t length, bool at_line_start, bool first_line) {
    size_t i = 0;
    if (first_line && at_line_start && length >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf) {
        i = 3;
    }
    for (; i < length; i++) {
        unsigned char c = bytes[i];
        if (warm_byte_replaces(c) || (at_line_start && warm_byte_opens(c))) {
            return false;
        }
        at_line_start = c == '\n' || c == '\r';
    }
    return true;
}

/* An open paragraph's accumulated content: its lines were already judged by
 * the block phase, so what matters is what its CLOSE would still do with it
 * — harvest definitions from a content that begins with `[`, or promote a
 * paragraph that is one display formula (which needs the formula at content
 * start, `$$` or `\\[`) — plus the replacing byte anywhere. */
static bool warm_content_eligible(const unsigned char *content, size_t size) {
    size_t i;
    if (size == 0) {
        return true;
    }
    if (content[0] == '[' || content[0] == '\\') {
        return false;
    }
    for (i = 0; i < size; i++) {
        if (warm_byte_replaces(content[i])) {
            return false;
        }
    }
    return true;
}

/* The open state a tick begins from, as the predicate sees it: the root, the
 * one open block under it (or NULL) with the flags it had while open, that
 * block's content, and the held partial line. Built either from the live
 * parser (a fresh build at EOF) or from the record of the previous publish
 * (a warm tick, before it has retracted anything). */
typedef struct warm_open_state {
    const markdown_core_node *root;
    const markdown_core_node *leaf;
    const unsigned char *content;
    size_t content_size;
    const unsigned char *held;
    size_t held_size;
    int line_number; /* 0 while the document's first line is still held */
    bool root_open;
    bool leaf_open;
    bool spine_shallow; /* nothing is open below the leaf */
} warm_open_state;

static bool warm_state_eligible(
    const markdown_core_parser *parser,
    const warm_open_state *state,
    const unsigned char *chunk,
    size_t length
) {
    if (parser->oom || parser->internal_error || !state->root || !state->root_open || !state->spine_shallow) {
        return false;
    }
    if (S_type(state->root) != MARKDOWN_CORE_NODE_DOCUMENT) {
        return false;
    }
    if (state->leaf) {
        /* A paragraph, subject to what its close would still do with its
         * content; or a heading, which nothing continues and whose close
         * does nothing but stamp its end — an ATX heading is the open leaf
         * after every "# Title\n", and a setext one after its underline. */
        if (!state->leaf_open) {
            return false;
        }
        switch (S_type(state->leaf)) {
        case MARKDOWN_CORE_NODE_PARAGRAPH:
            if (!warm_content_eligible(state->content, state->content_size)) {
                return false;
            }
            break;
        case MARKDOWN_CORE_NODE_HEADING:
            break;
        default:
            return false;
        }
    }
    if (!warm_run_eligible(state->held, state->held_size, true, state->line_number == 0)) {
        return false;
    }
    /* The chunk's first byte begins a line only when nothing is held. */
    return length == 0 || warm_run_eligible(chunk, length, state->held_size == 0, state->line_number == 0);
}

bool markdown_core_parser_warm_eligible_at_eof(const markdown_core_parser *parser) {
    warm_open_state state;

    memset(&state, 0, sizeof(state));
    state.root = parser->root;
    state.root_open = parser->root && (parser->root->flags & MARKDOWN_CORE_NODE__OPEN);
    state.held = (const unsigned char *)parser->linebuf.ptr;
    state.held_size = parser->linebuf.size;
    state.line_number = parser->line_number;
    /* THE OPEN CHAIN is root down to parser->current along youngest children
     * — the block phase's own definition, and the one to use: an OPEN flag
     * alone does not say it, since a block an extension made without ever
     * routing it through the block phase keeps the flag for life (a table
     * cell). So the leaf is the root's youngest child when the root is not
     * itself the current block, and the chain is shallow exactly when that
     * leaf is the current block. */
    state.spine_shallow = true;
    if (parser->root && parser->current != parser->root) {
        const markdown_core_node *leaf = parser->root->last_child;
        state.leaf = leaf;
        state.leaf_open = leaf && (leaf->flags & MARKDOWN_CORE_NODE__OPEN);
        state.content = leaf ? (const unsigned char *)leaf->content.ptr : NULL;
        state.content_size = leaf ? leaf->content.size : 0;
        state.spine_shallow = leaf == parser->current;
    }
    return warm_state_eligible(parser, &state, NULL, 0);
}

bool markdown_core_parser_warm_eligible(
    const markdown_core_parser *parser,
    const markdown_core_warm_undo *published,
    const unsigned char *chunk,
    size_t length
) {
    warm_open_state state;

    if (!published || published->final || published->retracted || published->spine_count == 0) {
        return false;
    }
    /* Read off the record, not the tree: the close changed the spine, but it
     * saved what it changed — every entry was open, and the content bytes
     * below a saved size are exactly the ones the block had, since appends
     * only ever add past them. */
    memset(&state, 0, sizeof(state));
    state.root = published->spine[0].node;
    state.root_open = true;
    state.spine_shallow = published->spine_count <= 2;
    if (published->spine_count >= 2) {
        const markdown_core_warm_open_block *entry = &published->spine[1];
        state.leaf = entry->node;
        state.leaf_open = true;
        state.content = (const unsigned char *)entry->node->content.ptr;
        state.content_size = entry->content_size;
    }
    state.held = published->held;
    state.held_size = published->held_size;
    state.line_number = published->line_number;
    return warm_state_eligible(parser, &state, chunk, length);
}

/* --- settle: refine what a step closed ------------------------------------ */

/* Whether `node` is on the open chain — the current block or one of its
 * ancestors. That, and not the OPEN flag, is what "still growing" means:
 * the flag stays set for life on a block an extension made and never routed
 * through the block phase (a table cell), while a settled block is simply
 * one the chain has left behind. */
static bool warm_on_chain(const markdown_core_parser *parser, const markdown_core_node *node) {
    const markdown_core_node *cursor;
    for (cursor = parser->current; cursor; cursor = cursor->parent) {
        if (cursor == node) {
            return true;
        }
    }
    return false;
}

/* Refines the settled units of one subtree, children before their container
 * (close order), and answers with the node now at the subtree's position —
 * a refine may replace a leaf and free what it replaced. `on_chain` says
 * whether this node is still open in the block phase's sense; such a node
 * is descended into and left alone, because the step that closes it will
 * refine it. Its youngest child is on the chain too unless the node is the
 * current block itself, below which nothing is open. */
static markdown_core_node *warm_settle_subtree(markdown_core_parser *parser, markdown_core_node *node, bool on_chain) {
    if (!contains_inlines(node)) {
        markdown_core_node *child = node->first_child;
        while (child) {
            bool child_on_chain = on_chain && node != parser->current && child == node->last_child;
            markdown_core_node *survivor = warm_settle_subtree(parser, child, child_on_chain);
            child = survivor->next;
        }
    }
    if (on_chain) {
        return node;
    }
    return markdown_core_parser_warm_refine_settled(parser, node);
}

/* The region a snapshot describes: for each saved open block, deepest first,
 * every child the step appended past its saved youngest child, then the
 * block itself if the step closed it. Answers whether every spine block is
 * still the object the snapshot named — a spine block replaced by its own
 * refine (a paragraph promoted to a formula block) is one the record can no
 * longer put back, and the entry is repointed at the survivor so nothing
 * dangles. */
static bool warm_refine_region(markdown_core_parser *parser, markdown_core_warm_open_block *spine, size_t count) {
    bool intact = true;
    size_t i = count;
    while (i-- > 0) {
        markdown_core_warm_open_block *entry = &spine[i];
        markdown_core_node *node = entry->node;
        bool on_chain = warm_on_chain(parser, node);
        markdown_core_node *child = entry->last_child ? entry->last_child->next : node->first_child;
        while (child) {
            bool child_on_chain = on_chain && node != parser->current && child == node->last_child;
            markdown_core_node *survivor = warm_settle_subtree(parser, child, child_on_chain);
            child = survivor->next;
        }
        /* The document root is never refined: refine_blocks starts below it,
         * and every hook expects a block. */
        if (i > 0 && !on_chain) {
            markdown_core_node *survivor = markdown_core_parser_warm_refine_settled(parser, node);
            if (survivor != node) {
                /* The survivor sits where the block sat, so its parent's
                 * saved youngest child — this very block — is repointed too,
                 * and the parent's run still begins after it. */
                entry->node = survivor;
                if (spine[i - 1].last_child == node) {
                    spine[i - 1].last_child = survivor;
                }
                intact = false;
            }
        }
    }
    return intact;
}

bool markdown_core_parser_warm_settle(markdown_core_parser *parser, markdown_core_warm_undo *before) {
    if (!parser->root) {
        return false;
    }
    if (before) {
        return warm_refine_region(parser, before->spine, before->spine_count);
    }
    /* No record: a fresh parser, whose whole settled part is unrefined. */
    {
        markdown_core_node *root = parser->root;
        markdown_core_node *child = root->first_child;
        while (child) {
            bool child_on_chain = root != parser->current && child == root->last_child;
            markdown_core_node *survivor = warm_settle_subtree(parser, child, child_on_chain);
            child = survivor->next;
        }
    }
    return true;
}

/* --- publish and retract -------------------------------------------------- */

static void warm_free_run(markdown_core_node *run) {
    while (run) {
        markdown_core_node *next = run->next;
        /* Detached from a parent that no longer lists it: unlinking through
         * a stale parent pointer would edit that parent's list. */
        run->parent = NULL;
        run->prev = NULL;
        run->next = NULL;
        markdown_core_node_free(run);
        run = next;
    }
}

void markdown_core_parser_warm_undo_free(markdown_core_warm_undo *undo) {
    markdown_core_mem *mem;
    size_t i;
    if (!undo) {
        return;
    }
    mem = undo->mem;
    for (i = 0; i < undo->spine_count; i++) {
        warm_free_run(undo->spine[i].retired);
    }
    mem->free(mem, undo->spine);
    mem->free(mem, undo->marks);
    mem->free(mem, undo->held);
    mem->free(mem, undo);
}

/* The open chain, root down, plus everything on the parser that outlives a
 * line. Taken before the close so the close has something to be undone to. */
static bool warm_undo_save(markdown_core_parser *parser, markdown_core_warm_undo *undo) {
    markdown_core_node *node;
    size_t depth = 0;

    /* The open chain: root down to the current block along youngest
     * children — not "while the OPEN flag is set", which would run past the
     * current block into an extension-made child that keeps the flag for
     * life (see warm_on_chain). */
    for (node = parser->current; node; node = node->parent) {
        depth++;
    }
    if (depth) {
        size_t i = 0;
        undo->spine = (markdown_core_warm_open_block *)undo->mem->calloc(undo->mem, depth, sizeof(*undo->spine));
        if (!undo->spine) {
            return false;
        }
        for (node = parser->root; i < depth; node = node->last_child) {
            markdown_core_warm_open_block *entry = &undo->spine[i++];
            entry->node = node;
            entry->last_child = node->last_child;
            entry->type = node->type;
            entry->flags = node->flags;
            entry->end_line = node->end_line;
            entry->end_column = node->end_column;
            entry->content_size = node->content.size;
        }
        undo->spine_count = depth;
    }
    if (parser->line_mark_count) {
        undo->marks = (struct markdown_core_line_mark *)
                          undo->mem->calloc(undo->mem, parser->line_mark_count, sizeof(*undo->marks));
        if (!undo->marks) {
            return false;
        }
        memcpy(undo->marks, parser->line_marks, parser->line_mark_count * sizeof(*undo->marks));
    }
    undo->mark_count = parser->line_mark_count;
    if (parser->linebuf.size) {
        undo->held = (unsigned char *)undo->mem->calloc(undo->mem, parser->linebuf.size, 1);
        if (!undo->held) {
            return false;
        }
        memcpy(undo->held, parser->linebuf.ptr, parser->linebuf.size);
    }
    undo->held_size = parser->linebuf.size;
    undo->line_number = parser->line_number;
    undo->last_line_length = parser->last_line_length;
    undo->current = parser->current;
    undo->last_buffer_ended_with_cr = parser->last_buffer_ended_with_cr;
    undo->diagnostic_count = parser->diagnostic_count;
    return true;
}

markdown_core_warm_undo *markdown_core_parser_warm_publish(markdown_core_parser *parser) {
    markdown_core_warm_undo *undo;

    if (!parser->root) {
        return NULL;
    }
    undo = (markdown_core_warm_undo *)parser->mem->calloc(parser->mem, 1, sizeof(*undo));
    if (!undo) {
        return NULL;
    }
    undo->mem = parser->mem;
    if (!warm_undo_save(parser, undo)) {
        markdown_core_parser_warm_undo_free(undo);
        return NULL;
    }
    markdown_core_parser_finalize_blocks(parser);
    /* A spine block the held line RETYPED (a setext underline, a table
     * delimiter row) is one the record cannot put back: the predicate keeps
     * such lines out, and this is what makes a predicate that missed one
     * fail loudly — a projection that can be read, not reopened — instead of
     * reopening a heading as a paragraph. */
    {
        size_t i;
        for (i = 0; i < undo->spine_count; i++) {
            if (undo->spine[i].node->type != undo->spine[i].type) {
                undo->final = true;
            }
        }
    }
    /* Only what the close closed: the spine, and whatever the held line put
     * under it. Units the feed closed are the caller's to settle, and were. */
    if (!warm_refine_region(parser, undo->spine, undo->spine_count)) {
        undo->final = true;
    }
    return undo;
}

/* Makes every node of a subtree own its bytes: the retired frontier outlives
 * the buffer its literals borrow (the open leaf's content, which the next
 * feed appends to and may move), and the identity handover reads those
 * literals to say whether a paired node changed. */
static bool warm_own_subtree(markdown_core_node *root) {
    markdown_core_node *node = root;
    for (;;) {
        if (!markdown_core_node_own_chunks(node)) {
            return false;
        }
        if (node->first_child) {
            node = node->first_child;
            continue;
        }
        while (node != root && !node->next) {
            node = node->parent;
        }
        if (node == root) {
            return true;
        }
        node = node->next;
    }
}

bool markdown_core_parser_warm_retract(markdown_core_parser *parser, markdown_core_warm_undo *undo) {
    size_t i;

    if (!undo || undo->retracted || undo->final) {
        return false;
    }
    /* Before anything moves: what is about to be retired must own its bytes,
     * and if it cannot, nothing has changed and the record is still the
     * published one. */
    for (i = 0; i < undo->spine_count; i++) {
        markdown_core_warm_open_block *entry = &undo->spine[i];
        markdown_core_node *child = entry->last_child ? entry->last_child->next : entry->node->first_child;
        for (; child; child = child->next) {
            if (!warm_own_subtree(child)) {
                return false;
            }
        }
    }
    /* Deepest spine entry first: a block the close minted hangs under the one
     * that was open when it was minted, so its owner is put back after it is
     * detached. What the close appended past each block's saved youngest
     * child is not freed but RETIRED — kept, detached, so the next publish
     * can hand its identities to what takes its place — and the block's
     * inline records go, because a refine assigns them once. Nothing here
     * reads a byte those retired nodes borrow. */
    i = undo->spine_count;
    while (i-- > 0) {
        markdown_core_warm_open_block *entry = &undo->spine[i];
        markdown_core_node *node = entry->node;
        markdown_core_node *run = entry->last_child ? entry->last_child->next : node->first_child;
        if (entry->last_child) {
            entry->last_child->next = NULL;
        } else {
            node->first_child = NULL;
        }
        node->last_child = entry->last_child;
        if (run) {
            run->prev = NULL;
        }
        entry->retired = run;
        if (node->inline_concrete) {
            markdown_core_inline_concrete_records_free(markdown_core_node_mem(node), node->inline_concrete);
            node->inline_concrete = NULL;
        }
        node->flags = entry->flags;
        node->end_line = entry->end_line;
        node->end_column = entry->end_column;
        markdown_core_strbuf_truncate(&node->content, entry->content_size);
    }
    parser->line_number = undo->line_number;
    parser->last_line_length = undo->last_line_length;
    parser->current = undo->current;
    parser->last_buffer_ended_with_cr = undo->last_buffer_ended_with_cr;
    parser->line_mark_count = undo->mark_count;
    if (undo->mark_count) {
        memcpy(parser->line_marks, undo->marks, undo->mark_count * sizeof(*undo->marks));
    }
    parser->diagnostic_count = undo->diagnostic_count;
    markdown_core_strbuf_clear(&parser->linebuf);
    if (undo->held_size) {
        markdown_core_strbuf_put(&parser->linebuf, undo->held, undo->held_size);
    }
    undo->retracted = true;
    return true;
}

markdown_core_node *markdown_core_parser_warm_refine_settled(markdown_core_parser *parser, markdown_core_node *unit) {
    bool owns_inlines = contains_inlines(unit);

    if (owns_inlines) {
        S_parse_node_inlines(parser, unit, parser->refmap, parser->options);
    }
    /* This unit alone: its children settled through their own calls. The
     * survivor comes back because a postprocessor may have replaced this
     * node and freed what it replaced — a caller that keeps the pointer it
     * passed in keeps a pointer to freed memory. */
    return S_postprocess_unit(parser, unit, owns_inlines);
}

bool markdown_core_parser_warm_refine(markdown_core_parser *parser) {
    if (parser->root == NULL) {
        return false;
    }

    process_inlines(parser, parser->refmap, parser->options);
    S_postprocess_blocks(parser);

    /* The same allocation-loss convergence refine_blocks performs, minus its
     * verdict: a warm refine reports the loss and leaves the tree alone,
     * because the caller still owns a parser it may want to put back. */
    if ((parser->refmap && parser->refmap->oom) || (parser->footnote_defs && parser->footnote_defs->oom)) {
        parser->oom = true;
    }
    if (parser->capture_lost) {
        parser->oom = true;
    }
    return !(parser->oom || parser->internal_error);
}

markdown_core_node *markdown_core_parser_refine_blocks(markdown_core_parser *parser) {
    markdown_core_node *res;

    if (parser->root == NULL) {
        return NULL;
    }

    process_inlines(parser, parser->refmap, parser->options);

    S_postprocess_blocks(parser);

    /* All allocation-loss routes converge here: block/inline structures set
     * parser->oom directly, definition maps carry their own sticky flag, and
     * capture losses after the last line boundary — the finalize-time
     * reference-definition harvest has no later S_process_line to fold them
     * into oom — surface through capture_lost. */
    if ((parser->refmap && parser->refmap->oom) || (parser->footnote_defs && parser->footnote_defs->oom)) {
        parser->oom = true;
    }
    if (parser->capture_lost) {
        parser->oom = true;
    }
    if (parser->oom || parser->internal_error) {
        markdown_core_node_free(parser->root);
        parser->root = NULL;
        return NULL;
    }

    /* WHERE THE TREE IS FINGERPRINTED: one pass, over the settled tree, after
     * every pass that could still move a node.
     *
     * Stamping used to ride along on process_inlines' walk, on the argument
     * that the walk was already running. That walk cannot do the job, twice
     * over. It never sees an inline node at all: it creates a unit's inline
     * children on that unit's ENTER, and the iterator settles its successor
     * before the caller gets the event, so the children appear behind the walk
     * and are never entered. Every inline node kept hash 0, so a unit's hash
     * mixed nothing but zeros and collapsed to a function of kind and child
     * count -- precisely the discrimination diff.c's prefix sweep needs it to
     * have. And it ran before S_postprocess_blocks, which merges adjacent
     * text, splits text for autolinks and replaces whole units, so whatever it
     * did compute described a tree that no longer existed by the time the diff
     * read it. One cause behind both: it stamped too early.
     *
     * It costs a traversal -- ~6% of parse time on the throughput corpus and
     * ~11% on multiple_duplicate_references, which is nearly all blocks and so
     * pays for every node. Folding it into S_postprocess_subtree instead is
     * measurably cheaper (+2.6%) and was rejected: it takes three stamping
     * shapes to cover the tree that way, and a rule about which shape owns
     * which node is the thing that failed here already. Placed after the
     * failure check, so a tree that is about to be freed is never walked. */
    markdown_core_node_stamp_tree(parser->root);

    res = parser->root;
    parser->root = NULL;

    return res;
}

markdown_core_node *markdown_core_parser_finish(markdown_core_parser *parser) {
    markdown_core_node *res;

    /* Parser was already finished once */
    if (parser->root == NULL) {
        return NULL;
    }

    markdown_core_parser_finalize_blocks(parser);

    markdown_core_strbuf_free(&parser->curline);
    markdown_core_strbuf_free(&parser->linebuf);

#if MARKDOWN_CORE_DEBUG_NODES
    if (markdown_core_node_check(parser->root, stderr)) {
        abort();
    }
#endif

    res = markdown_core_parser_refine_blocks(parser);

    markdown_core_parser_reset(parser);

    return res;
}

int markdown_core_parser_get_line_number(markdown_core_parser *parser) { return parser->line_number; }

markdown_core_bufsize markdown_core_parser_get_offset(markdown_core_parser *parser) { return parser->offset; }

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
