#include "comment_scanners.h"
#include "comment.h"
#include "services.h"
#include "inline_internal.h"
#include "block_internal.h"
#include <string.h>
#include "element.h"
#include "node.h"
#include "parser.h"

/* THE `%%` COMMENT (O3), both of its forms, producing the `Comment` kind that
 * M0 gave the HTML comment. Nothing is stripped: a consumer that does not want
 * comments drops the nodes.
 *
 * INLINE, step A5 of the recognition order: `%%` at the cursor opens, and the
 * body runs to the first later `%%`, so `%%%%` is an empty comment and
 * `%%%a%%%` is the comment `%a` followed by the text `%`. The body is opaque,
 * which the scanner gets for free by consuming it: no later step and no other
 * module sees a byte of it. An escaped percent sign is the base language's
 * (step A1 runs first), so `\%%` is text. An opener with no closer is text and
 * hides nothing, and its failed search is cached under the comment's rule id,
 * so a run of percent signs that never closes costs one scan of its suffix and
 * not one per sign.
 *
 * BLOCK, step 4 of the block-start order: a line whose content after the
 * container prefixes is `%%` at indentation zero to three, followed only by
 * spaces or tabs, is a candidate, and it commits only if a later line whose
 * content is the same fence under the same prefixes closes it. That later line
 * is found by the parser's block-start lookahead before anything opens, so a
 * candidate with no closer consumes nothing: its line is paragraph text and
 * the inline rule applies to it. A committed block accepts every line until
 * the fence that closes it, interpreting no block syntax in between, and its
 * literal is those lines as written after container-prefix removal.
 *
 * The block form is the `%%` grammar's alone; the HTML comment block is the
 * inherited grammar's and never passes through here. */

static int is_fence_line(const unsigned char *data, int len, int first_nonspace) {
    int i = first_nonspace;

    if (i + 2 > len || data[i] != '%' || data[i + 1] != '%') {
        return 0;
    }
    return markdown_core_is_blank_to_line_end(data, i + 2, len);
}

/* THE STOP LINES (element.h, `stops`): a line whose content is the fence,
 * at any indentation, which takes in every line that closes a reading from
 * an opener in the document's root. */
