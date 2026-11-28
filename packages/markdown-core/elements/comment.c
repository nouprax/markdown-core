#include "comment_scanners.h"
#include "comment.h"
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

/* THE CALL'S READING (element.h, `reads`): whether the fence line on the
 * reader opens a block a later line under the same prefixes in `container`
 * closes. In a commit more text may follow the reading waits where the text
 * read so far ends (markdown_core_parser_lookahead_wait), and a wait, or a
 * line the text has not finished, reads the end. */
static bool comment_reads(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_node *container, markdown_core_reader *reader, void *record) {
    (void)record;
    if (reader->indent >= 4 ||
        !is_fence_line(markdown_core_reader_rest(reader, 0), markdown_core_reader_length(reader), reader->nonspace)) {
        return false;
    }
    /* The lookahead's beginning writes the lookahead whole. */
    markdown_core_block_lookahead lookahead;
    if (!markdown_core_parser_lookahead_begin(parser, container, MARKDOWN_CORE_NODE_COMMENT_BLOCK, &lookahead)) {
        return false;
    }
    markdown_core_parser_lookahead_wait(&lookahead);
    bool closed = false;
    int blanks;
    markdown_core_reader *line;
    while ((line = markdown_core_parser_lookahead_next(&lookahead, &blanks))) {
        if (lookahead.limit && lookahead.incomplete) {
            markdown_core_parser_see_end(parser);
            break;
        }
        if (line->indent < 4 && is_fence_line(line->line.data, line->line.len, line->nonspace)) {
            closed = !parser->error;
            break;
        }
    }
    if (!line && (lookahead.exhausted || lookahead.waited)) {
        markdown_core_parser_see_end(parser);
    }
    markdown_core_parser_lookahead_opener(&lookahead, self, closed);
    markdown_core_parser_lookahead_end(&lookahead);
    return closed && !parser->error;
}

/* THE OPENER: a fence line whose call claims (markdown_core_parser_call). */
static markdown_core_node *open_block(const markdown_core_element_instance *self, int indented,
                                      markdown_core_parser *parser, markdown_core_node *parent_container,
                                      markdown_core_reader *reader) {
    int first_nonspace = reader->nonspace;
    markdown_core_node *node;

    if (indented ||
        !is_fence_line(markdown_core_reader_rest(reader, 0), markdown_core_reader_length(reader), first_nonspace) ||
        markdown_core_parser_call(parser, self, comment_reads, parent_container, reader,
                                  MARKDOWN_CORE_NODE_COMMENT_BLOCK, NULL, false) != MARKDOWN_CORE_CANDIDATE_ACCEPT) {
        return NULL;
    }

    node =
        markdown_core_parser_add_child(parser, parent_container, MARKDOWN_CORE_NODE_COMMENT_BLOCK, first_nonspace + 1);
    if (!node) {
        return NULL;
    }
    markdown_core_node_set_element(node, self->element);
    /* The rest of the fence line, spaces and its ending, is the first line
     * of the block's content, as a fence's rest is a code block's (blocks.c,
     * THE LEAP), and no part of its literal. */
    markdown_core_reader_advance(reader, first_nonspace + 2 - reader->offset, false);
    return node;
}

/* THE FEED of the opener's candidate (blocks.c, THE CANDIDATES): a fence
 * line closes the block and ends the construct, and any other line goes on
 * with it, a line the text has not finished once its bytes rule a fence
 * out. A block its text leaves open is not claimed. */
static markdown_core_candidate_step feed_candidate(const markdown_core_element_instance *self,
                                                   markdown_core_parser *parser, void *state,
                                                   const markdown_core_candidate_line *line, bool *ends) {
    (void)self;
    (void)parser;
    (void)state;
    markdown_core_reader *reader = line->reader;
    if (reader && reader->indent < 4) {
        const int first = reader->nonspace;
        const bool fence =
            is_fence_line(markdown_core_reader_rest(reader, 0), markdown_core_reader_length(reader), first);
        if (fence && !line->unfinished) {
            return MARKDOWN_CORE_CANDIDATE_ENDS;
        }
        if (line->unfinished && (fence || (markdown_core_reader_peek(reader, first) == '%' &&
                                           markdown_core_is_line_end(markdown_core_reader_peek(reader, first + 1))))) {
            return MARKDOWN_CORE_CANDIDATE_WAITS;
        }
    }
    *ends = false;
    return MARKDOWN_CORE_CANDIDATE_CONTINUES;
}

/* Every line reaches the open block until its closer; the closer line closes
 * it and is not added. The lookahead saw this closer before the block opened,
 * through the same container matchers, so an open block always finds it. */
static int block_matches(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         markdown_core_reader *reader, markdown_core_node *container) {
    if (reader->indent <= 3 &&
        is_fence_line(markdown_core_reader_rest(reader, 0), markdown_core_reader_length(reader), reader->nonspace)) {
        return MARKDOWN_CORE_BLOCK_CLOSED;
    }
    return 1;
}

/* One closer unit: `%%` closes; any other byte is one byte of body. */
static int scan_closer(const unsigned char *data, int length, int at, markdown_core_delimiter_rule rule, bool *closes) {
    (void)rule;
    if (data[at] == '%' && at + 1 < length && data[at + 1] == '%') {
        *closes = true;
        return 2;
    }
    return 1;
}

