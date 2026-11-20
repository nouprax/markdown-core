#include "callout.h"
#include "block_internal.h"
#include "parts.h"
#include "sink.h"

static bool markdown_core_block_parse_callout_metadata(markdown_core_callout_work *counts, markdown_core_parser *parser,
                                                       markdown_core_node *node, markdown_core_reader *reader);
static bool markdown_core_callout_scan(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                       block_start_context *context, block_start *start);
bool markdown_core_block_parse_callout_prefix(markdown_core_reader *reader) {
    bool res = false;
    bufsize_t matched = 0;

    matched = reader->indent <= 3 && markdown_core_reader_peek(reader, reader->nonspace) == '>';
    if (matched) {

        markdown_core_reader_advance(reader, reader->indent + 1, true);

        if (markdown_core_is_space_or_tab(markdown_core_reader_peek(reader, reader->offset))) {
            markdown_core_reader_advance(reader, 1, true);
        }

        res = true;
    }
    return res;
}

static bool variant_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

/* Where a variant goes on past a cursor. */
static bufsize_t variant_end(const unsigned char *data, bufsize_t length) {
    bufsize_t end = 0;
    while (end < length && variant_byte(data[end])) {
        end++;
    }
    return end;
}

/* A `[!` at `open` whose variant the line's end cuts short, to the parse's
 * context sink. */
static void S_report_variant(markdown_core_parser *parser, const markdown_core_reader *reader, bufsize_t open) {
    bufsize_t end = reader->line.len;
    while (end > open && markdown_core_is_line_end(reader->line.data[end - 1])) {
        end--;
    }
    if (end - open < 2 || reader->line.data[open] != '[' || reader->line.data[open + 1] != '!' ||
        open + 2 + variant_end(reader->line.data + open + 2, end - open - 2) != end) {
        return;
    }
    const markdown_core_field variant = {
        MARKDOWN_CORE_SLOT_VARIANT, MARKDOWN_CORE_KIND_CALLOUT, open + 2, variant_end, {{0}}, 0, false, 0};
    markdown_core_parser_open(parser, MARKDOWN_CORE_KIND_CALLOUT, open, open + 2, &variant, 1);
}

static bool markdown_core_block_parse_callout_metadata(markdown_core_callout_work *counts, markdown_core_parser *parser,
                                                       markdown_core_node *node, markdown_core_reader *reader) {
    bufsize_t pos = reader->offset;
    bufsize_t begin = pos;
    while (pos < reader->line.len && reader->line.data[pos] == ' ' && pos - begin < 3) {
        pos++;
        counts->scan++;
    }
    counts->scan++;
    if (parser->sink && !reader->split) {
        S_report_variant(parser, reader, pos);
    }
    if (reader->split || pos + 2 >= reader->line.len || reader->line.data[pos] != '[' ||
        reader->line.data[pos + 1] != '!') {
        return false;
    }
    pos += 2;
    begin = pos;
    while (pos < reader->line.len) {
        unsigned char c = reader->line.data[pos];
        counts->scan++;
        if (!variant_byte(c)) {
            break;
        }
        pos++;
    }
    if (pos == begin || pos >= reader->line.len || reader->line.data[pos] != ']') {
        return false;
    }
    markdown_core_chunk variant = {reader->line.data + begin, pos - begin, 0};
    pos++;
    bool has_fold = pos < reader->line.len && (reader->line.data[pos] == '+' || reader->line.data[pos] == '-');
    bool collapsed = has_fold && reader->line.data[pos] == '-';
    pos += has_fold;
    if (pos < reader->line.len && !markdown_core_is_whitespace(reader->line.data[pos])) {
        return false;
    }
    while (pos < reader->line.len && markdown_core_is_space_or_tab(reader->line.data[pos])) {
        pos++;
        counts->scan++;
    }
    bufsize_t end = reader->line.len;
    while (end > pos && markdown_core_is_whitespace(reader->line.data[end - 1])) {
        end--;
        counts->scan++;
    }
    if (!markdown_core_chunk_to_cstr(&variant)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return true;
    }
    node->as.callout->variant = markdown_core_optional_chunk_present(variant);
    node->as.callout->collapsed = (markdown_core_optional_bool){has_fold, collapsed};
    if (end > pos) {
        markdown_core_node *title = markdown_core_parser_make_node(parser, MARKDOWN_CORE_NODE_PARAGRAPH);
        if (!title) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return true;
        }
        node->as.callout->title = title;
        title->place =
            (markdown_core_place){(uint32_t)markdown_core_parser_source_offset(parser, reader->number, pos + 1),
                                  (uint32_t)markdown_core_parser_source_end(parser, reader->number, end)};
        markdown_core_strbuf_put(&title->content, reader->line.data + pos, end - pos);
        if (title->content.oom ||
            !markdown_core_parser_append_source_marks(parser, title, reader->number, pos + 1, title->content.size, 0)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
        markdown_core_block_settle_field(parser, title, node);
    }
    markdown_core_reader_advance(reader, reader->line.len - 1 - reader->offset, false);
    return true;
}

static bool markdown_core_callout_open(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                       markdown_core_node **container, markdown_core_reader *reader,
                                       block_start *start) {

    bufsize_t blockquote_startpos = reader->nonspace;

    markdown_core_reader_advance(reader, reader->nonspace + 1 - reader->offset, false);
    // optional following character
    if (markdown_core_is_space_or_tab(reader->line.data[reader->offset])) {
        markdown_core_reader_advance(reader, 1, true);
    }
    *container =
        markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_CALLOUT, blockquote_startpos + 1);
    if (!*container) {
        return false;
    }

    if (markdown_core_block_parse_callout_metadata(self->state, parser, *container, reader)) {
        return false;
    }

    return true;
}