static bool comment_stops(const markdown_core_element_instance *self, const unsigned char *line, size_t length) {
    (void)self;
    size_t i = 0;
    while (i < length && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    if (i + 2 > length || line[i] != '%' || line[i + 1] != '%') {
        return false;
    }
    for (i += 2; i < length; i++) {
        if (line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != '\n') {
            return false;
        }
    }
    return true;
}

/* THE OPENER: a fence line a later fence closes. */
enum { COMMENT_OPEN, COMMENT_SHUT, COMMENT_KINDS };

/* THE OPENER'S READING, a check of the document's reading: whether a later
 * line under the same prefixes is a fence line (markdown_core_look_next). */
static markdown_core_state S_closer_line(markdown_core_tokenizer *tokenizer, int code);

static markdown_core_state S_closer_none(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return markdown_core_nok(tokenizer);
}

static markdown_core_state S_closer_next(markdown_core_tokenizer *tokenizer, int code) {
    return markdown_core_look_next(tokenizer, markdown_core_state_of(S_closer_line),
                                   markdown_core_state_of(S_closer_none), code);
}

static markdown_core_state S_closer_line(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_parser *parser = markdown_core_block_turn_of(tokenizer)->parser;
    markdown_core_reader *line = &parser->look->read;
    if (line->indent < 4 &&
        is_fence_line(markdown_core_reader_rest(line, 0), markdown_core_reader_length(line), line->nonspace)) {
        return parser->error ? markdown_core_nok(tokenizer) : markdown_core_ok(tokenizer);
    }
    return S_closer_next(tokenizer, code);
}

static const markdown_core_construct COMMENT_CLOSER = {.start = S_closer_next};

/* The reading ends, the reader back on the fence line. */
static markdown_core_state S_read(markdown_core_tokenizer *tokenizer, bool closed) {
    markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    markdown_core_parser *parser = turn->parser;
    if (!closed || parser->error) {
        return markdown_core_nok(tokenizer);
    }
    return markdown_core_block_found(tokenizer, COMMENT_OPEN);
}

static markdown_core_state S_closed(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return S_read(tokenizer, true);
}

static markdown_core_state S_unclosed(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return S_read(tokenizer, false);
}

static markdown_core_state S_start(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    markdown_core_reader *reader = turn->context->reader;
    if (reader->indent >= 4 ||
        !is_fence_line(markdown_core_reader_rest(reader, 0), markdown_core_reader_length(reader), reader->nonspace)) {
        return markdown_core_nok(tokenizer);
    }
    const markdown_core_look_ask ask = {.container = turn->context->container,
                                        .kind = MARKDOWN_CORE_NODE_COMMENT_BLOCK,
                                        .construct = &COMMENT_CLOSER,
                                        .opener = markdown_core_self(tokenizer)};
    return markdown_core_look(tokenizer, &ask, markdown_core_state_of(S_closed), markdown_core_state_of(S_unclosed),
                              code);
}

static void S_open(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)token;
    (void)index;
    const markdown_core_element_instance *self = markdown_core_compile_self(compiler);
    markdown_core_block_turn *turn = markdown_core_block_turned(compiler);
    markdown_core_parser *parser = turn->parser;
    markdown_core_reader *reader = turn->context->reader;
    int first_nonspace = reader->nonspace;
    markdown_core_node *node =
        markdown_core_parser_add_child(parser, *turn->container, MARKDOWN_CORE_NODE_COMMENT_BLOCK, first_nonspace + 1);
    if (!node) {
        return;
    }
    markdown_core_node_set_element(node, self->element);
    /* Its literal is found in its text past the opening `%%`. */
    const markdown_core_field_to fields = {parser, node, NULL, reader, 0, NULL};
    markdown_core_field_put(&fields, MARKDOWN_CORE_SLOT_NONE, MARKDOWN_CORE_FIELD_FOUND, first_nonspace + 2,
                            first_nonspace + 2, true);
    /* The rest of the fence line, spaces and its ending, is the first line
     * of the block's content, as a fence's rest is a code block's (blocks.c,
     * THE LEAP), and no part of its literal. */
    markdown_core_reader_advance(reader, first_nonspace + 2 - reader->offset, false);
    *turn->container = node;
    turn->opened = true;
}

/* Every line reaches the open block until its closer; the closer line closes
 * it and is not added. The lookahead saw this closer before the block opened,
 * through the same container matchers, so an open block always finds it. */
static int block_matches(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         markdown_core_reader *reader, markdown_core_node *container) {
    if (reader->indent <= 3 &&
        is_fence_line(markdown_core_reader_rest(reader, 0), markdown_core_reader_length(reader), reader->nonspace)) {
        return MARKDOWN_CORE_TOKEN_CLOSE;
    }
    return 1;
}

/* `%` ends a text run and is offered to the scanner, and that is the whole set. */
static void close_comment(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index);

/* An HTML block of type 2 that its own end condition closed and that
 * holds one comment became a CommentBlock as it closed
 * (markdown_core_block_convert_comment_block): it read its lines as an
 * HTML block (element.h, THE FORMER KIND). A `%%` block read them as
 * itself. */
enum { MARKDOWN_CORE_NODE__COMMENT_HTML = MARKDOWN_CORE_NODE__ELEMENT_FIRST };

static markdown_core_node_type comment_former(const markdown_core_element_instance *self,
                                              const markdown_core_node *node) {
    (void)self;
    return node->kind == MARKDOWN_CORE_NODE_COMMENT_BLOCK && (node->flags & MARKDOWN_CORE_NODE__COMMENT_HTML)
               ? MARKDOWN_CORE_NODE_HTML_BLOCK
               : MARKDOWN_CORE_NODE_NONE;
}

/* The HTML block it was, of type 2, which its opening set, and which its
 * close makes a CommentBlock again when it still holds one comment. */
static bool comment_revert(const markdown_core_element_instance *self, markdown_core_parser *parser,
                           markdown_core_node *node, markdown_core_node *parent, uint32_t *kept) {
    (void)self;
    (void)kept;
    const markdown_core_node_set_kind_result result =
        markdown_core_parser_set_node_kind(parser, node, parent, MARKDOWN_CORE_NODE_HTML_BLOCK);
    assert(result != MARKDOWN_CORE_NODE_SET_KIND_REJECTED);
    if (result != MARKDOWN_CORE_NODE_SET_KIND_OK) {
        return false;
    }
    node->as.html_block->block_type = 2;
    return true;
}