static markdown_core_node *match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    markdown_core_chunk *input = markdown_core_inline_state_get_chunk(inline_state);
    int start = markdown_core_inline_state_get_offset(inline_state);
    int close;
    markdown_core_node *node;

    if (character != '%') {
        return NULL;
    }
    ((markdown_core_comment_work *)self->state)->scan++;
    if (start + 1 >= input->len || input->data[start + 1] != '%') {
        if (start + 1 >= input->len) {
            markdown_core_inline_state_saw_end(inline_state);
        }
        return NULL;
    }
    close = markdown_core_inline_state_find_opaque_close(inline_state, MARKDOWN_CORE_DELIM_RULE_COMMENT, start + 2,
                                                         scan_closer);
    if (close < 0) {
        return NULL;
    }
    node = markdown_core_parser_make_node_with_ext(parser, MARKDOWN_CORE_NODE_COMMENT, self->element);
    if (!node) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    *node->as.literal = markdown_core_chunk_dup(input, start + 2, close - start - 2);
    if (!markdown_core_chunk_to_cstr(node->as.literal)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        markdown_core_parser_release_node(parser, node);
        return NULL;
    }
    /* The scope covers both delimiters and the body. */
    markdown_core_inline_state_place(inline_state, node, start, close + 1);
    markdown_core_inline_state_set_offset(inline_state, close + 2);
    return node;
}

/* `%` ends a text run and is offered to the scanner, and that is the whole set. */
static bool close_comment(const markdown_core_element_instance *self, markdown_core_parser *, markdown_core_node *,
                          markdown_core_node *, markdown_core_reader *);

/* An HTML block of type 2 that its own end condition closed and that
 * holds one comment became a CommentBlock as it closed
 * (markdown_core_block_convert_comment_block): it read its lines as an
 * HTML block (element.h, THE FORMER KIND). A `%%` block read them as
 * itself. */
enum { MARKDOWN_CORE_NODE__COMMENT_HTML = MARKDOWN_CORE_NODE__ELEMENT_FIRST };

static const markdown_core_content_ops COMMENT_CONTENT = {
    .mode = MARKDOWN_CORE_CONTENT_LITERAL,
};

static const markdown_core_inline_ops COMMENT_INLINES = {
    .match = match,
    .terminates = "%",
    .dispatch = "%",
};

static const markdown_core_candidate_ops COMMENT_CANDIDATE = {
    .feed = feed_candidate,
    .reads = comment_reads,
};

static const markdown_core_open_ops COMMENT_OPENS = {
    .indent = 3,
    .open = open_block,
    /* `is_fence_line` refuses anything whose first non-space byte is not `%`. */
    .open_needs = {.bytes = "%"},
};

static const markdown_core_line_ops COMMENT_LINES = {
    .close = close_comment,
    .matches = block_matches,
};

static bufsize_t S_follow_html(const markdown_core_element_instance *self, markdown_core_parser *parser,
                               markdown_core_lead *lead);
static const markdown_core_node_type COMMENT_LEADERS[] = {MARKDOWN_CORE_NODE_HTML_BLOCK, MARKDOWN_CORE_NODE_NONE};
static const markdown_core_follow COMMENT_FOLLOWS[] = {
    {.at = MARKDOWN_CORE_FOLLOW_CLOSE, .kinds = COMMENT_LEADERS, .follow = S_follow_html},
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

const markdown_core_element MARKDOWN_CORE_ELEMENT_COMMENT = {
    .facets = MARKDOWN_CORE_COMMENT_FACETS,
    .state_size = sizeof(markdown_core_comment_work),
    .follows = COMMENT_FOLLOWS,
    .name = "comment",
    .content = &COMMENT_CONTENT,
    .inlines = &COMMENT_INLINES,
    .candidate = &COMMENT_CANDIDATE,
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

static bool close_comment(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_node *b, markdown_core_node *parent, markdown_core_reader *reader) {
    (void)self;
    (void)parser;
    (void)parent;
    /* O3: a `%%` block comment arrives here with its lines in `content`:
     * the first is the rest of its fence line, and the closer line is not
     * there, because its matcher closed the block before the line could be
     * added. The literal is the view of the lines after the first,
     * indentation and line endings as written after container-prefix
     * removal, lazy as a literal block's (node.h, A LITERAL, LAZY). An HTML
     * block comment never takes this arm: it is closed as the HTML block it
     * was parsed as and becomes a CommentBlock there. */
    markdown_core_literal_set(b, b->as.literal, S_percent_open(&b->content), b->content.size);
    return true;
}

bool markdown_core_comment_scan_html(markdown_core_inline_state *inline_state, bufsize_t pos, bufsize_t *length) {
    if (inline_state->input.data[pos] != '!' || inline_state->input.data[pos + 1] != '-' ||
        inline_state->input.data[pos + 2] != '-') {
        return false;
    }
    if (inline_state->input.data[pos + 3] == '>') {
        *length = 4;
    } else if (inline_state->input.data[pos + 3] == '-' && inline_state->input.data[pos + 4] == '>') {
        *length = 5;
    } else {
        *length = scan_html_comment(inline_state->input.data, inline_state->input.len, pos + 1);
        /* 0 when no closer follows through the end of the input. */
        if (*length > 0) {
            *length += 1; // prefix "<"
        }
    }
    return true;
}
markdown_core_node *markdown_core_comment_make_html(markdown_core_inline_state *inline_state, bufsize_t pos,
                                                    bufsize_t length) {
    /* The empty short forms have overlapping opening and closing markers. */
    bufsize_t body_length = length > 6 ? length - 6 : 0;
    markdown_core_chunk body = markdown_core_chunk_dup(&inline_state->input, pos + 3, body_length);
    inline_state->pos = pos + length;
    return markdown_core_comment_make_inline(inline_state, pos - 1, inline_state->pos - 1, body);
}
