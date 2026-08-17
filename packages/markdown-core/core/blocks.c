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
    /* An extension that opens blocks and puts a payload behind a node's
     * pointer must say how large it is: a stream's close is undone from a
     * snapshot of it, and there is no other way to reopen a block that
     * carries one. (An inline node's payload is never on the open spine.) */
    if (extension->try_opening_block && extension->alloc_opaque && !extension->opaque_size) {
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
    markdown_core_parser_warm_vanished_free(parser);
    /* After every node this parser owns is gone: their probes leave the
     * index as they die, and the index goes with the last of them. */
    markdown_core_probe_index_release(parser->probe_index);
    parser->probe_index = NULL;
    parser->mem->free(parser->mem, parser->probe_hashes);
    parser->probe_hashes = NULL;
    parser->probe_count = parser->probe_capacity = 0;
    parser->mem->free(parser->mem, parser->pending_flips);
    parser->pending_flips = NULL;
    parser->pending_count = parser->pending_capacity = 0;
    markdown_core_parser_warm_flipped_free(parser);
    parser->mem->free(parser->mem, parser->flipped);
    parser->flipped = NULL;
    parser->flipped_capacity = 0;

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
    parser->diagnostics[parser->diagnostic_count].unit = parser->refining;
    parser->diagnostics[parser->diagnostic_count].dead = false;
    parser->diagnostic_count++;
}

/* A unit refined again raises its diagnostics again: the ones its last
 * refine raised are dropped — or, when the refine may be undone, hidden
 * until the retract drops them and refines the unit once more. */