static const markdown_core_edit_ops COMMENT_EDIT = {
    .stops = comment_stops,
    .shuts = true,
};

static const markdown_core_content_ops COMMENT_CONTENT = {
    .mode = MARKDOWN_CORE_CONTENT_LITERAL,
};

/* THE INLINE COMMENT (plan symbols-cleanroom.md 23): `%%`, its body and
 * the next `%%`, one token. A `%` no comment follows is data. Where no
 * closer follows from, the read's record says, so a later opener past it
 * looks no further. */
enum { COMMENT_INLINE };
static markdown_core_state S_comment(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    ((markdown_core_comment_work *)self->state)->scan++;
    const uint32_t start = tokenizer->position, size = tokenizer->size;
    if (markdown_core_code_at(tokenizer, 1) != '%') {
        return markdown_core_nok(tokenizer);
    }
    /* One plus where a scan found no closer from. */
    uint32_t *failed = markdown_core_read_record(tokenizer, self->run_offset);
    const uint32_t from = start + 2;
    if (*failed && from >= *failed - 1) {
        markdown_core_met_end(tokenizer);
        return markdown_core_nok(tokenizer);
    }
    uint32_t close = from;
    while (close + 1 < size && !(tokenizer->data[close] == '%' && tokenizer->data[close + 1] == '%')) {
        close++;
    }
    if (close + 1 >= size) {
        markdown_core_met_end(tokenizer);
        *failed = from + 1;
        return markdown_core_nok(tokenizer);
    }
    if (close + 2 >= size) {
        markdown_core_met_end(tokenizer);
    }
    markdown_core_enter(tokenizer, (uint16_t)(self->token_base + COMMENT_INLINE));
    markdown_core_consume_to(tokenizer, close + 2);
    markdown_core_exit(tokenizer, (uint16_t)(self->token_base + COMMENT_INLINE));
    return markdown_core_ok(tokenizer);
}
static const markdown_core_text_construct COMMENT_CONSTRUCTS[] = {{"%", {.start = S_comment}}};

static void S_made(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    const markdown_core_element_instance *self = markdown_core_compile_self(compiler);
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    markdown_core_parser *parser = inline_state->owner_parser;
    const bufsize_t start = (bufsize_t)token->start, close = (bufsize_t)token->end - 2;
    markdown_core_node *node =
        markdown_core_parser_make_node_with_ext(parser, MARKDOWN_CORE_NODE_COMMENT, self->element);
    if (!node) {
        markdown_core_compile_append(compiler, NULL);
        return;
    }
    *node->as.literal = markdown_core_inline_take(inline_state, start + 2, close - start - 2);
    if (!markdown_core_chunk_to_cstr(node->as.literal)) {
        markdown_core_parser_release_node(parser, node);
        markdown_core_compile_append(compiler, NULL);
        return;
    }
    /* The scope covers both delimiters and the body. */
    markdown_core_inline_state_place(inline_state, node, start, close + 1);
    const markdown_core_field_to fields = markdown_core_inline_fields(inline_state, node);
    markdown_core_field_put(&fields, MARKDOWN_CORE_SLOT_NONE, MARKDOWN_CORE_FIELD_FOUND, start + 2, start + 2, true);
    markdown_core_compile_append(compiler, node);
}
static const markdown_core_compile_ops COMMENT_TOKENS[] = {[COMMENT_INLINE] = {.exit = S_made}};

static const markdown_core_inline_ops COMMENT_INLINES = {
    .constructs = COMMENT_CONSTRUCTS,
    .construct_count = 1,
    .tokens = COMMENT_TOKENS,
    .token_count = 1,
};

/* `is_fence_line` refuses anything whose first non-space byte is not `%`. */
static const markdown_core_block_construct COMMENT_STARTS[] = {
    {.bytes = "%", .indent = 3, .precedence = 1, .construct = {.start = S_start}},
};

