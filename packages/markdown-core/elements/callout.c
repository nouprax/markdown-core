#include "callout.h"
#include "block_internal.h"
#include "parts.h"
#include "sink.h"

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

/* Whether a `[!` at `open` has a variant the line's end cuts short. */
static bool S_variant_cut(markdown_core_reader *reader, bufsize_t open) {
    bufsize_t end = markdown_core_reader_length(reader);
    while (end > open && markdown_core_is_line_end(markdown_core_reader_peek(reader, end - 1))) {
        end--;
    }
    return end - open >= 2 && markdown_core_reader_peek(reader, open) == '[' &&
           markdown_core_reader_peek(reader, open + 1) == '!' &&
           open + 2 + variant_end(markdown_core_reader_bytes(reader, open + 2), end - open - 2) == end;
}

/* The marker's motion: past the `>` and the space or tab column after it. */
static void S_callout_marker(markdown_core_reader *reader) {
    markdown_core_reader_advance(reader, reader->nonspace + 1 - reader->offset, false);
    if (markdown_core_is_space_or_tab(markdown_core_reader_peek(reader, reader->offset))) {
        markdown_core_reader_advance(reader, 1, true);
    }
}

/* The header after the marker, from the reader's offset: up to three
 * spaces, then `[!variant]`, an optional fold and a title to the line's
 * end, which it reads. */
static void S_callout_header(markdown_core_callout_work *counts, markdown_core_parser *parser,
                             markdown_core_reader *reader, markdown_core_callout_header *header) {
    *header = (markdown_core_callout_header){.report = -1};
    bufsize_t pos = reader->offset;
    bufsize_t begin = pos;
    while (markdown_core_reader_peek(reader, pos) == ' ' && pos - begin < 3) {
        pos++;
        counts->scan++;
    }
    counts->scan++;
    if (parser->sink && !reader->split && S_variant_cut(reader, pos)) {
        header->report = pos;
    }
    if (reader->split || markdown_core_reader_peek(reader, pos) != '[' ||
        markdown_core_reader_peek(reader, pos + 1) != '!' || markdown_core_reader_ends(reader, pos + 2)) {
        return;
    }
    pos += 2;
    begin = pos;
    while (!markdown_core_reader_ends(reader, pos)) {
        unsigned char c = markdown_core_reader_peek(reader, pos);
        counts->scan++;
        if (!variant_byte(c)) {
            break;
        }
        pos++;
    }
    if (pos == begin || markdown_core_reader_ends(reader, pos) || markdown_core_reader_peek(reader, pos) != ']') {
        return;
    }
    header->variant = begin;
    header->variant_end = pos;
    pos++;
    header->fold = markdown_core_reader_peek(reader, pos) == '+' || markdown_core_reader_peek(reader, pos) == '-';
    header->collapsed = header->fold && markdown_core_reader_peek(reader, pos) == '-';
    pos += header->fold;
    if (!markdown_core_reader_ends(reader, pos) &&
        !markdown_core_is_whitespace(markdown_core_reader_peek(reader, pos))) {
        return;
    }
    while (markdown_core_is_space_or_tab(markdown_core_reader_peek(reader, pos))) {
        pos++;
        counts->scan++;
    }
    bufsize_t end = markdown_core_reader_length(reader);
    while (end > pos && markdown_core_is_whitespace(markdown_core_reader_peek(reader, end - 1))) {
        end--;
        counts->scan++;
    }
    header->title = pos;
    header->title_end = end;
    header->metadata = true;
}