static bool markdown_core_callout_scan(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                       block_start_context *context, block_start *start) {
    (void)self;
    markdown_core_reader *reader = context->reader;
    int first = reader->nonspace;
    if (!(reader->line.data[first] == '>')) {
        return false;
    }
    start->kind = MARKDOWN_CORE_NODE_CALLOUT;
    start->open = markdown_core_callout_open;
    return true;
}

static bool accepts_lazy(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         markdown_core_node *node, const markdown_core_reader *reader) {
    (void)self;
    return node->kind == MARKDOWN_CORE_NODE_CALLOUT && node->as.callout->variant.has_value &&
           !markdown_core_parser_last_child(node) &&
           markdown_core_parser_starts_on_line(parser, node, reader->number - 1);
}

static bool continue_container(const markdown_core_element_instance *self, markdown_core_parser *parser,
                               markdown_core_node *node, markdown_core_reader *reader,
                               const markdown_core_node *joining, bool *taken) {
    (void)self;
    return markdown_core_block_parse_callout_prefix(reader);
}
/* The prefix reads the first non-space byte. Its column-counted advance
 * over a `>` reads the byte after the `>`'s scalar, and takes it when it is
 * a space or a tab -- a space's scalar too (markdown_core_reader_scalar_end). */
static bufsize_t continue_reach(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                const markdown_core_node *node, const markdown_core_reader *reader) {
    (void)self;
    (void)node;
    const bufsize_t first = reader->nonspace;
    if (reader->indent > 3 || reader->line.data[first] != '>') {
        return first;
    }
    const bufsize_t after = markdown_core_reader_scalar_end(reader, first + 1);
    return reader->line.data[after] == ' ' ? markdown_core_reader_scalar_end(reader, after + 1) : after;
}
/* The scan reads the first non-space byte. The open reads the byte after
 * the `>`, which it takes when it is a space -- with the space's scalar
 * (markdown_core_reader_scalar_end) -- or one column of a tab; then, unless
 * that tab is only partly taken, up to three spaces and the two bytes after
 * them, which a metadata line opens with `[!`, and whose header reads to the
 * line's end (markdown_core_block_parse_callout_metadata). */
static bufsize_t start_reach(const markdown_core_element_instance *self, markdown_core_parser *parser,
                             const block_start_context *context, bool *opens) {
    (void)self;
    (void)parser;
    const markdown_core_reader *reader = context->reader;
    const bufsize_t end = reader->line.len - 1;
    bufsize_t at = reader->nonspace;
    if (reader->line.data[at] != '>') {
        return at;
    }
    *opens = true;
    at++;
    if (reader->line.data[at] == '\t') {
        if (TAB_STOP - (reader->nonspace_column + 1) % TAB_STOP > 1) {
            return at;
        }
        at++;
    } else if (reader->line.data[at] == ' ') {
        at = markdown_core_reader_scalar_end(reader, at + 1);
    }
    for (int spaces = 0; spaces < 3 && reader->line.data[at] == ' '; spaces++) {
        at++;
    }
    if (at + 1 >= end || (reader->line.data[at] == '[' && reader->line.data[at + 1] == '!')) {
        return end;
    }
    return at + 1;
}
/* The marker line's text is the callout's title, so a lazy line after it
 * cannot continue a paragraph: it starts the body's first one, as the same
 * text would with the quote's prefix. */
static markdown_core_node *open_lazy(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                     markdown_core_node *node, markdown_core_reader *reader) {
    (void)self;
    const markdown_core_element_instance *text_block = parser->dialect->text_block_structure;
    return text_block->element->opens->text(text_block, parser, node, reader);
}

static const markdown_core_node_type CALLOUT_TRAILED[] = {MARKDOWN_CORE_NODE_CALLOUT, MARKDOWN_CORE_NODE_NONE};

static const markdown_core_edit_ops CALLOUT_EDIT = {
    .reopens = true,
    /* A block identifier line after it names it (block_identifier.c). */
    .trailed_kinds = CALLOUT_TRAILED,
};

static const markdown_core_open_ops CALLOUT_OPENS = {
    .indent = 3,
    .scan = markdown_core_callout_scan,
    .reach = start_reach,
    .scan_needs = {.bytes = ">"},
};

static const markdown_core_line_ops CALLOUT_LINES = {
    .lazy = accepts_lazy,
    .lazy_open = open_lazy,
    .prefix = continue_container,
    .reach = continue_reach,
    .prefix_bytes = ">",
    .blank_opaque = true,
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_CALLOUT = {
    .edit = &CALLOUT_EDIT,
    .state_size = sizeof(markdown_core_callout_work),
    .name = "callout",
    .opens = &CALLOUT_OPENS,
    .lines = &CALLOUT_LINES,
};

/* A callout's parts: its variant, inside `[!` and `]` on its first line, and
 * a block identifier on the line after it. The fold sign is a marker. */
void markdown_core_callout_parts(markdown_core_parts *parts) {
    const unsigned char *text = parts->text;
    bufsize_t at = parts->node->as.callout->variant.has_value ? 0 : parts->length;
    while (at < parts->length && text[at] != '\n' && text[at] != '\r' &&
           !(text[at] == '[' && at + 1 < parts->length && text[at + 1] == '!')) {
        at++;
    }
    if (at + 1 < parts->length && text[at] == '[' && text[at + 1] == '!') {
        const bufsize_t end = at + 2 + variant_end(text + at + 2, parts->length - at - 2);
        if (end < parts->length && text[end] == ']') {
            markdown_core_parts_add(parts, MARKDOWN_CORE_SLOT_VARIANT, at + 2, end);
        }
    }
    markdown_core_block_identifier_parts(parts);
}