static void warm_diagnostics_drop(markdown_core_parser *parser, const markdown_core_node *unit, bool hide) {
    size_t i;
    size_t kept = 0;
    for (i = 0; i < parser->diagnostic_count; i++) {
        if (parser->diagnostics[i].unit == unit) {
            if (hide) {
                parser->diagnostics[i].dead = true;
            } else {
                continue;
            }
        }
        if (kept != i) {
            parser->diagnostics[kept] = parser->diagnostics[i];
        }
        kept++;
    }
    parser->diagnostic_count = kept;
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
    bool answer;
    if (S_last_line_checked(node)) {
        return (node->flags & MARKDOWN_CORE_NODE__ENDS_BLANK) != 0;
    }
    if ((S_type(node) == MARKDOWN_CORE_NODE_LIST || S_type(node) == MARKDOWN_CORE_NODE_LIST_ITEM) && node->last_child) {
        answer = S_ends_with_blank_line(node->last_child);
    } else {
        answer = S_last_line_blank(node);
    }
    S_set_last_line_checked(node);
    if (answer) {
        node->flags |= MARKDOWN_CORE_NODE__ENDS_BLANK;
    } else {
        node->flags &= (uint16_t)~MARKDOWN_CORE_NODE__ENDS_BLANK;
    }
    return answer;
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
/* A definition just registered: if it is its label's first — the one a
 * lookup now answers with — the label is pending a flip, and the next settle
 * or publish re-refines the units that asked about it. A definition that
 * repeats a label changes no answer and pends nothing. */
static void S_pend_flip(markdown_core_parser *parser, markdown_core_map *map, const markdown_core_map_entry *before) {
    markdown_core_map_entry *entry;
    for (entry = map ? map->refs : NULL; entry && entry != before; entry = entry->next) {
        if (!markdown_core_map_entry_wins(map, entry)) {
            continue;
        }
        if (parser->pending_count == parser->pending_capacity) {
            size_t capacity = parser->pending_capacity ? parser->pending_capacity * 2 : 4;
            uint64_t *grown =
                (uint64_t *)parser->mem->realloc(parser->mem, parser->pending_flips, capacity * sizeof(*grown));
            if (!grown) {
                parser->oom = true;
                return;
            }
            parser->pending_flips = grown;
            parser->pending_capacity = capacity;
        }
        parser->pending_flips[parser->pending_count++] = markdown_core_map_label_hash(entry->label);
    }
}

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
        S_pend_flip(parser, parser->refmap, before);
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
            /* Nothing but definitions: the paragraph leaves the tree. It is
             * kept, unlinked, rather than freed — a close that is to be
             * undone puts it back, a record that named it must learn it
             * left — on the parser's list, `prev` still the sibling it
             * followed. */
            markdown_core_node *prev = b->prev;
            markdown_core_node_unlink(b);
            b->prev = prev;
            b->next = parser->vanished;
            parser->vanished = b;
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

    case MARKDOWN_CORE_NODE_LIST: { // determine tight/loose status
        /* Weighed from the first item not yet marked (node.h TIGHT_SCANNED /
         * LOOSE_AT): an item that has a sibling after it is settled, and
         * what it weighs — a blank line at its end before that sibling, or
         * a blank line between its own blocks — never changes again, so a
         * close that comes while the list is still open (a stream's, at
         * every tick) weighs what grew, not the list. The whole-list weigh
         * gives the same answer. */
        bool loose;
        item = b->last_child;
        while (item && item->prev &&
               !(item->prev->flags & (MARKDOWN_CORE_NODE__TIGHT_SCANNED | MARKDOWN_CORE_NODE__LOOSE_AT))) {
            item = item->prev;
        }
        loose = item && item->prev && (item->prev->flags & MARKDOWN_CORE_NODE__LOOSE_AT) != 0;
        for (; item; item = item->next) {
            if (!loose) {
                // check for non-final non-empty list item ending with blank line:
                if (S_last_line_blank(item) && item->next) {
                    loose = true;
                } else {
                    // recurse into children of list item, to see if there are
                    // spaces between them:
                    for (subitem = item->first_child; subitem; subitem = subitem->next) {
                        if ((item->next || subitem->next) && S_ends_with_blank_line(subitem)) {
                            loose = true;
                            break;
                        }
                    }
                }
            }
            if (loose) {
                item->flags |= MARKDOWN_CORE_NODE__LOOSE_AT;
            } else if (item->next) {
                item->flags |= MARKDOWN_CORE_NODE__TIGHT_SCANNED;
            }
        }
        b->as.list.tight = !loose;
        break;
    }

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
        /* Not the memos of "ends with a blank line" and of a list's weighed
         * items: caches a list's finalize fills on settled nodes, whose
         * answers recomputation gives back — present or absent, every parse
         * continues the same. */
        hash = fp_mix(
            hash,
            (uint64_t)(node->flags & ~(MARKDOWN_CORE_NODE__LAST_LINE_CHECKED | MARKDOWN_CORE_NODE__ENDS_BLANK |
                                       MARKDOWN_CORE_NODE__TIGHT_SCANNED | MARKDOWN_CORE_NODE__LOOSE_AT))
        );
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
            {
                const markdown_core_map_entry *before = parser->footnote_defs ? parser->footnote_defs->refs : NULL;
                markdown_core_footnote_definition_create(parser->footnote_defs, &c);
                S_pend_flip(parser, parser->footnote_defs, before);
            }
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
 * unless a postprocessor replaced it — and hands the replaced unit back
 * through `replaced`, unlinked and alive (extensions/formula.c replaces in
 * two of its arms): the whole-tree driver frees it, and a stream may keep
 * it to put the unit back after a speculative close. */
static markdown_core_node *S_postprocess_unit(
    markdown_core_parser *parser,
    markdown_core_node *unit,
    bool owns_inlines,
    markdown_core_node **replaced
) {
    markdown_core_llist *extensions;

    *replaced = NULL;

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
            if (processed && processed != unit) {
                /* The hook unlinked the unit and spliced the survivor in; the
                 * unit is the caller's now — freed by the whole-tree walk,
                 * kept by a stream that may need to put it back. A second
                 * replacement in one pipeline would orphan the first; no
                 * bundled hook replaces what another already did. */
                if (*replaced) {
                    markdown_core_node_free(*replaced);
                }
                *replaced = unit;
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
 * Does not stamp `subtree_hash`: the hash is the streaming frontier's to
 * pair on, and the facade stamps exactly the subtrees the frontier will
 * pair, when it publishes them (extensions/document.c). */
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

        {
            markdown_core_node *replaced;
            S_postprocess_unit(parser, node, owns_inlines, &replaced);
            if (replaced) {
                markdown_core_node_free(replaced);
            }
        }
        node = next;
    }
}

static void S_postprocess_blocks(markdown_core_parser *parser) {
    if (parser->root->first_child) {
        S_postprocess_subtree(parser, parser->root, parser->root->first_child);
    }
}

/* --- the warm tick: settle, publish, retract ------------------------------- */

/* WHEN A BUILD CAN BE REOPENED: always. Every close is retractable — what
 * it writes into the open spine's flags, coordinates and payloads, into a
 * leaf's content, into the definition tables and the units those
 * definitions flip, and the leaf paragraph it takes for being nothing but
 * definitions, the record holds and the retract puts back; an extension's
 * block carries a payload only if the extension has said how large it is
 * (markdown_core_parser_attach_extension refuses one that has not), so the
 * record snapshots exactly that. Nothing is decided by the arriving bytes,
 * and a parser publishes from any state a feed can leave it in — unless it
 * has failed. */

bool markdown_core_parser_warm_eligible_at_eof(const markdown_core_parser *parser) {
    return parser->root != NULL && !parser->oom && !parser->internal_error;
}

/* Whether a block's close moves its content bytes out of the buffer, so
 * the record must keep a copy of them. */
static bool warm_close_moves_content(const markdown_core_node *node) {
    if (node->extension) {
        /* An extension's block that takes lines does something with them
         * at its close or its refine (a formula block's literal is minted
         * from its content, and the content cleared); the copy costs what
         * the block is, and pays for not guessing. */
        return node->extension->accepts_lines &&
               node->extension->accepts_lines(node->extension, (markdown_core_node *)node);
    }
    if (S_type(node) == MARKDOWN_CORE_NODE_PARAGRAPH) {
        /* Its close harvests definitions off the FRONT of the content when
         * it begins with one; the bytes come back from the copy. */
        return node->content.size > 0 && node->content.ptr[0] == '[';
    }
    return S_type(node) == MARKDOWN_CORE_NODE_CODE_BLOCK || S_type(node) == MARKDOWN_CORE_NODE_HTML_BLOCK;
}

/* --- settle: refine what a step closed ------------------------------------ */

/* Whether `node` is on the open chain — the current block or one of its
 * ancestors. That, and not the OPEN flag, is what "still growing" means:
 * the flag stays set for life on a block an extension made and never routed
 * through the block phase (a table cell), while a settled block is simply
 * one the chain has left behind. O(depth); the region walk below never
 * needs it per node. */
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
 * a refine may replace a leaf and free what it replaced. Nodes on the open
 * chain are walked through and left alone: the step that closes them will
 * refine them.
 *
 * Iterative, like every walk in this engine — nesting is bounded only by
 * the input. Chain membership costs nothing extra: a block is on the chain
 * exactly when it is the current block or when its youngest child was, and
 * in a postorder walk the youngest child is the node that exited just
 * before its parent, so one remembered exit answers it. */
static markdown_core_node *warm_settle_subtree(markdown_core_parser *parser, markdown_core_node *root) {
    markdown_core_node *node = root;
    markdown_core_node *last_exited = NULL;
    bool last_exited_on_chain = false;

    for (;;) {
        /* Down to the first unit that owns inlines or has no children —
         * inline-owning units are leaves of this walk, their subtrees being
         * the inline parse's to make. */
        while (!contains_inlines(node) && node->first_child) {
            node = node->first_child;
        }
        for (;;) {
            bool on_chain = node == parser->current ||
                            (node->last_child && last_exited == node->last_child && last_exited_on_chain);
            markdown_core_node *replaced = NULL;
            markdown_core_node *survivor =
                on_chain ? node : markdown_core_parser_warm_refine_settled(parser, node, &replaced);
            if (replaced) {
                /* A settled unit its own refine replaced: what it replaced
                 * is nothing to anyone now. */
                markdown_core_node_free(replaced);
            }
            last_exited = survivor;
            last_exited_on_chain = on_chain;
            if (node == root) {
                return survivor;
            }
            if (survivor->next) {
                node = survivor->next;
                break;
            }
            node = survivor->parent;
        }
    }
}

/* The region a snapshot describes: for each saved open block, deepest first,
 * every child the step appended past its saved youngest child, then the
 * block itself if the step closed it. Answers whether every spine block is
 * still the object the snapshot named — a spine block replaced by its own
 * refine (a paragraph promoted to a formula block) is one the record can no
 * longer put back, and the entry is repointed at the survivor so nothing
 * dangles. */
/* `keep_replaced` says a spine block its refine replaces is kept on its
 * entry (a publish, whose retract puts it back) rather than freed (a settle,
 * whose replacement is for good). */
static void warm_unthread_subtree(markdown_core_node *root);
static void warm_free_run(markdown_core_node *run);

static void warm_refine_region(
    markdown_core_parser *parser,
    markdown_core_warm_open_block *spine,
    size_t count,
    bool keep_replaced
) {
    size_t i = count;
    while (i-- > 0) {
        markdown_core_warm_open_block *entry = &spine[i];
        markdown_core_node *node = entry->node;
        markdown_core_node *child = markdown_core_warm_run_first(entry);
        while (child) {
            markdown_core_node *survivor = warm_settle_subtree(parser, child);
            child = markdown_core_warm_run_next(entry, survivor);
        }
        /* The document root is never refined: refine_blocks starts below it,
         * and every hook expects a block. */
        if (i > 0 && !warm_on_chain(parser, node)) {
            markdown_core_node *replaced = NULL;
            markdown_core_node *survivor = markdown_core_parser_warm_refine_settled(parser, node, &replaced);
            if (survivor != node) {
                /* The survivor sits where the block sat, so its parent's
                 * saved youngest child — this very block — is repointed too,
                 * and the parent's run still begins after it. The block
                 * itself is kept for the retract to put back, or freed. */
                entry->node = survivor;
                if (spine[i - 1].last_child == node) {
                    spine[i - 1].last_child = survivor;
                }
                if (keep_replaced) {
                    /* Kept off the tree, and off the index with it: it comes
                     * back as the OPEN leaf, which has no inline children
                     * and which no definition refines — so the children its
                     * refine gave it before the promotion, never published,
                     * go now, and not as a run some retract would retire. */
                    warm_free_run(replaced->first_child);
                    replaced->first_child = NULL;
                    replaced->last_child = NULL;
                    markdown_core_inline_concrete_records_free(
                        markdown_core_node_mem(replaced),
                        replaced->inline_concrete
                    );
                    replaced->inline_concrete = NULL;
                    warm_unthread_subtree(replaced);
                    entry->replaced = replaced;
                } else {
                    markdown_core_node_free(replaced);
                }
            }
        }
    }
}

/* --- flips: a definition changes what settled units answered ------------- */

static struct markdown_core_warm_flip *warm_log_flipped(markdown_core_parser *parser) {
    if (parser->flipped_count == parser->flipped_capacity) {
        size_t capacity = parser->flipped_capacity ? parser->flipped_capacity * 2 : 4;
        struct markdown_core_warm_flip *grown =
            (struct markdown_core_warm_flip *)
                parser->mem->realloc(parser->mem, parser->flipped, capacity * sizeof(*grown));
        if (!grown) {
            return NULL;
        }
        parser->flipped = grown;
        parser->flipped_capacity = capacity;
    }
    return &parser->flipped[parser->flipped_count++];
}

static struct markdown_core_warm_flip *warm_record_flip(markdown_core_warm_undo *undo) {
    if (undo->flip_count == undo->flip_capacity) {
        size_t capacity = undo->flip_capacity ? undo->flip_capacity * 2 : 4;
        struct markdown_core_warm_flip *grown =
            (struct markdown_core_warm_flip *)undo->mem->realloc(undo->mem, undo->flips, capacity * sizeof(*grown));
        if (!grown) {
            return NULL;
        }
        undo->flips = grown;
        undo->flip_capacity = capacity;
    }
    return &undo->flips[undo->flip_count++];
}

void markdown_core_parser_warm_vanished_free(markdown_core_parser *parser) {
    markdown_core_node *node = parser->vanished;
    while (node) {
        markdown_core_node *next = node->next;
        node->prev = NULL;
        node->next = NULL;
        markdown_core_node_free(node);
        node = next;
    }
    parser->vanished = NULL;
}

bool markdown_core_parser_warm_vanished(const markdown_core_parser *parser, const markdown_core_node *node) {
    const markdown_core_node *item;
    for (item = parser->vanished; item; item = item->next) {
        if (item == node) {
            return true;
        }
    }
    return false;
}

void markdown_core_parser_warm_flipped_free(markdown_core_parser *parser) {
    size_t i;
    for (i = 0; i < parser->flipped_count; i++) {
        warm_free_run(parser->flipped[i].children);
    }
    parser->flipped_count = 0;
}

/* Re-refines one settled unit whose answer a definition changed: its inline
 * children are detached — kept on the parser's list for the caller to pair
 * against and free, or on the record for a retract to put back — and it is
 * parsed again against the tables as they are now. The unit keeps its
 * identity; its children are new objects. */
static void warm_flip_unit(markdown_core_parser *parser, markdown_core_node *unit, markdown_core_warm_undo *undo) {
    markdown_core_node *replaced = NULL;
    struct markdown_core_warm_flip *flip = undo ? warm_record_flip(undo) : warm_log_flipped(parser);
    markdown_core_node *child;
    if (!flip) {
        parser->oom = true;
        return;
    }
    flip->unit = unit;
    flip->children = unit->first_child;
    flip->published = NULL;
    for (child = unit->first_child; child; child = child->next) {
        child->parent = NULL;
    }
    unit->first_child = NULL;
    unit->last_child = NULL;
    /* The records, the probes and the diagnostics go with the refine they
     * belong to. */
    markdown_core_inline_concrete_records_free(markdown_core_node_mem(unit), unit->inline_concrete);
    unit->inline_concrete = NULL;
    markdown_core_probes_free(unit->probes);
    unit->probes = NULL;
    warm_diagnostics_drop(parser, unit, undo != NULL);
    /* Same bytes, same postprocess: what did not replace the unit before does
     * not now, and what would have was never a unit that asked. */
    markdown_core_parser_warm_refine_settled(parser, unit, &replaced);
    if (replaced) {
        markdown_core_node_free(replaced);
    }
}

static int warm_unit_order(const void *a, const void *b) {
    const markdown_core_node *x = *(const markdown_core_node *const *)a;
    const markdown_core_node *y = *(const markdown_core_node *const *)b;
    return x < y ? -1 : x > y;
}

/* Every settled unit that asked about any pending label is re-refined ONCE,
 * against the tables as they are now — found through the probe index, in
 * the size of what asked, so a definition costs the units it reaches and a
 * definition storm costs the labels it registers. Once, because a second
 * refine of the same unit in one pass would see the same tables and answer
 * the same, and its record would hold children that were never published.
 * The units are gathered before any is flipped: a flip rethreads its unit's
 * probes, and the chains are not walked while they move. */
static void warm_flip_pending(markdown_core_parser *parser, markdown_core_warm_undo *undo) {
    markdown_core_node **units = NULL;
    size_t count = 0;
    size_t capacity = 0;
    size_t i;
    if (parser->pending_count == 0) {
        return;
    }
    for (i = 0; i < parser->pending_count; i++) {
        if (!markdown_core_probe_index_units(
                parser->probe_index,
                parser->pending_flips[i],
                parser->mem,
                &units,
                &count,
                &capacity
            )) {
            parser->oom = true;
            parser->mem->free(parser->mem, units);
            parser->pending_count = 0;
            return;
        }
    }
    parser->pending_count = 0;
    if (count > 1) {
        qsort(units, count, sizeof(*units), warm_unit_order);
    }
    for (i = 0; i < count; i++) {
        markdown_core_node *unit = units[i];
        if (i > 0 && units[i - 1] == unit) {
            continue;
        }
        /* A unit that asked, is refined, and is not still open: an open
         * unit's refine is the close's, and comes with the tables as they
         * are then. */
        if (contains_inlines(unit) && !warm_on_chain(parser, unit)) {
            warm_flip_unit(parser, unit, undo);
        }
    }
    parser->mem->free(parser->mem, units);
}

bool markdown_core_parser_warm_settle(markdown_core_parser *parser, markdown_core_warm_undo *before) {
    if (!parser->root) {
        return false;
    }
    if (before) {
        /* Definitions the feed registered first: the units settled earlier
         * that asked about them are re-refined, for good, before what the
         * feed closed is refined against the tables as they are now. */
        warm_flip_pending(parser, NULL);
        warm_refine_region(parser, before->spine, before->spine_count, false);
        return true;
    }
    /* No record: a fresh parser, whose whole settled part is unrefined —
     * refined now against every definition the feed registered, so nothing
     * is pending; and no record named what the feed took. */
    parser->pending_count = 0;
    markdown_core_parser_warm_vanished_free(parser);
    {
        markdown_core_node *child = parser->root->first_child;
        while (child) {
            markdown_core_node *survivor = warm_settle_subtree(parser, child);
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
        warm_free_run(undo->spine[i].retired_inserted);
        if (undo->spine[i].replaced) {
            markdown_core_node_free(undo->spine[i].replaced);
        }
        /* A vanished paragraph the record still holds — never put back —
         * is nobody else's. */
        if (undo->spine[i].vanished && !undo->retracted) {
            markdown_core_node_free(undo->spine[i].node);
        }
        mem->free(mem, undo->spine[i].content_copy);
        mem->free(mem, undo->spine[i].opaque_copy);
    }
    /* What the close's flips took off their units, and what the retract
     * did: both runs are dead once the caller has paired against them. */
    for (i = 0; i < undo->flip_count; i++) {
        warm_free_run(undo->flips[i].children);
        warm_free_run(undo->flips[i].published);
    }
    mem->free(mem, undo->flips);
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
            entry->prev = node->last_child ? node->last_child->prev : NULL;
            entry->type = node->type;
            entry->flags = node->flags;
            entry->end_line = node->end_line;
            entry->end_column = node->end_column;
            entry->content_size = node->content.size;
            entry->payload = node->as;
            entry->concrete_count = node->concrete ? node->concrete->count : 0;
            entry->last_child_flags = node->last_child ? node->last_child->flags : 0;
            entry->extension = node->extension;
            entry->start_line = node->start_line;
            entry->start_column = node->start_column;
            if (node->extension && node->extension->opaque_size && node->as.opaque) {
                size_t size = node->extension->opaque_size(node->extension, node);
                if (size) {
                    entry->opaque_copy = undo->mem->calloc(undo->mem, size, 1);
                    if (!entry->opaque_copy) {
                        return false;
                    }
                    memcpy(entry->opaque_copy, node->as.opaque, size);
                    entry->opaque_copy_size = size;
                }
            }
            entry->content_moved = warm_close_moves_content(node);
            if (entry->content_moved && node->content.size) {
                entry->content_copy = (unsigned char *)undo->mem->calloc(undo->mem, node->content.size, 1);
                if (!entry->content_copy) {
                    return false;
                }
                memcpy(entry->content_copy, node->content.ptr, node->content.size);
                entry->content_copy_size = node->content.size;
            }
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
    {
        undo->definitions = parser->refmap ? parser->refmap->size : 0;
        undo->footnotes = parser->footnote_defs ? parser->footnote_defs->size : 0;
        markdown_core_parser_finalize_blocks(parser);
        /* The close took the leaf paragraph — nothing but definitions — and
         * the record keeps it, with the sibling it followed, to put back.
         * (What the feed took stays on the parser's list for the caller.) */
        if (parser->vanished) {
            markdown_core_warm_open_block *leaf = &undo->spine[undo->spine_count - 1];
            /* Anywhere on the list, not only at its head: the held line
             * that closed the leaf may itself have opened and closed a
             * second definitions-only paragraph (`> [b]: /2`), which the
             * parser pushed after it. */
            markdown_core_node **link = &parser->vanished;
            while (*link && *link != leaf->node) {
                link = &(*link)->next;
            }
            if (*link) {
                leaf->vanished = true;
                leaf->vanished_prev = leaf->node->prev;
                *link = leaf->node->next;
                leaf->node->prev = NULL;
                leaf->node->next = NULL;
            }
        }
        /* Definitions the close registered change what settled units
         * answered; those units are re-refined for this projection and keep
         * their old children on the record. */
        warm_flip_pending(parser, undo);
    }
    /* Only what the close closed: the spine, and whatever the held line put
     * under it. Units the feed closed are the caller's to settle, and were. */
    warm_refine_region(parser, undo->spine, undo->spine_count, true);
    return undo;
}

/* Makes every node of a subtree own its bytes: the retired frontier outlives
 * the buffer its literals borrow (the open leaf's content, which the next
 * feed appends to and may move), and the identity handover reads those
 * literals to say whether a paired node changed. */
/* A subtree that leaves the tree — retired, kept for pairing — leaves the
 * probe index too: a definition that arrives finds units IN the tree, and a
 * retired unit is nobody's to refine again. */
static void warm_unthread_subtree(markdown_core_node *root) {
    markdown_core_node *node = root;
    for (;;) {
        markdown_core_probes_free(node->probes);
        node->probes = NULL;
        if (node->first_child) {
            node = node->first_child;
            continue;
        }
        while (node != root && !node->next) {
            node = node->parent;
        }
        if (node == root) {
            return;
        }
        node = node->next;
    }
}

static bool warm_own_subtree(markdown_core_node *root) {
    markdown_core_node *node = root;
    warm_unthread_subtree(root);
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

    if (!undo || undo->retracted) {
        return false;
    }
    /* Before anything moves: what is about to be retired must own its bytes,
     * and if it cannot, nothing has changed and the record is still the
     * published one. The run a block will retire begins after its saved
     * youngest child — or, when that child is the leaf the close took
     * (unlinked, so its `next` says nothing), after the sibling the leaf
     * followed, which is where the leaf goes back and the run resumes. */
    for (i = 0; i < undo->spine_count; i++) {
        markdown_core_warm_open_block *entry = &undo->spine[i];
        markdown_core_node *child;
        if (entry->last_child && i + 1 < undo->spine_count && undo->spine[i + 1].vanished &&
            undo->spine[i + 1].node == entry->last_child) {
            child =
                undo->spine[i + 1].vanished_prev ? undo->spine[i + 1].vanished_prev->next : entry->node->first_child;
        } else {
            child = entry->last_child ? entry->last_child->next : entry->node->first_child;
        }
        for (; child; child = child->next) {
            if (!warm_own_subtree(child)) {
                /* An allocation lost, and the sticky bit says so, so the
                 * caller reports it as one and not as a broken record. */
                parser->oom = true;
                return false;
            }
        }
    }
    /* The definitions the close registered go, and the units they flipped
     * give up the flip's children — detached and kept as what it published
     * — with the records and probes of that refine; they are refined again
     * once everything else is back, at the end. */
    markdown_core_map_truncate(parser->refmap, undo->definitions);
    markdown_core_map_truncate(parser->footnote_defs, undo->footnotes);
    for (i = 0; i < undo->flip_count; i++) {
        struct markdown_core_warm_flip *flip = &undo->flips[i];
        markdown_core_node *unit = flip->unit;
        markdown_core_node *child;
        flip->published = unit->first_child;
        for (child = unit->first_child; child; child = child->next) {
            child->parent = NULL;
        }
        unit->first_child = NULL;
        unit->last_child = NULL;
        markdown_core_inline_concrete_records_free(markdown_core_node_mem(unit), unit->inline_concrete);
        unit->inline_concrete = NULL;
        markdown_core_probes_free(unit->probes);
        unit->probes = NULL;
    }
    parser->pending_count = 0;
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
        markdown_core_node *node;
        markdown_core_node *run;
        markdown_core_node *inserted;
        if (entry->replaced) {
            /* The close's refine replaced this block (a paragraph promoted to
             * a formula block): the block goes back where the survivor
             * stands, and the survivor — published, paired, and now gone —
             * is freed. */
            markdown_core_node *survivor = entry->node;
            markdown_core_node_insert_before_unchecked(survivor, entry->replaced);
            markdown_core_node_unlink(survivor);
            markdown_core_node_free(survivor);
            entry->node = entry->replaced;
            entry->replaced = NULL;
            if (i > 0 && undo->spine[i - 1].last_child == survivor) {
                undo->spine[i - 1].last_child = entry->node;
            }
        }
        node = entry->node;
        /* A leaf paragraph the close took goes back where it stood — after
         * the sibling it followed, which is the last of what the harvest
         * inserted before it — under the parent, which is the next entry up. */
        if (entry->vanished && i > 0) {
            markdown_core_node *parent = undo->spine[i - 1].node;
            if (entry->vanished_prev) {
                markdown_core_node_insert_after_unchecked(entry->vanished_prev, node);
            } else if (parent->first_child) {
                markdown_core_node_insert_before_unchecked(parent->first_child, node);
            } else {
                markdown_core_node_append_child_unchecked(parent, node);
            }
            entry->vanished = false;
        }
        run = entry->last_child ? entry->last_child->next : node->first_child;
        inserted = entry->prev ? entry->prev->next : node->first_child;
        /* What the close INSERTED before the youngest child — the definitions
         * harvested out of it, a paragraph split off a table — is retired
         * too, apart, for the next publish's insertions to pair against. (A
         * block with no youngest child had nothing to insert before;
         * everything under it is the appended run.) */
        if (entry->last_child && inserted != entry->last_child) {
            markdown_core_node *cursor = inserted;
            markdown_core_node *tail = NULL;
            while (cursor && cursor != entry->last_child) {
                tail = cursor;
                cursor = cursor->next;
            }
            if (tail) {
                tail->next = NULL;
            }
            inserted->prev = NULL;
            entry->retired_inserted = inserted;
            for (cursor = inserted; cursor; cursor = cursor->next) {
                warm_unthread_subtree(cursor);
            }
            if (entry->prev) {
                entry->prev->next = entry->last_child;
            } else {
                node->first_child = entry->last_child;
            }
            entry->last_child->prev = entry->prev;
        }
        if (entry->last_child) {
            entry->last_child->next = NULL;
        } else {
            node->first_child = NULL;
        }
        node->last_child = entry->last_child;
        if (entry->last_child) {
            entry->last_child->flags = entry->last_child_flags;
        }
        if (run) {
            run->prev = NULL;
        }
        entry->retired = run;
        if (node->inline_concrete) {
            markdown_core_inline_concrete_records_free(markdown_core_node_mem(node), node->inline_concrete);
            node->inline_concrete = NULL;
        }
        /* The close's refine of this block asked the tables; that refine is
         * retired with its children, and so is what it asked. */
        markdown_core_probes_free(node->probes);
        node->probes = NULL;
        node->flags = entry->flags;
        node->end_line = entry->end_line;
        node->end_column = entry->end_column;
        node->start_line = entry->start_line;
        node->start_column = entry->start_column;
        entry->published_type = node->type;
        entry->published_payload = node->as;
        entry->published_own_hash = markdown_core_node_stamp_own(node);
        /* A retype that attached an extension (a paragraph turned table)
         * minted a payload the extension frees; a block that had one keeps
         * it and gets its bytes back. */
        if (node->extension != entry->extension) {
            if (node->extension && node->extension->free_opaque && node->as.opaque) {
                node->extension->free_opaque(node->extension, markdown_core_node_mem(node), node);
            }
            node->as.opaque = NULL;
            node->extension = entry->extension;
        } else if (entry->opaque_copy && node->as.opaque) {
            if (node->extension->restore_opaque) {
                node->extension
                    ->restore_opaque(node->extension, markdown_core_node_mem(node), node, entry->opaque_copy);
            } else {
                memcpy(node->as.opaque, entry->opaque_copy, entry->opaque_copy_size);
            }
        }
        node->type = entry->type;
        if (entry->content_moved) {
            /* The close moved the bytes into the literal (and, for a fenced
             * block, minted the info from their first line): those chunks
             * go, and the buffer gets its bytes back from the copy. An
             * extension's block frees what its payload minted in its own
             * restore below. */
            if (!node->extension && S_type(node) == MARKDOWN_CORE_NODE_CODE_BLOCK) {
                markdown_core_chunk_free(markdown_core_node_mem(node), &node->as.code.info);
                markdown_core_chunk_free(markdown_core_node_mem(node), &node->as.code.literal);
            } else if (!node->extension) {
                markdown_core_chunk_free(markdown_core_node_mem(node), &node->as.literal);
            }
            markdown_core_strbuf_clear(&node->content);
            if (entry->content_copy_size) {
                markdown_core_strbuf_put(&node->content, entry->content_copy, entry->content_copy_size);
                if (node->content.oom) {
                    /* The bytes did not come back: the parser is not where it
                     * was, and says so through its sticky bit — the caller's
                     * tick fails, and with it the chain. */
                    parser->oom = true;
                    return false;
                }
            }
        } else {
            markdown_core_strbuf_truncate(&node->content, entry->content_size);
        }
        node->as = entry->payload;
        if (node->concrete) {
            if (entry->concrete_count == 0) {
                markdown_core_concrete_records_free(markdown_core_node_mem(node), node->concrete);
                node->concrete = NULL;
            } else {
                node->concrete->count = entry->concrete_count;
            }
        }
        /* A list's tentative finalize memoized "ends with a blank line" and
         * "weighed tight" on nodes it looked at (node.h), and every one of
         * them is closed — an item it weighs has a sibling after it, a block
         * it asks has a sibling after it or sits under such an item, and a
         * closed node's blank-line answer never changes — so the memos are
         * as good as a one-shot parse's and stay. Nothing to take back. */
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
    /* Last, the units the close flipped are refined again, against the
     * tables as they are now — the diagnostics the flip hid go first, since
     * this refine raises them afresh — so each stands as it did before the
     * close, in new objects. Nothing above reads their children. */
    for (i = 0; i < undo->flip_count; i++) {
        markdown_core_node *unit = undo->flips[i].unit;
        markdown_core_node *replaced = NULL;
        warm_diagnostics_drop(parser, unit, false);
        markdown_core_parser_warm_refine_settled(parser, unit, &replaced);
        if (replaced) {
            markdown_core_node_free(replaced);
        }
    }
    undo->retracted = true;
    return true;
}

markdown_core_node *markdown_core_parser_warm_refine_settled(
    markdown_core_parser *parser,
    markdown_core_node *unit,
    markdown_core_node **replaced
) {
    bool owns_inlines = contains_inlines(unit);

    if (owns_inlines) {
        S_parse_node_inlines(parser, unit, parser->refmap, parser->options);
    }
    /* This unit alone: its children settled through their own calls. The
     * survivor comes back because a postprocessor may have replaced this
     * node; the replaced unit comes back too, unlinked, for the caller to
     * free or to keep. */
    return S_postprocess_unit(parser, unit, owns_inlines, replaced);
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

    /* NO STAMP: the subtree hash is what the streaming frontier pairs on
     * (extensions/diff.c), and a tree this hands over is finished — nothing
     * pairs against it. Stamping it whole once cost ~6% of parse time on
     * the throughput corpus and ~11% on multiple_duplicate_references. */

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
