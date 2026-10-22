#include "comment_scanners.h"
#include "html.h"
#include "comment.h"
#include "inline_internal.h"
#include "block_internal.h"
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

static int probe_comment_block(const markdown_core_element_instance *self, markdown_core_parser *parser,
                               markdown_core_chunk *input, int first, int indent, markdown_core_block_reader *reader) {
    (void)self;
    if (indent >= 4 || !is_fence_line(input->data, input->len, first)) {
        return 0;
    }
    markdown_core_chunk line;
    while (reader->next(reader->context, &line, &first, &indent)) {
        if (indent < 4 && is_fence_line(line.data, line.len, first)) {
            return !parser->error;
        }
    }
    return 0;
}

/* THE LINES AFTER THE FENCE LINE, read for its closer. In a commit more
 * text may follow the reading waits where the text read so far ends
 * (markdown_core_parser_lookahead_wait) and stops at a line the text has not
 * finished, which does not tell yet: either leaves the call `undecided`, and
 * the call asks to be a candidate (blocks.c, THE CANDIDATES). */
typedef struct {
    markdown_core_block_lookahead lookahead;
    bool undecided;
} comment_lines;

static int read_block_line(void *context, markdown_core_chunk *input, int *first, int *indent) {
    comment_lines *lines = context;
    int blanks;
    if (!markdown_core_parser_lookahead_next(&lines->lookahead, input, first, indent, &blanks)) {
        lines->undecided = lines->lookahead.exhausted || lines->lookahead.waited;
        return 0;
    }
    if (lines->lookahead.limit && lines->lookahead.incomplete) {
        lines->undecided = true;
        return 0;
    }
    return 1;
}

/* The block's reading from the fence line `line` in `parent`: whether a
 * later line under the same prefixes closes it. */
static bool comment_reads(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_node *parent, markdown_core_chunk *line, int first, bool *undecided) {
    /* The lookahead's beginning writes the lookahead whole. */
    comment_lines lines;
    lines.undecided = false;
    *undecided = false;
    if (!markdown_core_parser_lookahead_begin(parser, parent, MARKDOWN_CORE_NODE_COMMENT_BLOCK, &lines.lookahead)) {
        return false;
    }
    if (!markdown_core_parser_candidate_final(parser)) {
        markdown_core_parser_lookahead_wait(&lines.lookahead);
    }
    markdown_core_block_reader reader = {&lines, read_block_line};
    const bool closed = probe_comment_block(self, parser, line, first, parser->at.indent, &reader);
    markdown_core_parser_lookahead_end(&lines.lookahead);
    *undecided = !closed && lines.undecided;
    return closed && !parser->error;
}

/* THE OPENER. A call whose reading decides nothing asks to be a candidate;
 * a call that meets its candidate declines. */
static markdown_core_node *open_block(const markdown_core_element_instance *self, int indented,
                                      markdown_core_parser *parser, markdown_core_node *parent_container,
                                      unsigned char *input, int len) {
    int first_nonspace = markdown_core_parser_get_first_nonspace(parser);
    markdown_core_node *node;

    if (indented || !is_fence_line(input, len, first_nonspace) ||
        markdown_core_parser_candidate_meets(parser, self, parent_container)) {
        return NULL;
    }
    markdown_core_chunk line = {input, len, 0};
    bool undecided;
    const bool closed = comment_reads(self, parser, parent_container, &line, first_nonspace, &undecided);
    if (undecided && !parser->error) {
        markdown_core_parser_request_candidate(parser, self, parent_container, MARKDOWN_CORE_NODE_COMMENT_BLOCK, NULL,
                                               false);
    }
    if (!closed) {
        return NULL;
    }

    node =
        markdown_core_parser_add_child(parser, parent_container, MARKDOWN_CORE_NODE_COMMENT_BLOCK, first_nonspace + 1);
    if (!node) {
        return NULL;
    }
    markdown_core_node_set_element(node, self->element);
    /* The fence line is the block's marker and no part of its literal. */
    markdown_core_parser_advance_offset(parser, (char *)input, len - markdown_core_parser_get_offset(parser), false);
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
    const markdown_core_chunk *input = line->content;
    if (input && line->indent < 4) {
        const int first = line->first;
        const bool fence = is_fence_line(input->data, input->len, first);
        if (fence && !line->unfinished) {
            return MARKDOWN_CORE_CANDIDATE_ENDS;
        }
        if (line->unfinished &&
            (fence || (input->data[first] == '%' && markdown_core_is_line_end(input->data[first + 1])))) {
            return MARKDOWN_CORE_CANDIDATE_WAITS;
        }
    }
    *ends = false;
    return MARKDOWN_CORE_CANDIDATE_CONTINUES;
}

/* The opener's call decided as it would read now (blocks.c, THE
 * CANDIDATES). */
static markdown_core_candidate_decision decide_candidate(const markdown_core_element_instance *self,
                                                         markdown_core_parser *parser, const void *state) {
    (void)state;
    markdown_core_chunk line;
    markdown_core_node *container;
    if (!markdown_core_parser_candidate_start(parser, &line, &container)) {
        return MARKDOWN_CORE_CANDIDATE_REJECT;
    }
    bool closed = false, undecided = false;
    if (parser->at.indent < 4 && is_fence_line(line.data, line.len, parser->at.first_nonspace)) {
        closed = comment_reads(self, parser, container, &line, parser->at.first_nonspace, &undecided);
    }
    markdown_core_parser_candidate_end(parser);
    return undecided ? MARKDOWN_CORE_CANDIDATE_WAIT
           : closed  ? MARKDOWN_CORE_CANDIDATE_ACCEPT
                     : MARKDOWN_CORE_CANDIDATE_REJECT;
}