static const markdown_core_compile_ops COMMENT_BLOCK_TOKENS[COMMENT_KINDS] = {
    [COMMENT_OPEN] = {.enter = S_open},
    [COMMENT_SHUT] = {.exit = close_comment},
};

static const markdown_core_open_ops COMMENT_OPENS = {
    .starts = COMMENT_STARTS,
    .start_count = 1,
    .tokens = COMMENT_BLOCK_TOKENS,
    .token_count = COMMENT_KINDS,
};

/* THE CONTINUATION (element.h, markdown_core_line_ops `continuation`). */
static markdown_core_state S_continue(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    /* A block of its kind holds no block a lookahead adds. */
    return markdown_core_block_verdict(
        tokenizer, turn->look ? MARKDOWN_CORE_LINE_OFF
                              : block_matches(markdown_core_self(tokenizer), turn->parser, turn->reader, turn->node));
}

static const markdown_core_line_ops COMMENT_LINES = {
    .shut = &COMMENT_BLOCK_TOKENS[COMMENT_SHUT],
    .continuation = {.start = S_continue},
    .beyond = true,
};

static bufsize_t S_follow_html(const markdown_core_element_instance *self, markdown_core_parser *parser,
                               markdown_core_lead *lead);
static const markdown_core_node_type COMMENT_LEADERS[] = {MARKDOWN_CORE_NODE_HTML_BLOCK, MARKDOWN_CORE_NODE_NONE};
static const markdown_core_follow COMMENT_FOLLOWS[] = {
    {.at = MARKDOWN_CORE_FOLLOW_CLOSE,
     .kinds = COMMENT_LEADERS,
     .follow = S_follow_html,
     .makes = MARKDOWN_CORE_NODE_COMMENT_BLOCK,
     .former = comment_former,
     .revert = comment_revert},
    {0},
};

/* A comment's literal (element.h, markdown_core_facet): a block's body
 * starts past its opener, which counts only while it is pending. */
static markdown_core_chunk *S_literal(const markdown_core_node *node, bufsize_t *start) {
    const bool held = markdown_core_claim_read((void *const *)&node->as.literal->data) != NULL;
    *start = node->kind != MARKDOWN_CORE_NODE_COMMENT_BLOCK || held ? 0 : markdown_core_comment_block_start(node);
    return node->as.literal;
}

const markdown_core_facet MARKDOWN_CORE_COMMENT_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_COMMENT, .literal = S_literal},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

static markdown_core_node *S_make_html(const markdown_core_element_instance *self,
                                       markdown_core_inline_state *inline_state, bufsize_t pos, bufsize_t length);
static const markdown_core_make_ops COMMENT_HTML = {.make = S_make_html};
static const markdown_core_service COMMENT_SERVICES[] = {{MARKDOWN_CORE_SERVICE_HTML_COMMENTS, &COMMENT_HTML}, {0}};

const markdown_core_element MARKDOWN_CORE_ELEMENT_COMMENT = {
    .provides = COMMENT_SERVICES,
    .facets = MARKDOWN_CORE_COMMENT_FACETS,
    .state_size = sizeof(markdown_core_comment_work),
    .run_state_size = sizeof(uint32_t),
    .follows = COMMENT_FOLLOWS,
    .name = "comment",
    .edit = &COMMENT_EDIT,
    .content = &COMMENT_CONTENT,
    .inlines = &COMMENT_INLINES,
    .opens = &COMMENT_OPENS,
    .lines = &COMMENT_LINES,
};

/* Where the body of an HTML comment block's content starts: past the
 * indent and the `<!--` (markdown_core_block_convert_comment_block). */
static bufsize_t S_comment_open(const markdown_core_strbuf *content) {
    bufsize_t open = 0;
    while (open < content->size && markdown_core_is_space_or_tab(markdown_core_strbuf_at(content, open))) {
        open++;
    }
    return open;
}

/* Where the body of a `%%` block's content starts: past its first line,
 * the rest of its fence line, spaces and its line ending. */
static bufsize_t S_percent_open(const markdown_core_strbuf *content) {
    bufsize_t open = S_comment_open(content);
    open += open < content->size && markdown_core_strbuf_at(content, open) == '\r';
    open += open < content->size && markdown_core_strbuf_at(content, open) == '\n';
    return open;
}

