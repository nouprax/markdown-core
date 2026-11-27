#include "callout.h"
#include "block_internal.h"

/* What a callout's scan found after its marker (WHAT THE SCAN FOUND,
 * block_internal.h): a header `[!variant]`, its fold and where its title
 * starts, and where a variant the line's end cuts short stands, -1 for
 * none, which its open reports. */
typedef struct {
    bufsize_t variant, variant_end, title, report, stop;
    bool metadata, fold, collapsed;
    /* Where the marker is. */
    bufsize_t marker;
} markdown_core_callout_header;
MARKDOWN_CORE_BLOCK_FOUND_FITS(markdown_core_callout_header, callout_header_fits);

static bool variant_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

/* Builds the header the start read on the callout `node`. */
static void S_callout_build(markdown_core_parser *parser, markdown_core_node *node, markdown_core_reader *reader,
                            const unsigned char *line, const markdown_core_callout_header *header) {
    markdown_core_chunk variant = {(unsigned char *)line + header->variant, header->variant_end - header->variant, 0};
    if (!markdown_core_chunk_to_cstr(&variant)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    node->as.callout->variant = markdown_core_optional_chunk_present(variant);
    node->as.callout->collapsed = (markdown_core_optional_bool){header->fold, header->collapsed};
    const markdown_core_field_to fields = {parser, node, NULL, reader, 0, NULL};
    markdown_core_field_put(&fields, MARKDOWN_CORE_SLOT_VARIANT, MARKDOWN_CORE_FIELD, header->variant,
                            header->variant_end, true);
}

/* Where a callout holds the title its text made (element.h,
 * markdown_core_edit_ops). */
static markdown_core_node **callout_title(markdown_core_node *node) { return &node->as.callout->title; }

/* A callout's open builds what its start read: the report of a variant
 * cut short, and the header, after which the line is the header's. */
static bool markdown_core_callout_open(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                       markdown_core_node **container, markdown_core_reader *reader,
                                       const unsigned char *line, block_start *start) {
    (void)self;
    const markdown_core_callout_header *header = MARKDOWN_CORE_BLOCK_FOUND(start, markdown_core_callout_header);
    bufsize_t blockquote_startpos = header->marker;
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
        S_callout_build(parser, *container, reader, line, header);
        return false;
    }
    return true;
}

enum { CALLOUT_OPEN, CALLOUT_TITLE, CALLOUT_BLOCKS };

/* THE TEXT OF ITS LINE (element.h, `text`): the text of the line a callout
 * with a variant opened on, past its header, is its title, code by code to
 * the line's ending, one token; the title is its bytes without the
 * whitespace at their end. A callout the edit reopens inside the line
 * makes it anew: the engine let the old one go. */
static markdown_core_state S_title_text(markdown_core_tokenizer *tokenizer, int code) {
    if (code == MARKDOWN_CORE_EOF || markdown_core_code_ending(code)) {
        if (code == MARKDOWN_CORE_EOF) {
            markdown_core_met_end(tokenizer);
        }
        markdown_core_exit(tokenizer, markdown_core_block_token(tokenizer, CALLOUT_TITLE));
        return markdown_core_ok(tokenizer);
    }
    ((markdown_core_callout_work *)((const markdown_core_element_instance *)markdown_core_self(tokenizer))->state)
        ->scan++;
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_title_text);
}

static markdown_core_state S_title(markdown_core_tokenizer *tokenizer, int code) {
    const markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    if (!turn->node->as.callout->variant.has_value) {
        return markdown_core_nok(tokenizer);
    }
    /* The title reads from where the cursor stands. */
    markdown_core_cursor_move(tokenizer);
    code = markdown_core_code_at(tokenizer, 0);
    if (code == MARKDOWN_CORE_EOF || markdown_core_code_ending(code)) {
        return markdown_core_ok(tokenizer);
    }
    markdown_core_enter(tokenizer, markdown_core_block_token(tokenizer, CALLOUT_TITLE));
    return S_title_text(tokenizer, code);
}

/* The title, made of its token's bytes without the whitespace at their
 * end. */
