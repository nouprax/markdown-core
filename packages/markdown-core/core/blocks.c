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

/* Appends and reports failure directly instead of relying on llist_append's
 * silent-drop behavior. */
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

    if (!parser || !extension || !parser->inline_config || parser->total_size != 0) {
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

/* Like markdown_core_parser_reset, but keeps the allocations one parse hands
 * back intact for the next: the line buffers' capacity, the definition maps
 * when the caller left them attached (a consumed map is replaced by a fresh
 * one), and the extension attachments. A kept map must be empty — the caller
 * either never inserted into it or took it away. Failure poisons the parser
 * exactly like a failed reset. */
void markdown_core_parser_renew(markdown_core_parser *parser) {
    markdown_core_llist *saved_exts = parser->extensions;
    markdown_core_inline_config *saved_inline_config = parser->inline_config;
    markdown_core_delimiter_engine saved_inline_delimiters = parser->inline_delimiters;
    int saved_options = parser->options;
    markdown_core_mem *saved_mem = parser->mem;
    markdown_core_map *saved_refmap = parser->refmap;
    markdown_core_map *saved_footnote_defs = parser->footnote_defs;
    markdown_core_strbuf saved_curline = parser->curline;
    markdown_core_strbuf saved_linebuf = parser->linebuf;

    if (parser->root) {
        markdown_core_node_free(parser->root);
    }

    if (parser->line_marks) {
        parser->mem->free(parser->mem, parser->line_marks);
    }

    memset(parser, 0, sizeof(markdown_core_parser));
    parser->mem = saved_mem;

    parser->curline = saved_curline;
    parser->linebuf = saved_linebuf;
    markdown_core_strbuf_clear(&parser->curline);
    markdown_core_strbuf_clear(&parser->linebuf);

    markdown_core_node *document = make_document(parser->mem);

    parser->refmap = saved_refmap ? saved_refmap : markdown_core_reference_map_new(parser->mem);
    parser->footnote_defs =
        saved_footnote_defs ? saved_footnote_defs : markdown_core_footnote_definition_map_new(parser->mem);
    parser->root = document;
    parser->current = document;

    parser->extensions = saved_exts;
    parser->inline_config = saved_inline_config;
    parser->inline_delimiters = saved_inline_delimiters;
    parser->options = saved_options;

    if (!parser->root || !S_definition_maps_ready(parser) || parser->curline.oom || parser->linebuf.oom) {
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
 * region-relative — a suffix reflow moves the node and every record moves
 * with it, untouched, exactly like the sealed parent-relative node lines.
 * Capture runs only while the node's parse is live, so start_line is still
 * the absolute opening line, never the sealed delta.
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

/* The document-child anchor a definition retracts with: the direct document
 * child containing `b`. Sessions reparse whole document children at a
 * time, so a
 * definition stamped with its anchor is re-harvested exactly when its bytes
 * are reparsed. Anchors are stamped as node pointers here and rewritten to
 * node ids once the session's adoption walk has assigned them; the map is
 * parse-local everywhere else, so the stamp is inert outside sessions. */
static markdown_core_node *S_definition_anchor(markdown_core_parser *parser, markdown_core_node *b) {
    markdown_core_node *anchor = b;
    while (anchor->parent && anchor->parent != parser->root) {
        anchor = anchor->parent;
    }
    return anchor;
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
    *column = mark->column + (int)(offset - mark->content_offset);
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
        node->as.definition.title = markdown_core_chunk_borrow(&harvested->title, 0, harvested->title.len);
        if (!markdown_core_chunk_to_cstr(parser->mem, &node->as.definition.url) ||
            !markdown_core_chunk_to_cstr(parser->mem, &node->as.definition.title)) {
            parser->oom = true;
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
    /* Only the first definition inherits the paragraph's restart-anchor bits.
     * Those bits say "reparsing from this line reproduces everything after
     * it", and that held for the line the paragraph opened on — which is the
     * line the first definition starts on. A later definition starts on a
     * line that arrived while the paragraph was already open, and such a line
     * can be the continuation of the definition above it: `[foo]:` on one
     * line, its indented url on the next. Restarting there reads the
     * continuation as an indented code block.
     *
     * Before definitions had nodes, a definition-only paragraph vanished and
     * the session kept a sentinel clean entry to stand in for the anchor the
     * tree no longer had; the node is now that anchor. */
    if (start == 0) {
        node->flags |= b->flags & MARKDOWN_CORE_NODE__CLEAN_ANCHOR;
    }
    if (!markdown_core_node_insert_before(b, node)) {
        /* Containment is proved by construction here, so a refusal means the
         * insert lost an allocation. Freeing the node keeps the failure from
         * also leaking it. */
        parser->oom = true;
        markdown_core_node_free(node);
        return;
    }

    /* A definition now has a node of its own, so its map entry is owned by
     * that node's document child rather than by the paragraph the bytes were
     * harvested from. This is what the owner always meant — "retract this
     * entry exactly when its bytes are reparsed" — and it was approximated by
     * the harvesting paragraph only because the definition had nowhere else
     * to live. A definition-only paragraph vanishing can no longer orphan an
     * entry, since the node it now belongs to is the thing that survives. */
    if (harvested) {
        markdown_core_reference *entry = (markdown_core_reference *)harvested;
        entry->entry.owner = (uint64_t)(uintptr_t)S_definition_anchor(parser, node);
        entry->entry.definition_node = (uint64_t)(uintptr_t)node;
        entry->entry.start_line = node->start_line;
    }
}

// returns true if content remains after link defs are resolved.
static bool resolve_reference_link_definitions(markdown_core_parser *parser, markdown_core_node *b) {
    markdown_core_bufsize pos;
    markdown_core_strbuf *node_content = &b->content;
    markdown_core_chunk chunk = {node_content->ptr, node_content->size, 0};
    markdown_core_bufsize consumed = 0;
    size_t mark_cursor = 0;
    if (parser->refmap) {
        parser->refmap->pending_owner = (uint64_t)(uintptr_t)S_definition_anchor(parser, b);
        parser->refmap->pending_line = b->start_line;
    }
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
     * and neither parity oracle compares positions. The session's restart
     * reparses from the surviving line and got this right, which is how the
     * equivalence gate found it. */
    if (consumed) {
        S_content_position(parser, b, consumed, &mark_cursor, &b->start_line, &b->start_column);
        /* The restart-anchor bits asserted that reparsing from the line the
         * paragraph opened on reproduces it. That line now belongs to the
         * first definition, which took the bits; the paragraph's new start is
         * a line that arrived while it was already open, and reparsing from
         * there does not reproduce it — an indented continuation reads as a
         * code block instead. Moving the start without dropping the claim is
         * what made the session restart mid-definition. */
        b->flags &= (markdown_core_node_internal_flags)~MARKDOWN_CORE_NODE__CLEAN_ANCHOR;
    }
    markdown_core_strbuf_drop(node_content, (node_content->size - chunk.len));
    return !is_blank(&b->content, 0);
}

/* When a definition-only paragraph is about to be freed, its own pointer can
 * no longer anchor the definitions it produced. Re-anchor them to the
 * previous surviving sibling (already finalized, so never itself about to
 * vanish), or to owner 0: the region before the first document child. Only a
 * top-level paragraph anchors to itself, so nested vanishing paragraphs
 * never reach this. Freshly added entries sit at the head of the live chain,
 * so the walk stops at the first entry with another owner. */
static void S_reanchor_vanishing_definitions(markdown_core_parser *parser, markdown_core_node *b) {
    uint64_t vanishing = (uint64_t)(uintptr_t)b;
    uint64_t replacement = (uint64_t)(uintptr_t)b->prev;
    markdown_core_map_entry *entry;

    if (!parser->refmap) {
        return;
    }
    for (entry = parser->refmap->refs; entry && entry->owner == vanishing; entry = entry->next) {
        entry->owner = replacement;
        entry->from_vanished_clean = (b->flags & MARKDOWN_CORE_NODE__CLEAN_START) != 0;
    }
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
            if (b->parent == parser->root) {
                S_reanchor_vanishing_definitions(parser, b);
            }
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

    /* Direct document children born on a clean line are incremental restart
     * points; every other child attaches under (or next to) a block that was
     * open when the line began and stays fused to it. A line that seals a
     * definitions-only chain counts as clean, with the qualifier marking the
     * anchor as conditional on the line's shape. */
    if (parent == parser->root && parser->line_began_clean) {
        child->flags |= MARKDOWN_CORE_NODE__CLEAN_START;
        if (parser->line_defs_only) {
            child->flags |= MARKDOWN_CORE_NODE__CLEAN_START_SEALING;
        }
    }

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

void markdown_core_parser_manage_extensions_special_characters(markdown_core_parser *parser, int add) {
    /*
     * Compatibility no-op for callers built against the former mutable-table
     * SPI. The compiled parser-local config is always active.
     */
}

/* The node a lookup observed during `node`'s inline parse is attributed to
 * the outermost inline-owning node of the parent chain (the unit the
 * per-block postprocess pipeline visits). */
static markdown_core_node *S_lookup_attribution(markdown_core_node *node) {
    markdown_core_node *unit = node;
    markdown_core_node *up;
    for (up = node->parent; up; up = up->parent) {
        if (contains_inlines(up)) {
            unit = up;
        }
    }
    return unit;
}

static void S_parse_node_inlines(
    markdown_core_parser *parser,
    markdown_core_node *cur,
    markdown_core_map *refmap,
    int options
) {
    /* Both definition maps attribute to the same unit: a lookup in either is
     * a dependency of the block whose inlines are being parsed. One test arms
     * both, because a watcher attaches its sink to both maps or to neither
     * (session.c, watch_definition_lookups) — so a watched reference map is
     * proof that the footnote map is there. */
    if (refmap && refmap->lookup_sink) {
        markdown_core_node *unit = S_lookup_attribution(cur);
        refmap->lookup_unit = unit;
        parser->footnote_defs->lookup_unit = unit;
    }
    /* A session-staged leaf may carry an inline seam in user_data (offset+1):
     * bytes before it are an inert, already-materialized prefix whose nodes
     * the commit transplants later, so inline parsing starts at the seam.
     * One-shot parses never set user_data. */
    if (cur->user_data) {
        markdown_core_parse_inlines_from(
            parser,
            cur,
            refmap,
            options,
            (markdown_core_bufsize)((uintptr_t)cur->user_data - 1)
        );
        return;
    }
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

    // Limit total size of extra content created from reference links to
    // document size to avoid superlinear growth. Always allow 100KB.
    if (parser->refmap) {
        if (parser->total_size > 100000) {
            parser->refmap->max_ref_size = parser->total_size;
        } else {
            parser->refmap->max_ref_size = 100000;
        }
    }
}

markdown_core_node *markdown_core_node_parse_document(const char *buffer, size_t len, int options) {
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

static void S_parser_feed(markdown_core_parser *parser, const unsigned char *buffer, size_t len, bool eof) {
    const unsigned char *end = buffer + len;
    static const uint8_t repl[] = {239, 191, 189};

    if (len > UINT_MAX - parser->total_size) {
        parser->total_size = UINT_MAX;
    } else {
        parser->total_size += len;
    }

    if (parser->last_buffer_ended_with_cr && *buffer == '\n') {
        // skip NL if last buffer ended with CR ; see #117
        buffer++;
    }
    parser->last_buffer_ended_with_cr = false;
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

/* True when the document's open chain is nonempty and consists solely of
 * footnote definitions: every inner block has already been closed (a blank
 * line finalizes a definition's trailing paragraph while the definition
 * itself stays open). Such a chain continues only on a blank or indented
 * line and holds no open paragraph a lazy continuation could ride, so a
 * line that fails every prefix closes the whole chain before anything above
 * can capture it. */
static bool S_open_chain_defs_only(markdown_core_node *root) {
    markdown_core_node *node = root->last_child;
    if (!node || !(node->flags & MARKDOWN_CORE_NODE__OPEN)) {
        return false;
    }
    while (node && (node->flags & MARKDOWN_CORE_NODE__OPEN)) {
        if (S_type(node) != MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION) {
            return false;
        }
        node = node->last_child;
    }
    return true;
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
            markdown_core_footnote_definition_create(
                parser->footnote_defs,
                &c,
                (uint64_t)(uintptr_t)S_definition_anchor(parser, *container),
                (*container)->start_line,
                (uint64_t)(uintptr_t)*container
            );
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
    parser->line_began_clean = !S_last_child_is_open(parser->root);
    parser->line_defs_only = !parser->line_began_clean && S_open_chain_defs_only(parser->root);

    last_matched_container = check_open_blocks(parser, &input, &all_matched);

    if (!last_matched_container) {
        goto finished;
    }

    /* A line that fails every prefix of a definitions-only chain seals it:
     * the definitions close during this line, before its own content
     * attaches, so direct document children restart as if the line had
     * begun clean. The anchor is conditional on the line's sealing shape,
     * which the qualifying flag records for restart planning. */
    if (last_matched_container != parser->root) {
        parser->line_defs_only = false;
    } else if (parser->line_defs_only) {
        parser->line_began_clean = true;
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
 * and HTML-comment stripping. The caller precomputes its traversal successor
 * because a postprocessor may replace the unit. Returns the surviving node. */
static markdown_core_node *S_postprocess_unit(
    markdown_core_parser *parser,
    markdown_core_node *unit,
    bool owns_inlines
) {
    markdown_core_llist *extensions;
    markdown_core_node_internal_flags clean_start = unit->flags & MARKDOWN_CORE_NODE__CLEAN_ANCHOR;

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
            if (processed) {
                /* A replacement unit (formula promotion) still starts at the
                 * replaced unit's first line, so it inherits the incremental
                 * restart mark. */
                unit = processed;
                unit->flags |= clean_start;
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
 * flatten the unit node. Returns the node occupying `first`'s position after
 * processing; callers use this when the bounded root itself may be replaced. */
static markdown_core_node *S_postprocess_subtree(
    markdown_core_parser *parser,
    markdown_core_node *boundary,
    markdown_core_node *first
) {
    markdown_core_node *result = first;
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

        {
            markdown_core_node *processed = S_postprocess_unit(parser, node, owns_inlines);
            if (node == first) {
                result = processed;
            }
        }
        node = next;
    }
    return result;
}

static void S_postprocess_blocks(markdown_core_parser *parser) {
    if (parser->root->first_child) {
        S_postprocess_subtree(parser, parser->root, parser->root->first_child);
    }
}

markdown_core_node *markdown_core_parser_refine_unit(
    markdown_core_parser *parser,
    markdown_core_map *refmap,
    markdown_core_node *unit
) {
    markdown_core_iter *iter = markdown_core_iter_new(unit);
    markdown_core_node *cur;
    markdown_core_event_type ev_type;

    if (!iter) {
        parser->oom = true;
        return unit;
    }
    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        cur = markdown_core_iter_get_node(iter);
        if (ev_type == MARKDOWN_CORE_EVENT_ENTER && contains_inlines(cur)) {
            S_parse_node_inlines(parser, cur, refmap, parser->options);
        }
    }
    markdown_core_iter_free(iter);

    return S_postprocess_subtree(parser, unit, unit);
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
