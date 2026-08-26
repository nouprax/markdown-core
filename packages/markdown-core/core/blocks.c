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

/* Appends and reports failure directly instead of relying on llist_append's
 * silent-drop behavior.
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
    if (parser->source.mem) {
        markdown_core_strbuf_free(&parser->source);
    }
    parser->mem->free(parser->line_starts);
    parser->line_starts = NULL;
    parser->line_starts_size = 0;
    parser->line_starts_alloc = 0;
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

    /* Requirement 13's list. Released here whether or not it was retained: a
     * parse that FAILED never reaches the move, and the requirement's converse
     * says a failure carries no diagnostics. */
    markdown_core_diagnostics_dispose(&parser->diagnostics);
}

static void markdown_core_parser_reset(markdown_core_parser *parser) {
    markdown_core_llist *saved_exts = parser->syntax_extensions;
    markdown_core_llist *saved_inline_exts = parser->inline_syntax_extensions;
    int saved_options = parser->options;
    markdown_core_mem *saved_mem = parser->mem;

    markdown_core_parser_dispose(parser);

    memset(parser, 0, sizeof(markdown_core_parser));
    parser->mem = saved_mem;

    markdown_core_strbuf_init(parser->mem, &parser->curline, 256);
    markdown_core_strbuf_init(parser->mem, &parser->linebuf, 0);
    markdown_core_strbuf_init(parser->mem, &parser->source, 0);

    markdown_core_node *document = make_document(parser->mem);

    parser->refmap = markdown_core_reference_map_new(parser->mem);
    parser->footnote_defs = markdown_core_footnote_definition_map_new(parser->mem);
    parser->root = document;
    parser->current = document;
    if (document) {
        markdown_core_parser_touch(parser, document);
    }

    parser->syntax_extensions = saved_exts;
    parser->inline_syntax_extensions = saved_inline_exts;
    parser->options = saved_options;

    /* A reset that could not rebuild its structures poisons the parser: feed
     * becomes a no-op and finish reports failure. */
    if (!parser->root || !parser->refmap || !parser->footnote_defs || parser->curline.oom || parser->source.oom) {
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
    markdown_core_strbuf_free(&parser->source);
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

/* REQUIREMENT 13 -- the diagnostic list.
 *
 * It is the concrete record set's shape with no difference at all, and the
 * sameness is the point: A GROWTH THIS LIST CANNOT AFFORD ABANDONS THE PARSE,
 * exactly as `S_claim_region`'s does.
 *
 * OWNER RULING, 2026-08-24: "we do not want fallback when OOM happens; the
 * parser should return an error rather than return a fallback." So there is no
 * truncation marker and no short list: either the parse produced its complete
 * diagnostics, or there is no document. A degraded success is a document that
 * lies about how much the engine had to say about it, and this repository has
 * spent four defects (D27, D30, and both halves of A1) learning that a lossy
 * result reported as a success is worse than no result. */

/* The message pool cap. A diagnostic says what the tree cannot; it does not
 * quote the document back. Cutting at a code-point boundary is not tidiness:
 * `message` is published as a UTF-8 `markdown_core_string`, and a cut through a
 * continuation byte would hand a consumer a sequence the engine itself would
 * have replaced on input. */
#define MARKDOWN_CORE_DIAGNOSTIC_SUBJECT_MAX 40

static bufsize_t S_diagnostic_subject_fit(const unsigned char *subject, bufsize_t length) {
    if (length <= MARKDOWN_CORE_DIAGNOSTIC_SUBJECT_MAX) {
        return length;
    }
    length = MARKDOWN_CORE_DIAGNOSTIC_SUBJECT_MAX;
    while (length > 0 && (subject[length] & 0xC0) == 0x80) {
        length--;
    }
    return length;
}

const char *markdown_core_diagnostic_code_string(markdown_core_diagnostic_code code) {
    switch (code) {
    case MARKDOWN_CORE_DIAGNOSTIC_DIRECTIVE_LABEL_REJECTED:
        return "directive-label-rejected";
    case MARKDOWN_CORE_DIAGNOSTIC_DIRECTIVE_ATTRIBUTES_REJECTED:
        return "directive-attributes-rejected";
    case MARKDOWN_CORE_DIAGNOSTIC_DIRECTIVE_REJECTED:
        return "directive-rejected";
    case MARKDOWN_CORE_DIAGNOSTIC_DIRECTIVE_UNCLOSED:
        return "directive-unclosed";
    case MARKDOWN_CORE_DIAGNOSTIC_TABLE_REJECTED:
        return "table-rejected";
    case MARKDOWN_CORE_DIAGNOSTIC_LABEL_TOO_LONG:
        return "label-too-long";
    }
    return NULL;
}

/* Record one diagnostic, at the place `start` .. `end`, with `message` and an
 * optional excerpt of the source it is about.
 *
 * NOTHING HERE MAY TOUCH `parser->oom`, and nothing here may change what the
 * parse builds. Both are the law. The three failure paths -- recording is off,
 * the entry array cannot grow, the message pool cannot grow -- all leave the
 * document exactly as it would have been, and the last two say so on the list.
 *
 * The message pool is written BEFORE the entry is committed, so a pool that
 * failed leaves no entry naming a slice of it. */
void markdown_core_parser_diagnose(
    markdown_core_parser *parser,
    markdown_core_diagnostic_severity severity,
    markdown_core_diagnostic_code code,
    int start_line,
    int start_column,
    int end_line,
    int end_column,
    const char *message,
    const unsigned char *subject,
    bufsize_t subject_length
) {
    markdown_core_diagnostics *list;
    markdown_core_diagnostic_record *entry;
    bufsize_t pool_start;

    if (!parser || !parser->diagnostics_on || !message) {
        return;
    }
    list = &parser->diagnostics;

    if (list->entries_size == list->entries_alloc) {
        bufsize_t alloc;
        markdown_core_diagnostic_record *grown;
        if (list->entries_alloc > (bufsize_t)(INT32_MAX / 2)) {
            parser->oom = true;
            return;
        }
        alloc = list->entries_alloc ? list->entries_alloc * 2 : 16;
        grown = (markdown_core_diagnostic_record *)parser->mem->realloc(list->entries, (size_t)alloc * sizeof(*grown));
        if (!grown) {
            parser->oom = true;
            return;
        }
        list->entries = grown;
        list->entries_alloc = alloc;
    }

    pool_start = list->messages.size;
    markdown_core_strbuf_puts(&list->messages, message);
    if (subject && subject_length > 0) {
        bufsize_t fit = S_diagnostic_subject_fit(subject, subject_length);
        unsigned char excerpt[MARKDOWN_CORE_DIAGNOSTIC_SUBJECT_MAX];
        bufsize_t i;
        /* A CONTROL BYTE BECOMES A SPACE, and this is the wire format's
         * requirement rather than tidiness: an excerpt is cut out of the
         * source, a label or an attribute list may span a line ending, and one
         * `\n` inside a message turns one diagnostic row into two. Found by the
         * neutrality gate on `spec.txt`, which is the only reason it is a
         * one-line fix rather than a corrupt oracle. */
        for (i = 0; i < fit; i++) {
            excerpt[i] = subject[i] < 0x20 || subject[i] == 0x7F ? (unsigned char)' ' : subject[i];
        }
        markdown_core_strbuf_puts(&list->messages, ": \"");
        markdown_core_strbuf_put(&list->messages, excerpt, fit);
        markdown_core_strbuf_puts(&list->messages, fit < subject_length ? "...\"" : "\"");
    }
    if (list->messages.oom) {
        /* The pool is a strbuf and its failure is sticky, so the partial write
         * above cannot be trusted -- and an entry naming a slice of it would
         * name a message that is a prefix of what it meant to say. */
        parser->oom = true;
        return;
    }

    entry = &list->entries[list->entries_size++];
    entry->start_line = (int32_t)start_line;
    entry->start_column = (int32_t)start_column;
    entry->end_line = (int32_t)end_line;
    entry->end_column = (int32_t)end_column;
    entry->message_start = pool_start;
    entry->message_length = list->messages.size - pool_start;
    entry->severity = (uint8_t)severity;
    entry->code = (uint8_t)code;
}

/* The same, over the LINE IN HAND, from `from` to its last non-space byte.
 *
 * The block phase needs no projection: a line offset IS a column, and the line
 * is the one being processed. This exists because two extensions want it and a
 * second copy of the rtrim is how the two would drift. */
void markdown_core_parser_diagnose_line(
    markdown_core_parser *parser,
    markdown_core_diagnostic_severity severity,
    markdown_core_diagnostic_code code,
    const unsigned char *input,
    bufsize_t len,
    bufsize_t from,
    const char *message,
    const unsigned char *subject,
    bufsize_t subject_length
) {
    bufsize_t to = len;

    if (!parser || !parser->diagnostics_on || !input) {
        return;
    }
    while (to > from &&
           (input[to - 1] == '\n' || input[to - 1] == '\r' || input[to - 1] == ' ' || input[to - 1] == '\t')) {
        to--;
    }
    if (to <= from) {
        return;
    }
    markdown_core_parser_diagnose(
        parser,
        severity,
        code,
        parser->line_number,
        (int)from + 1,
        parser->line_number,
        (int)to,
        message,
        subject,
        subject_length
    );
}

void markdown_core_parser_retain_diagnostics(markdown_core_parser *parser, markdown_core_diagnostics *out) {
    if (!parser || !out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    parser->diagnostics_on = true;
    parser->diagnostics_retain = out;
    if (!parser->diagnostics.mem) {
        parser->diagnostics.mem = parser->mem;
        markdown_core_strbuf_init(parser->mem, &parser->diagnostics.messages, 0);
    }
}

void markdown_core_diagnostics_dispose(markdown_core_diagnostics *diagnostics) {
    if (!diagnostics || !diagnostics->mem) {
        return;
    }
    markdown_core_strbuf_free(&diagnostics->messages);
    diagnostics->mem->free(diagnostics->entries);
    memset(diagnostics, 0, sizeof(*diagnostics));
}

void markdown_core_diagnostics_write(const markdown_core_diagnostics *diagnostics, FILE *out) {
    bufsize_t i;
    bufsize_t count = diagnostics ? diagnostics->entries_size : 0;

    fprintf(out, "diagnostics count=%ld\n", (long)count);
    for (i = 0; i < count; i++) {
        const markdown_core_diagnostic_record *entry = &diagnostics->entries[i];
        const char *name = markdown_core_diagnostic_code_string((markdown_core_diagnostic_code)entry->code);
        fprintf(
            out,
            "diagnostic %s %s %d:%d..%d:%d %.*s\n",
            entry->severity == (uint8_t)MARKDOWN_CORE_DIAGNOSTIC_ERROR ? "ERROR" : "WARNING",
            name ? name : "unknown",
            entry->start_line,
            entry->start_column,
            entry->end_line,
            entry->end_column,
            (int)entry->message_length,
            diagnostics->messages.ptr + entry->message_start
        );
    }
}

/* THE NORMALIZED SOURCE AND ITS LINE INDEX, in the form the gate reads.
 *
 * That is the whole of it. A scope answers where an element is; this answers
 * what its line and column numbers are COUNTED AGAINST -- the normalized
 * source, which is not the bytes the caller passed (a NUL is three bytes here,
 * every line ending is one `\n`, and every line has one). */
static void S_write_concrete(markdown_core_parser *parser, FILE *out) {
    bufsize_t i;

    fprintf(out, "concrete source=%ld lines=%ld\n", (long)parser->source.size, (long)parser->line_starts_size);
    for (i = 0; i < parser->line_starts_size; i++) {
        fprintf(out, "line %ld %ld\n", (long)i + 1, (long)parser->line_starts[i]);
    }
}

#define MARKDOWN_CORE_MAX_INLINE_DEPTH 256

/* Note that a line begins at `start` in the normalized source.
 *
 * Returns false only when the index could not grow, in which case the parse is
 * already marked lost: a line index missing a line would answer a source offset
 * with the wrong line, silently. */
static bool S_record_line_start(markdown_core_parser *parser, bufsize_t start) {
    if (parser->line_starts_size == parser->line_starts_alloc) {
        bufsize_t alloc = parser->line_starts_alloc ? parser->line_starts_alloc * 2 : 128;
        bufsize_t *grown;
        if (parser->line_starts_alloc > (bufsize_t)(INT32_MAX / 2)) {
            parser->oom = true;
            return false;
        }
        grown = (bufsize_t *)parser->mem->realloc(parser->line_starts, (size_t)alloc * sizeof(bufsize_t));
        if (!grown) {
            parser->oom = true;
            return false;
        }
        parser->line_starts = grown;
        parser->line_starts_alloc = alloc;
    }
    parser->line_starts[parser->line_starts_size++] = start;
    return true;
}

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
    markdown_core_parser_touch(parser, parent);
    markdown_core_parser_touch(parser, child);

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
 * WHICH HALF A CACHE HIT SKIPS -- F15 rule 2, the resolution T18 owes. A pass
 * over the block's CHILDREN (consolidation, a hook declared `"*inlines"`, the
 * strip of inline comments) is skipped for a block whose children are
 * borrowed from the projection cache: the cache stored them after those
 * passes ran. A pass over the block NODE (a hook declared by name, the strip
 * of a comment `HTML_BLOCK`) runs on every projection, because the node is
 * the one part of a hit the cache never serves -- a `PARAGRAPH` around a
 * standalone formula is a fresh paragraph on every projection, and only the
 * hook makes it the `FormulaBlock` five gates pin. `holder` (T19) says which
 * kind of block this is; today nothing sets it and every block is its own. */

static const char S_INLINES_MEMBER[] = "*inlines";

static bool S_set_names(const char *set, const char *name) {
    const char *p;
    for (p = set; *p; p += strlen(p) + 1) {
        if (strcmp(p, name) == 0) {
            return true;
        }
    }
    return false;
}

/* The memo is keyed on the name's POINTER, and the name is a function of the
 * NODE rather than of its type: a `LIST_ITEM` carrying tasklist answers
 * "tasklist" and a plain one "list_item"; a `TABLE_ROW` answers
 * "table_header" or "table_row". Keyed on the type both would be wrong; keyed
 * on the name there is nothing to special-case. A literal with the same bytes
 * at another address misses once and takes its own entry. */
static bool S_extension_names(
    markdown_core_parser *parser,
    const markdown_core_syntax_extension *ext,
    const char *name
) {
    size_t i;
    bool wants;
    for (i = 0; i < parser->tail_memo_size; i++) {
        if (parser->tail_memo[i].ext == ext && parser->tail_memo[i].name == name) {
            return parser->tail_memo[i].wants;
        }
    }
    wants = S_set_names(ext->postprocess_blocks, name);
    if (parser->tail_memo_size < MARKDOWN_CORE_TAIL_MEMO) {
        parser->tail_memo[parser->tail_memo_size].ext = ext;
        parser->tail_memo[parser->tail_memo_size].name = name;
        parser->tail_memo[parser->tail_memo_size].wants = wants;
        parser->tail_memo_size++;
    }
    return wants;
}

/* Is `block` offered to `ext` -- by its inline content, when the children are
 * the block's own, or by the name it answers NOW? "Now", because a hook that
 * ran before this one may have replaced it, and the next extension must be
 * matched against the node that stands there (F15 rule 1). */
static bool S_extension_wants(
    markdown_core_parser *parser,
    const markdown_core_syntax_extension *ext,
    markdown_core_node *block,
    bool children_own
) {
    if (!ext->postprocess_block_func || !ext->postprocess_blocks) {
        return false;
    }
    if (children_own && contains_inlines(block) && S_extension_names(parser, ext, S_INLINES_MEMBER)) {
        return true;
    }
    return S_extension_names(parser, ext, markdown_core_node_get_type_string(block));
}

/* Asked at the block's EXIT, so a block nothing will touch is never queued. */
static bool S_block_has_tail_work(markdown_core_parser *parser, markdown_core_node *block) {
    markdown_core_llist *extensions;
    if (contains_inlines(block)) {
        return true;
    }
    if ((parser->options & MARKDOWN_CORE_OPT_STRIP_HTML_COMMENTS) && S_type(block) == MARKDOWN_CORE_NODE_HTML_BLOCK) {
        return true;
    }
    for (extensions = parser->syntax_extensions; extensions; extensions = extensions->next) {
        if (S_extension_wants(parser, (const markdown_core_syntax_extension *)extensions->data, block, true)) {
            return true;
        }
    }
    return false;
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

/* One block's tail. `*block` comes back reseated or NULL exactly as a hook
 * leaves it, so the caller can tell a replaced root from a removed one. */
static void S_run_block_tail(markdown_core_parser *parser, markdown_core_node **block) {
    markdown_core_llist *extensions;
    markdown_core_node *node = *block;
    bool children_own = node->holder == NULL;

    if (children_own && contains_inlines(node)) {
        if (!markdown_core_consolidate_text_nodes_with_parser(parser, node)) {
            parser->oom = true;
        }
    }

    for (extensions = parser->syntax_extensions; extensions; extensions = extensions->next) {
        const markdown_core_syntax_extension *ext = (const markdown_core_syntax_extension *)extensions->data;
        if (!S_extension_wants(parser, ext, node, children_own)) {
            continue;
        }
        ext->postprocess_block_func(ext, parser, &node);
        if (!node) {
            *block = NULL;
            return;
        }
        children_own = node->holder == NULL;
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
            if (contains_inlines(cur)) {
                markdown_core_parse_inlines(parser, cur, refmap, options);
            }
        } else if (MARKDOWN_CORE_NODE_BLOCK_P(cur) && S_block_has_tail_work(parser, cur)) {
            /* COLLECTED, NOT ACTED ON: a hook may replace or remove the block
             * and the walk is standing on it (F13 requirement 2). The walk
             * never descends into a block's inlines -- its lookahead was taken
             * before they were parsed -- so this EXIT follows the ENTER above
             * directly and the queue is the blocks in post-order. */
            if (!S_tail_queue_push(parser, cur)) {
                parser->oom = true;
            }
        }
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
 * parser, which outlives the derivation). `user_data` is deliberately not
 * copied -- it is caller-owned decoration on a returned tree, and the parse's
 * own CST never carries any. */
static markdown_core_node *S_clone_block_node(markdown_core_parser *parser, const markdown_core_node *src) {
    markdown_core_mem *mem = parser->mem;
    markdown_core_node *dst = (markdown_core_node *)mem->calloc(1, sizeof(*dst));
    if (!dst) {
        return NULL;
    }
    markdown_core_strbuf_init(mem, &dst->content, 0);
    if (src->content.size) {
        markdown_core_strbuf_put(&dst->content, src->content.ptr, src->content.size);
        if (dst->content.oom) {
            markdown_core_strbuf_free(&dst->content);
            mem->free(dst);
            return NULL;
        }
    }
    dst->start_line = src->start_line;
    dst->start_column = src->start_column;
    dst->end_line = src->end_line;
    dst->end_column = src->end_column;
    dst->internal_offset = src->internal_offset;
    dst->content_mark = src->content_mark;
    dst->content_mark_count = src->content_mark_count;
    dst->type = src->type;
    dst->flags = src->flags;
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
        if (!S_chunk_copy(mem, &dst->as.code.literal, &src->as.code.literal) ||
            !S_optional_chunk_copy(mem, &dst->as.code.info, &src->as.code.info)) {
            goto lost;
        }
        break;
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
        /* The union arm depends on the block's life stage: open, it holds the
         * matched block TYPE; closed, `finalize` detached the content into the
         * literal. The flag this engine now maintains says which (§12.8 Q3). */
        if (dst->flags & MARKDOWN_CORE_NODE__OPEN) {
            dst->as.html_block_type = src->as.html_block_type;
        } else if (!S_chunk_copy(mem, &dst->as.literal, &src->as.literal)) {
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
    return dst;

lost:
    markdown_core_node_free(dst);
    return NULL;
}

/* Clone the block skeleton. Iterative, because block nesting is input-shaped.
 * Children are linked raw: the source tree already proved containment. */
static markdown_core_node *S_clone_block_tree(markdown_core_parser *parser, const markdown_core_node *src_root) {
    markdown_core_node *dst_root = S_clone_block_node(parser, src_root);
    markdown_core_node *dst_parent = dst_root;
    const markdown_core_node *src = src_root->first_child;

    if (!dst_root) {
        return NULL;
    }
    while (src) {
        markdown_core_node *dst = S_clone_block_node(parser, src);
        if (!dst) {
            markdown_core_node_free(dst_root);
            return NULL;
        }
        dst->parent = dst_parent;
        dst->prev = dst_parent->last_child;
        if (dst_parent->last_child) {
            dst_parent->last_child->next = dst;
        } else {
            dst_parent->first_child = dst;
        }
        dst_parent->last_child = dst;

        if (src->first_child) {
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
 * the CST itself.
 *
 * `record_diagnostics` gates the rows the projection itself raises (after
 * §12.9 that is `label-too-long` and the directive attribute/label codes).
 * They are CST facts read off the construct's own bytes, but the rule is that
 * a diagnostic speaks when its construct COMPLETES (§12.8 Q4) -- so only the
 * final projection, over a fully closed CST, records; one taken mid-parse
 * stays silent rather than describing an open block's prefix. */
static markdown_core_node *S_project(
    markdown_core_parser *parser,
    markdown_core_node *skeleton,
    markdown_core_map *refmap,
    int record_diagnostics
) {
    bool recording = parser->diagnostics_on;

    parser->diagnostics_on = recording && record_diagnostics != 0;
    process_inlines(parser, skeleton, refmap, parser->options);
    parser->diagnostics_on = recording;

    S_run_block_tails(parser, &skeleton);

    return skeleton;
}

/* THE PROJECTION: AST = project(CST, refmap) (§12.1), as a NEW tree. Clone
 * the block skeleton and project onto the clone. The CST is not written: a
 * later derivation, against this map or another, starts from the same bytes.
 * This is the RE-projection path -- `finish`, whose CST has no later, skips
 * the clone and projects in place (T1). */
markdown_core_node *markdown_core_parser_derive_tree(
    markdown_core_parser *parser,
    markdown_core_map *refmap,
    int record_diagnostics
) {
    markdown_core_node *derived;

    /* A parse that lost an allocation may hold a tree that is not all there --
     * the sweep's witness is a footnote definition whose label still borrows a
     * freed temporary -- and `finish` answers NULL for it regardless, so there
     * is nothing to derive. */
    if (parser->oom) {
        return NULL;
    }

    derived = S_clone_block_tree(parser, parser->root);
    if (!derived) {
        parser->oom = true;
        return NULL;
    }

    return S_project(parser, derived, refmap, record_diagnostics);
}

markdown_core_node *markdown_core_parse_file(FILE *f, int options) {
    unsigned char buffer[4096];
    markdown_core_parser *parser = markdown_core_parser_new(options);
    size_t bytes;
    markdown_core_node *document;

    while ((bytes = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        bool eof = bytes < sizeof(buffer);
        S_parser_feed(parser, buffer, bytes, eof);
        if (eof) {
            break;
        }
    }

    document = markdown_core_parser_finish(parser);
    markdown_core_parser_free(parser);
    return document;
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

void markdown_core_parser_feed_reentrant(markdown_core_parser *parser, const char *buffer, size_t len) {
    markdown_core_strbuf saved_linebuf;

    markdown_core_strbuf_init(parser->mem, &saved_linebuf, 0);
    markdown_core_strbuf_puts(&saved_linebuf, markdown_core_strbuf_cstr(&parser->linebuf));
    markdown_core_strbuf_clear(&parser->linebuf);

    S_parser_feed(parser, (const unsigned char *)buffer, len, true);

    markdown_core_strbuf_sets(&parser->linebuf, markdown_core_strbuf_cstr(&saved_linebuf));
    markdown_core_strbuf_free(&saved_linebuf);
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
    markdown_core_node *current;

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

    /* The line joins the normalized source HERE, before anything reads it, so
     * the source is complete for lines 1..N the moment line N has been fed --
     * which is requirement 11a's L4 and the reason nothing about the record
     * set may be built at close. */
    if (!S_record_line_start(parser, parser->source.size)) {
        return;
    }
    markdown_core_strbuf_put(&parser->source, parser->curline.ptr, parser->curline.size);
    if (parser->source.oom) {
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

    current = parser->current;

    open_new_blocks(parser, &container, &input, all_matched);

    if (container == NULL || parser->oom) {
        goto finished;
    }

    /* parser->current might have changed if feed_reentrant was called */
    if (current == parser->current) {
        add_text_to_container(parser, container, last_matched_container, &input);
    }

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
        res = S_project(parser, parser->root, parser->refmap, 1);
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

    if (parser->concrete_out) {
        S_write_concrete(parser, parser->concrete_out);
    }

    /* REQUIREMENT 12: the document keeps the concrete view. Moved rather than
     * copied -- the parser is about to reset and would free all three -- and
     * moved HERE, after every rewrite and before the reset, which is the only
     * moment at which the view is both complete and still owned. */
    if (parser->concrete_retain) {
        markdown_core_concrete *out = parser->concrete_retain;
        out->mem = parser->mem;
        out->source = parser->source;
        out->line_starts = parser->line_starts;
        out->line_starts_size = parser->line_starts_size;
        markdown_core_strbuf_init(parser->mem, &parser->source, 0);
        parser->line_starts = NULL;
        parser->line_starts_size = 0;
        parser->line_starts_alloc = 0;
    }

    /* REQUIREMENT 13: the document keeps the diagnostic list, moved at the
     * same moment and for the same reason as the concrete view -- and, like
     * it, only on the success path. Everything above this line has already
     * decided that there IS a document; a parse failure falls out at the `oom`
     * test and leaves the caller's list empty, which is the requirement's
     * converse said in code. */
    if (parser->diagnostics_retain) {
        *parser->diagnostics_retain = parser->diagnostics;
        memset(&parser->diagnostics, 0, sizeof(parser->diagnostics));
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

void markdown_core_parser_retain_concrete(markdown_core_parser *parser, markdown_core_concrete *out) {
    if (!parser || !out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    parser->concrete_retain = out;
}

void markdown_core_concrete_dispose(markdown_core_concrete *concrete) {
    if (!concrete || !concrete->mem) {
        return;
    }
    markdown_core_strbuf_free(&concrete->source);
    concrete->mem->free(concrete->line_starts);
    memset(concrete, 0, sizeof(*concrete));
}

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

void markdown_core_parser_set_backslash_ispunct_func(markdown_core_parser *parser, markdown_core_ispunct_func func) {
    parser->backslash_ispunct = func;
}

markdown_core_llist *markdown_core_parser_get_syntax_extensions(markdown_core_parser *parser) {
    return parser->syntax_extensions;
}
