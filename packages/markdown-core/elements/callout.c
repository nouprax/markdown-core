#include "callout.h"
#include "block_internal.h"

/* What a callout's scan found after its marker (WHAT THE SCAN FOUND,
 * block_internal.h): a header `[!variant]`, its fold and where its title
 * starts, and where a variant the line's end cuts short stands, -1 for
 * none, which its open reports. */
typedef struct {
    bufsize_t variant, variant_end, title, report, stop;
    bool metadata, fold, collapsed;
    /* Where the marker's motion leaves the reader. */
    markdown_core_reader_mark after;
} markdown_core_callout_header;
MARKDOWN_CORE_BLOCK_FOUND_FITS(markdown_core_callout_header, callout_header_fits);

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

/* The marker's motion: past the `>` and the space or tab column after it. */
static void S_callout_marker(markdown_core_reader *reader) {
    markdown_core_reader_advance(reader, reader->nonspace + 1 - reader->offset, false);
    if (markdown_core_is_space_or_tab(markdown_core_reader_peek(reader, reader->offset))) {
        markdown_core_reader_advance(reader, 1, true);
    }
}

/* The header after the marker, from the reader's offset: up to three
 * spaces, then `[!variant]`, an optional fold, and where a title would
 * start, which the line gives the callout as its text (THE TEXT OF ITS
 * LINE). */
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
    if (reader->split || markdown_core_reader_peek(reader, pos) != '[' ||
        markdown_core_reader_peek(reader, pos + 1) != '!') {
        return;
    }
    /* A header begun and given up keeps its variant as far as it is read
     * (markdown_core_callout_open). */
    header->report = pos;
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
    header->stop = pos;
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
    header->title = pos;
    header->metadata = true;
    header->report = -1;
}

/* Builds the header the scan found on the callout `node`, up to where its
 * title would start, where the reader goes. */
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
    const markdown_core_field_to fields = {parser, node, NULL, reader, 0, NULL};
    markdown_core_field_put(&fields, MARKDOWN_CORE_SLOT_VARIANT, MARKDOWN_CORE_FIELD, header->variant,
                            header->variant_end, true);
    markdown_core_reader_advance(reader, header->title - reader->offset, false);
}

/* THE TEXT OF ITS LINE (element.h, `text`): the text of the line a callout
 * with a variant opened on, past its header, is its title, to the line's
 * end and without the whitespace there. A callout the edit reopens inside
 * the line makes it anew: the engine let the old one go. */
static bool callout_text(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         markdown_core_node *node, markdown_core_reader *reader) {
    markdown_core_callout_work *counts = self->state;
    if (!node->as.callout->variant.has_value) {
        return false;
    }
    const bufsize_t pos = reader->offset;
    bufsize_t length;
    const unsigned char *text = markdown_core_reader_handed(reader, pos, &length);
    while (length && markdown_core_is_whitespace(text[length - 1])) {
        length--;
        counts->scan++;
    }
    if (!length) {
        return true;
    }
    markdown_core_node *title = markdown_core_parser_make_node(parser, MARKDOWN_CORE_NODE_PARAGRAPH);
    if (!title) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return true;
    }
    node->as.callout->title = title;
    title->place = markdown_core_reader_span(reader, parser, pos, pos + length);
    markdown_core_strbuf_put(&title->content, text, length);
    if (title->content.oom || !markdown_core_reader_marks(reader, parser, title, pos, title->content.size, 0)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    markdown_core_block_settle_field(parser, title, node);
    return true;
}

/* Where a callout holds the title its text made (element.h,
 * markdown_core_edit_ops). */
static markdown_core_node **callout_title(markdown_core_node *node) { return &node->as.callout->title; }

/* A callout's open builds what its scan read: the marker's motion, the
 * report of a variant cut short, and the header, after which the line is
 * the header's. */
static bool markdown_core_callout_open(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                       markdown_core_node **container, markdown_core_reader *reader,
                                       block_start *start) {
    (void)self;
    const markdown_core_callout_header *header = MARKDOWN_CORE_BLOCK_FOUND(start, markdown_core_callout_header);
    bufsize_t blockquote_startpos = reader->nonspace;
    markdown_core_reader_back(reader, header->after);
    *container =
        markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_CALLOUT, blockquote_startpos + 1);
    if (!*container) {
        return false;
    }
    if (header->report >= 0) {
        const markdown_core_field variant = {MARKDOWN_CORE_SLOT_VARIANT, MARKDOWN_CORE_KIND_CALLOUT, header->report + 2,
                                             header->stop};
        const markdown_core_field_to fields = {parser, *container, NULL, reader, 0, NULL};
        markdown_core_field_opener(&fields, MARKDOWN_CORE_KIND_CALLOUT, true, header->report, header->report + 2,
                                   &variant, 1);
    }
    if (header->metadata) {
        S_callout_build(parser, *container, reader, header);
        return false;
    }
    return true;
}