static void S_callout_title(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    markdown_core_block_turn *turn = markdown_core_block_turned(compiler);
    markdown_core_parser *parser = turn->parser;
    markdown_core_node *node = turn->node;
    markdown_core_reader *reader = turn->reader;
    const bufsize_t pos = (bufsize_t)(token->start - turn->base);
    const unsigned char *bytes = compiler->data + token->start;
    bufsize_t length = (bufsize_t)(token->end - token->start);
    while (length && markdown_core_is_whitespace(bytes[length - 1])) {
        length--;
    }
    if (!length) {
        return;
    }
    markdown_core_node *title = markdown_core_parser_make_node(parser, MARKDOWN_CORE_NODE_PARAGRAPH);
    if (!title) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    node->as.callout->title = title;
    title->place = markdown_core_reader_span(reader, parser, pos, pos + length);
    markdown_core_strbuf_put(&title->content, bytes, length);
    if (title->content.oom || !markdown_core_reader_marks(reader, parser, title, pos, title->content.size, 0)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    markdown_core_block_settle_field(parser, title, node);
}

static void S_callout_open(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)token;
    (void)index;
    markdown_core_block_turn *turn = markdown_core_block_turned(compiler);
    turn->opened = markdown_core_callout_open(markdown_core_compile_self(compiler), turn->parser, turn->container,
                                              turn->context->reader, compiler->data + turn->base, turn->start);
}

/* THE MARKER (micromark's blockQuote, code by code): up to three spaces,
 * `>`, and one space or tab column after it, the start's and, as a
 * prefix, every later line's. */
enum { MARK_AT };
static markdown_core_state S_start_space(markdown_core_tokenizer *tokenizer, int code);

/* THE HEADER after the marker: up to three spaces, `[!variant]`, a fold,
 * and the spaces before the title, read into the start's record as far as
 * it goes, which a variant cut short reports. A tab the marker split
 * starts none. */
static inline bufsize_t S_offset(const markdown_core_tokenizer *tokenizer) {
    return (bufsize_t)(tokenizer->position - tokenizer->base);
}

static inline markdown_core_callout_header *S_header(const markdown_core_tokenizer *tokenizer) {
    return MARKDOWN_CORE_BLOCK_FOUND(markdown_core_block_turn_of(tokenizer)->start, markdown_core_callout_header);
}

static inline void S_scan(markdown_core_tokenizer *tokenizer) {
    ((markdown_core_callout_work *)((const markdown_core_element_instance *)markdown_core_self(tokenizer))->state)
        ->scan++;
}

static markdown_core_state S_title_spaces(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_callout_header *header = S_header(tokenizer);
    if (code == ' ' || code == '\t' || code == MARKDOWN_CORE_HT || code == MARKDOWN_CORE_VS) {
        S_scan(tokenizer);
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_title_spaces);
    }
    header->title = S_offset(tokenizer);
    header->metadata = true;
    header->report = -1;
    return markdown_core_ok(tokenizer);
}

static markdown_core_state S_fold_after(markdown_core_tokenizer *tokenizer, int code) {
    if (!markdown_core_code_ending(code) && !markdown_core_code_space(code)) {
        return markdown_core_nok(tokenizer);
    }
    return S_title_spaces(tokenizer, code);
}

static markdown_core_state S_variant_after(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_callout_header *header = S_header(tokenizer);
    if (code == '+' || code == '-') {
        header->fold = true;
        header->collapsed = code == '-';
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_fold_after);
    }
    return S_fold_after(tokenizer, code);
}

static markdown_core_state S_variant(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_callout_header *header = S_header(tokenizer);
    S_scan(tokenizer);
    if (code >= 0 && code < 0x80 && variant_byte((unsigned char)code)) {
        markdown_core_consume(tokenizer);
        header->stop = S_offset(tokenizer);
        return markdown_core_state_of(S_variant);
    }
    if (code != ']' || header->stop == header->report + 2) {
        return markdown_core_nok(tokenizer);
    }
    header->variant = header->report + 2;
    header->variant_end = header->stop;
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_variant_after);
}

static markdown_core_state S_bang(markdown_core_tokenizer *tokenizer, int code) {
    if (code != '!') {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_callout_header *header = S_header(tokenizer);
    header->report = S_offset(tokenizer) - 1;
    header->stop = header->report + 2;
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_variant);
}

static markdown_core_state S_header_open(markdown_core_tokenizer *tokenizer, int code) {
    S_scan(tokenizer);
    if (code != '[') {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_bang);
}

static markdown_core_state S_header_start(markdown_core_tokenizer *tokenizer, int code) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    if (code == ' ' && words[MARK_AT] < 3) {
        S_scan(tokenizer);
        words[MARK_AT]++;
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_header_start);
    }
    return S_header_open(tokenizer, code);
}