/* Builds the header the scan found on the callout `node`. */
static void S_callout_build(markdown_core_parser *parser, markdown_core_node *node, markdown_core_reader *reader,
                            const markdown_core_callout_header *header) {
    markdown_core_chunk variant = {(unsigned char *)markdown_core_reader_bytes(reader, header->variant),
                                   header->variant_end - header->variant, 0};
    if (!markdown_core_chunk_to_cstr(&variant)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    node->as.callout->variant = markdown_core_optional_chunk_present(variant);
    node->as.callout->collapsed = (markdown_core_optional_bool){header->fold, header->collapsed};
    const bufsize_t pos = header->title, end = header->title_end;
    if (end > pos) {
        markdown_core_node *title = markdown_core_parser_make_node(parser, MARKDOWN_CORE_NODE_PARAGRAPH);
        if (!title) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return;
        }
        node->as.callout->title = title;
        title->place = markdown_core_reader_span(reader, parser, pos, end);
        markdown_core_strbuf_put(&title->content, markdown_core_reader_bytes(reader, pos), end - pos);
        if (title->content.oom || !markdown_core_reader_marks(reader, parser, title, pos, title->content.size, 0)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
        markdown_core_block_settle_field(parser, title, node);
    }
    markdown_core_reader_end(reader);
}

/* A callout's open builds what its scan read: the marker's motion, the
 * report of a variant cut short, and the header, after which the line is
 * the header's. */
static bool markdown_core_callout_open(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                       markdown_core_node **container, markdown_core_reader *reader,
                                       block_start *start) {
    (void)self;
    const markdown_core_callout_header *header = &start->callout;
    bufsize_t blockquote_startpos = reader->nonspace;
    markdown_core_reader_back(reader, header->after);
    *container =
        markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_CALLOUT, blockquote_startpos + 1);
    if (!*container) {
        return false;
    }
    if (header->report >= 0) {
        const markdown_core_field variant = {MARKDOWN_CORE_SLOT_VARIANT,
                                             MARKDOWN_CORE_KIND_CALLOUT,
                                             header->report + 2,
                                             variant_end,
                                             {{0}},
                                             0,
                                             false,
                                             0};
        markdown_core_parser_open(parser, MARKDOWN_CORE_KIND_CALLOUT, header->report, header->report + 2, &variant, 1);
    }
    if (header->metadata) {
        S_callout_build(parser, *container, reader, header);
        return false;
    }
    return true;
}

static bool markdown_core_callout_scan(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                       block_start_context *context, block_start *start) {
    (void)self;
    markdown_core_reader *reader = context->reader;
    int first = reader->nonspace;
    if (!(markdown_core_reader_peek(reader, first) == '>')) {
        return false;
    }
    /* What the open reads, read here: the marker's motion and the header
     * after it, from where the reader comes back to the marker. */
    const markdown_core_reader_mark mark = markdown_core_reader_tell(reader);
    S_callout_marker(reader);
    const markdown_core_reader_mark after = markdown_core_reader_tell(reader);
    S_callout_header(self->state, parser, reader, &start->callout);
    start->callout.after = after;
    markdown_core_reader_back(reader, mark);
    start->kind = MARKDOWN_CORE_NODE_CALLOUT;
    start->open = markdown_core_callout_open;
    return true;
}

static bool accepts_lazy(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         markdown_core_node *node, markdown_core_reader *reader) {
    (void)self;
    return node->kind == MARKDOWN_CORE_NODE_CALLOUT && node->as.callout->variant.has_value &&
           !markdown_core_parser_last_child(node) &&
           markdown_core_reader_starts(reader, parser, node, reader->number - 1);
}

static bool continue_container(const markdown_core_element_instance *self, markdown_core_parser *parser,
                               markdown_core_node *node, markdown_core_reader *reader,
                               const markdown_core_node *joining, bool *taken) {
    (void)self;
    return markdown_core_block_parse_callout_prefix(reader);
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
    .scan_needs = {.bytes = ">"},
};

static const markdown_core_line_ops CALLOUT_LINES = {
    .lazy = accepts_lazy,
    .lazy_open = open_lazy,
    .prefix = continue_container,
    .prefix_bytes = ">",
    .blank_opaque = true,
};

static const markdown_core_partition CALLOUT_PARTS[] = {
    {MARKDOWN_CORE_KIND_CALLOUT, markdown_core_callout_parts},
    {MARKDOWN_CORE_KIND_NONE, NULL},
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_CALLOUT = {
    .parts = CALLOUT_PARTS,
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