enum { CALLOUT_OPEN, CALLOUT_BLOCKS };

static void S_callout_open(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)token;
    (void)index;
    markdown_core_block_turn *turn = markdown_core_block_turned(compiler);
    turn->opened = markdown_core_callout_open(markdown_core_compile_self(compiler), turn->parser, turn->container,
                                              turn->context->reader, turn->start);
}

static markdown_core_state markdown_core_callout_scan(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    markdown_core_parser *parser = markdown_core_block_turn_of(tokenizer)->parser;
    block_start_context *context = markdown_core_block_turn_of(tokenizer)->context;
    block_start *start = markdown_core_block_turn_of(tokenizer)->start;
    markdown_core_reader *reader = context->reader;
    int first = reader->nonspace;
    if (!(markdown_core_reader_peek(reader, first) == '>')) {
        return markdown_core_nok(tokenizer);
    }
    /* What the open reads, read here: the marker's motion and the header
     * after it, from where the reader comes back to the marker. */
    const markdown_core_reader_mark mark = markdown_core_reader_tell(reader);
    S_callout_marker(reader);
    const markdown_core_reader_mark after = markdown_core_reader_tell(reader);
    S_callout_header(self->state, parser, reader, MARKDOWN_CORE_BLOCK_FOUND(start, markdown_core_callout_header));
    MARKDOWN_CORE_BLOCK_FOUND(start, markdown_core_callout_header)->after = after;
    markdown_core_reader_back(reader, mark);
    start->kind = MARKDOWN_CORE_NODE_CALLOUT;
    return markdown_core_block_found(tokenizer, CALLOUT_OPEN);
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

static const markdown_core_edit_ops CALLOUT_EDIT = {
    .reopens = true,
    .text = callout_title,
};

static const markdown_core_block_construct CALLOUT_STARTS[] = {
    {.bytes = ">", .indent = 3, .construct = {.start = markdown_core_callout_scan}},
};

static const markdown_core_compile_ops CALLOUT_BLOCK_TOKENS[CALLOUT_BLOCKS] = {
    [CALLOUT_OPEN] = {.enter = S_callout_open},
};

static const markdown_core_open_ops CALLOUT_OPENS = {
    .starts = CALLOUT_STARTS,
    .start_count = 1,
    .tokens = CALLOUT_BLOCK_TOKENS,
    .token_count = CALLOUT_BLOCKS,
};

/* THE CONTINUATION (element.h, markdown_core_line_ops `continuation`). */
static markdown_core_state S_continue(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    bool taken = false;
    if (!continue_container(markdown_core_self(tokenizer), turn->parser, turn->node, turn->reader, turn->joining,
                            &taken)) {
        return markdown_core_nok(tokenizer);
    }
    return taken ? markdown_core_block_continued(tokenizer, MARKDOWN_CORE_TOKEN_TAKE) : markdown_core_ok(tokenizer);
}

static const markdown_core_line_ops CALLOUT_LINES = {
    .continuation = {.start = S_continue},
    .lazy = accepts_lazy,
    .lazy_open = open_lazy,
    .prefix_bytes = ">",
    .blank_opaque = true,
    .text = callout_text,
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_CALLOUT = {
    .edit = &CALLOUT_EDIT,
    .state_size = sizeof(markdown_core_callout_work),
    .name = "callout",
    .opens = &CALLOUT_OPENS,
    .lines = &CALLOUT_LINES,
};

/* A callout's record (markdown_core_record_ops): its variant, and its title,
 * a field root. */
static void free_callout(markdown_core_node_pool *pool, markdown_core_node *node) {
    (void)pool;
    markdown_core_optional_chunk_free(&node->as.callout->variant);
}

static int visit_callout(markdown_core_node *node, markdown_core_owned_subtree_visitor visitor, void *context) {
    return !node->as.callout->title || visitor(&node->as.callout->title, context);
}

const markdown_core_record_ops MARKDOWN_CORE_RECORD_CALLOUT = {
    .size = sizeof(markdown_core_callout),
    .free = free_callout,
    .visit = visit_callout,
};