static const markdown_core_construct CALLOUT_HEADER = {.start = S_header_start};

static markdown_core_state S_opened(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    markdown_core_exit(tokenizer, markdown_core_block_token(tokenizer, CALLOUT_OPEN));
    return markdown_core_ok(tokenizer);
}

static markdown_core_state S_start_marked(markdown_core_tokenizer *tokenizer, int code) {
    if (code == MARKDOWN_CORE_VS) {
        return S_opened(tokenizer, code);
    }
    return markdown_core_attempt(tokenizer, &CALLOUT_HEADER, markdown_core_state_of(S_opened),
                                 markdown_core_state_of(S_opened), code);
}

static markdown_core_state S_start_marker(markdown_core_tokenizer *tokenizer, int code) {
    if (code != '>') {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_callout_header *header = S_header(tokenizer);
    *header = (markdown_core_callout_header){.report = -1, .marker = S_offset(tokenizer)};
    markdown_core_enter(tokenizer, markdown_core_block_token(tokenizer, CALLOUT_OPEN));
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_start_space);
}

static markdown_core_state S_start_space(markdown_core_tokenizer *tokenizer, int code) {
    if (markdown_core_code_space(code)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_start_marked);
    }
    return S_start_marked(tokenizer, code);
}

static markdown_core_state markdown_core_callout_scan(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_block_turn_of(tokenizer)->start->kind = MARKDOWN_CORE_NODE_CALLOUT;
    return markdown_core_spaces(tokenizer, markdown_core_state_of(S_start_marker), MARKDOWN_CORE_TOKEN_SPACE, 3, code);
}

/* The marker line's text is the callout's title, so a lazy line after it
 * cannot continue a paragraph: it starts the body's first one, as the same
 * text would with the quote's prefix. */
static markdown_core_state S_lazy(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    const markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    const markdown_core_node *node = turn->node;
    return node->kind == MARKDOWN_CORE_NODE_CALLOUT && node->as.callout->variant.has_value &&
                   !markdown_core_parser_last_child(node) &&
                   markdown_core_reader_starts(turn->reader, turn->parser, node, turn->reader->number - 1)
               ? markdown_core_ok(tokenizer)
               : markdown_core_nok(tokenizer);
}

static const markdown_core_edit_ops CALLOUT_EDIT = {
    .reopens = true,
    .text = callout_title,
};

static const markdown_core_block_construct CALLOUT_STARTS[] = {
    {.bytes = ">", .indent = 3, .construct = {.start = markdown_core_callout_scan}},
};

static const markdown_core_compile_ops CALLOUT_BLOCK_TOKENS[CALLOUT_BLOCKS] = {
    [CALLOUT_OPEN] = {.exit = S_callout_open},
    [CALLOUT_TITLE] = {.enter = S_callout_title},
};

static const markdown_core_open_ops CALLOUT_OPENS = {
    .starts = CALLOUT_STARTS,
    .start_count = 1,
    .tokens = CALLOUT_BLOCK_TOKENS,
    .token_count = CALLOUT_BLOCKS,
};

/* THE CONTINUATION (element.h, markdown_core_line_ops `continuation`): the
 * marker, as a prefix. */
static markdown_core_state S_prefix_end(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    markdown_core_exit(tokenizer, MARKDOWN_CORE_TOKEN_PREFIX);
    return markdown_core_ok(tokenizer);
}

static markdown_core_state S_prefix_space(markdown_core_tokenizer *tokenizer, int code) {
    if (markdown_core_code_space(code)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_prefix_end);
    }
    return S_prefix_end(tokenizer, code);
}

static markdown_core_state S_prefix_marker(markdown_core_tokenizer *tokenizer, int code) {
    if (code != '>') {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_prefix_space);
}

static markdown_core_state S_continue(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_enter(tokenizer, MARKDOWN_CORE_TOKEN_PREFIX);
    return markdown_core_spaces(tokenizer, markdown_core_state_of(S_prefix_marker), 0, 3, code);
}

static const markdown_core_line_ops CALLOUT_LINES = {
    .continuation = {.start = S_continue},
    .lazy = {.start = S_lazy},
    .prefix_bytes = ">",
    .blank_opaque = true,
    .text = {.start = S_title},
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