/* Every line reaches the open block until its closer; the closer line closes
 * it and is not added. The lookahead saw this closer before the block opened,
 * through the same container matchers, so an open block always finds it. */
static int block_matches(const markdown_core_element_instance *self, markdown_core_parser *parser, unsigned char *input,
                         int len, markdown_core_node *container) {
    if (markdown_core_parser_get_indent(parser) <= 3 &&
        is_fence_line(input, len, markdown_core_parser_get_first_nonspace(parser))) {
        return MARKDOWN_CORE_BLOCK_CLOSED;
    }
    return 1;
}

static int comment_accepts_lines(const markdown_core_element *element, markdown_core_node *node) {
    return node && node->kind == MARKDOWN_CORE_NODE_COMMENT_BLOCK;
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
    int line;
    bufsize_t first, last;
    markdown_core_parser_content_place(parser, &parent->content_map, start, &line, &first);
    markdown_core_parser_content_end_place(parser, &parent->content_map, close + 1, &line, &last);
    node->place = (markdown_core_place){(uint32_t)first, (uint32_t)last};
    markdown_core_inline_state_set_offset(inline_state, close + 2);
    return node;
}

/* `%` ends a text run and is offered to the scanner, and that is the whole set. */
static bool close_comment(const markdown_core_element_instance *self, markdown_core_parser *, markdown_core_node *,
                          markdown_core_node *);

const markdown_core_element MARKDOWN_CORE_ELEMENT_COMMENT = {
    .state_size = sizeof(markdown_core_comment_work),
    .interrupts_paragraph = true,

    .close_block = close_comment,

    .name = "comment",
    .match_inline = match,
    .last_block_matches = block_matches,
    .maximum_block_indent = 3,
    .try_opening_block = open_block,
    .feed_candidate = feed_candidate,
    .decide_candidate = decide_candidate,
    /* `is_fence_line` refuses anything whose first non-space byte is not `%`. */
    .open_block_gate = {.bytes = "%"},
    .probe_block = probe_comment_block,
    .accepts_lines_func = comment_accepts_lines,
    .terminates_text = "%",
    .dispatch = "%",
};

void markdown_core_block_convert_comment_block(markdown_core_parser *parser, markdown_core_node *b,
                                               markdown_core_node *parent) {
    const unsigned char *data = b->content.ptr;
    bufsize_t len = b->content.size;
    bufsize_t open = 0;
    bufsize_t close;
    bufsize_t body_start;
    bufsize_t body_len;
    bufsize_t rest;

    while (open < len && markdown_core_is_space_or_tab(data[open])) {
        open++;
    }
    if (len - open < 4 || memcmp(data + open, "<!--", 4) != 0) {
        return;
    }
    for (close = open + 2; close + 3 <= len; close++) {
        if (data[close] == '-' && data[close + 1] == '-' && data[close + 2] == '>') {
            break;
        }
    }
    if (close + 3 > len) {
        return;
    }
    rest = close + 3;
    while (rest < len && markdown_core_is_space_or_tab(data[rest])) {
        rest++;
    }
    if (rest < len && data[rest] == '\r') {
        rest++;
    }
    if (rest < len && data[rest] == '\n') {
        rest++;
    }
    if (rest != len) {
        return;
    }

    body_start = open + 4;
    body_len = close > body_start ? close - body_start : 0;
    markdown_core_node_set_kind_result result =
        markdown_core_parser_set_node_kind(parser, b, parent, MARKDOWN_CORE_NODE_COMMENT_BLOCK);
    if (result != MARKDOWN_CORE_NODE_SET_KIND_OK) {
        if (result == MARKDOWN_CORE_NODE_SET_KIND_ALLOCATION_FAILED) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
        return;
    }
    /* The comment's literal is the view of the body between its delimiters. */
    *b->as.literal = (markdown_core_chunk){b->content.ptr + body_start, body_len, 0};
}

markdown_core_node *markdown_core_comment_make_inline(markdown_core_inline_state *inline_state, int from, int to,
                                                      markdown_core_chunk literal) {
    return markdown_core_inline_make_literal(inline_state, MARKDOWN_CORE_NODE_COMMENT, from, to, literal);
}

static bool close_comment(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_node *b, markdown_core_node *parent) {
    (void)self;
    (void)parser;
    (void)parent;
    /* O3: a `%%` block comment arrives here with its lines in `content`:
     * the opener line contributed nothing, because the element that
     * opened it consumed the line, and the closer line is not there,
     * because its matcher closed the block before the line could be
     * added. The literal is the view of those lines, indentation and line
     * endings as written after container-prefix removal. An HTML block
     * comment never takes this arm: it is closed as the HTML block it was
     * parsed as and becomes a CommentBlock there. */
    *b->as.literal = (markdown_core_chunk){b->content.ptr, b->content.size, 0};
    return true;
}

bool markdown_core_comment_scan_html(markdown_core_inline_state *inline_state, bufsize_t pos, unsigned *flags,
                                     bufsize_t *length) {
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
        if (*length > 0) {
            *length += 1; // prefix "<"
        } else {          // no match through end of input: set a flag so
                          // we don't reparse looking for -->:
            *flags |= FLAG_SKIP_HTML_COMMENT;
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