bufsize_t markdown_core_comment_block_start(const markdown_core_node *node) {
    return node->flags & MARKDOWN_CORE_NODE__COMMENT_HTML ? S_comment_open(&node->content) + 4
                                                          : S_percent_open(&node->content);
}

/* The byte at `at` of a content in its two parts (markdown_core_strbuf_span),
 * or in `front` alone when `whole`. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) unsigned char S_byte(const unsigned char *front,
                                                                            bufsize_t front_size,
                                                                            const unsigned char *back, bufsize_t at,
                                                                            const bool whole) {
    return whole || at < front_size ? front[at] : back[at - front_size];
}

/* Whether a `-->` starts on the line of `close`, past `from` and before
 * `close`, of a content in its two parts: read back from `close`, a part's
 * bytes at a time, each `>` that ends one. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool S_closed_before(const unsigned char *front,
                                                                            bufsize_t front_size,
                                                                            const unsigned char *back, bufsize_t from,
                                                                            bufsize_t close, const bool whole) {
    const bufsize_t low = from + 2;
    for (bufsize_t at = close + 1; at > low;) {
        const bool behind = !whole && at > front_size;
        const unsigned char *part = behind ? back : front;
        const bufsize_t base = behind ? front_size : 0;
        const bufsize_t stop = behind && front_size > low ? front_size : low;
        const unsigned char *const first = part + (stop - base);
        for (const unsigned char *byte = part + (at - base); byte > first;) {
            const unsigned char c = *--byte;
            if (c > '>') {
                continue;
            }
            const bufsize_t here = (bufsize_t)(byte - part) + base;
            if (c == '>' && S_byte(front, front_size, back, here - 1, whole) == '-' &&
                S_byte(front, front_size, back, here - 2, whole) == '-') {
                return true;
            }
            if (markdown_core_is_line_end(c)) {
                return false;
            }
        }
        at = stop;
    }
    return false;
}

/* The content reads through its parts when it is in two (buffer.h,
 * markdown_core_split), and only its first line and its last, from its
 * end: the block's end condition closed it at the first line that holds a
 * `-->`, so its first `-->` is on its last line, or on its first when it
 * has one, and only spaces and tabs may follow it. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void S_convert(markdown_core_parser *parser,
                                                                      markdown_core_node *b, markdown_core_node *parent,
                                                                      const unsigned char *front, bufsize_t front_size,
                                                                      const unsigned char *back, const bool whole) {
    const bufsize_t len = b->content.size;
    bufsize_t open = 0;
    while (open < len && markdown_core_is_space_or_tab(S_byte(front, front_size, back, open, whole))) {
        open++;
    }
    if (len - open < 4 || S_byte(front, front_size, back, open, whole) != '<' ||
        S_byte(front, front_size, back, open + 1, whole) != '!' ||
        S_byte(front, front_size, back, open + 2, whole) != '-' ||
        S_byte(front, front_size, back, open + 3, whole) != '-') {
        return;
    }
    /* Past the spaces and tabs before the line ending that ends it, a
     * `-->`, and no `-->` before it on its line from past the `<!`. */
    bufsize_t end = len;
    end -= end && S_byte(front, front_size, back, end - 1, whole) == '\n';
    end -= end && S_byte(front, front_size, back, end - 1, whole) == '\r';
    while (end && markdown_core_is_space_or_tab(S_byte(front, front_size, back, end - 1, whole))) {
        end--;
    }
    if (end < open + 5 || S_byte(front, front_size, back, end - 1, whole) != '>' ||
        S_byte(front, front_size, back, end - 2, whole) != '-' ||
        S_byte(front, front_size, back, end - 3, whole) != '-') {
        return;
    }
    const bufsize_t close = end - 3;
    if (S_closed_before(front, front_size, back, open + 2, close, whole)) {
        return;
    }

    const bufsize_t body_start = open + 4;
    const bufsize_t body_end = close > body_start ? close : body_start;
    markdown_core_node_set_kind_result result =
        markdown_core_parser_set_node_kind(parser, b, parent, MARKDOWN_CORE_NODE_COMMENT_BLOCK);
    if (result != MARKDOWN_CORE_NODE_SET_KIND_OK) {
        if (result == MARKDOWN_CORE_NODE_SET_KIND_ALLOCATION_FAILED) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
        return;
    }
    b->flags |= MARKDOWN_CORE_NODE__COMMENT_HTML;
    /* The comment's literal is the body between its delimiters, lazy as a
     * literal block's (node.h, A LITERAL, LAZY). */
    markdown_core_literal_set(b, b->as.literal, body_start, body_end);
    const markdown_core_field_to fields = {parser, b, &b->content_map, NULL, 0, NULL};
    markdown_core_field_put(&fields, MARKDOWN_CORE_SLOT_NONE, MARKDOWN_CORE_FIELD_FOUND, body_start, body_start, true);
}

/* AN HTML BLOCK THAT IS ONE COMMENT becomes a CommentBlock as it closes
 * (element.h, markdown_core_follow): a type-2 block its own end condition
 * closed that holds one comment and nothing else. */
static bufsize_t S_follow_html(const markdown_core_element_instance *self, markdown_core_parser *parser,
                               markdown_core_lead *lead) {
    (void)self;
    markdown_core_node *b = lead->node, *parent = lead->parent;
    if (b->as.html_block->block_type != 2 || !(b->flags & MARKDOWN_CORE_NODE__CLOSED_BY_END_CONDITION)) {
        return 0;
    }
    const markdown_core_strbuf *content = &b->content;
    if (!content->split) {
        S_convert(parser, b, parent, content->ptr, content->size, NULL, true);
        return 0;
    }
    const unsigned char *front, *back;
    bufsize_t front_size, back_size;
    markdown_core_strbuf_span(content, 0, content->size, &front, &front_size, &back, &back_size);
    S_convert(parser, b, parent, front, front_size, back, false);
    return 0;
}

markdown_core_node *markdown_core_comment_make_inline(markdown_core_inline_state *inline_state, int from, int to,
                                                      markdown_core_chunk literal) {
    return markdown_core_inline_make_literal(inline_state, MARKDOWN_CORE_NODE_COMMENT, from, to, literal);
}

static void close_comment(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)token;
    (void)index;
    markdown_core_block_turn *turn = markdown_core_block_turned(compiler);
    const markdown_core_element_instance *self = compiler->current->context;
    markdown_core_parser *parser = turn->parser;
    markdown_core_node *b = turn->node;
    markdown_core_node *parent = *turn->container;
    markdown_core_reader *reader = turn->reader;
    (void)self;
    (void)parser;
    (void)b;
    (void)parent;
    (void)reader;
    /* O3: a `%%` block comment arrives here with its lines in `content`:
     * the first is the rest of its fence line, and the closer line is not
     * there, because its matcher closed the block before the line could be
     * added. The literal is the view of the lines after the first,
     * indentation and line endings as written after container-prefix
     * removal, lazy as a literal block's (node.h, A LITERAL, LAZY). An HTML
     * block comment never takes this arm: it is closed as the HTML block it
     * was parsed as and becomes a CommentBlock there. */
    markdown_core_literal_set(b, b->as.literal, S_percent_open(&b->content), b->content.size);
}

/* (services.h, MARKDOWN_CORE_SERVICE_HTML_COMMENTS) The comment the HTML
 * grammar read, `length` bytes after its `<`, at `pos`. */
static markdown_core_node *S_make_html(const markdown_core_element_instance *self,
                                       markdown_core_inline_state *inline_state, bufsize_t pos, bufsize_t length) {
    (void)self;
    /* The empty short forms have overlapping opening and closing markers. */
    bufsize_t body_length = length > 6 ? length - 6 : 0;
    markdown_core_chunk body = markdown_core_inline_take(inline_state, pos + 3, body_length);
    markdown_core_inline_seek(inline_state, pos + length);
    markdown_core_node *node =
        markdown_core_comment_make_inline(inline_state, pos - 1, markdown_core_inline_offset(inline_state) - 1, body);
    if (node) {
        const markdown_core_field_to fields = markdown_core_inline_fields(inline_state, node);
        markdown_core_field_put(&fields, MARKDOWN_CORE_SLOT_NONE, MARKDOWN_CORE_FIELD_FOUND, pos + 3, pos + 3, true);
    }
    return node;
}
