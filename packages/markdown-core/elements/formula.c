#include "alloc.h"
#include "formula_scanners.h"
#include "formula.h"
#include "element.h"
#include "inline_internal.h"

#include <assert.h>
#include <string.h>

#include <buffer.h>
#include <chunk.h>
#include <markdown_core_ctype.h>
#include <node.h>
#include <parser.h>

/* These were four SENTINEL BYTES -- 1, 2, 3, 4 -- because a delimiter carried a
 * byte and four kinds of formula opener had to be told apart by it. They were
 * ordinary file bytes: a literal 0x01 in a document ended the text run in front
 * of it and was offered to this element's inline hook. They are now four of
 * the eleven delimiter RULES, and no byte below 0x20 is special anywhere. */
#define FORMULA_DELIM_DOLLAR_INLINE MARKDOWN_CORE_DELIM_RULE_FORMULA_DOLLAR_INLINE
#define FORMULA_DELIM_DOLLAR_DISPLAY MARKDOWN_CORE_DELIM_RULE_FORMULA_DOLLAR_DISPLAY
#define FORMULA_DELIM_LATEX_BACKSLASH_INLINE MARKDOWN_CORE_DELIM_RULE_FORMULA_LATEX_INLINE
#define FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY MARKDOWN_CORE_DELIM_RULE_FORMULA_LATEX_DISPLAY

#define FORMULA_BLOCK_DELIM_NONE 0
#define FORMULA_BLOCK_DELIM_LATEX_BACKSLASH 1
#define FORMULA_BLOCK_DELIM_DOLLAR 2

typedef struct {
    markdown_core_chunk literal;
    markdown_core_formula_mode mode;
    int block_delim;
    int closed;
} node_formula;

/* A formula node's payload, its `opaque`. */
static node_formula *get_formula(markdown_core_node *node) { return (node_formula *)node->opaque; }

const char *markdown_core_elements_get_formula_literal(markdown_core_node *node) {
    return markdown_core_chunk_to_cstr(&get_formula(node)->literal);
}

int markdown_core_elements_set_formula_literal(markdown_core_node *node, const char *literal) {
    return markdown_core_chunk_set_cstr(&get_formula(node)->literal, literal);
}

markdown_core_formula_mode markdown_core_elements_get_formula_mode(markdown_core_node *node) {
    return get_formula(node)->mode;
}

void markdown_core_elements_set_formula_mode(markdown_core_node *node, markdown_core_formula_mode mode) {
    get_formula(node)->mode = mode;
}

/* Every node of this element is a formula. A payload that could not be
 * allocated is the constructor's allocation failure. */
static void formula_opaque_alloc(const markdown_core_element *element, markdown_core_node *node) {
    node->opaque = markdown_core_alloc(1, sizeof(node_formula));
}

static void formula_opaque_free(const markdown_core_element *element, markdown_core_node *node) {
    node_formula *formula = (node_formula *)node->opaque;
    if (!formula) {
        return;
    }

    markdown_core_chunk_free(&formula->literal);
    markdown_core_free(formula);
}

static int set_formula_literal_bytes(markdown_core_node *node, const unsigned char *data, bufsize_t len) {
    node_formula *formula = get_formula(node);
    markdown_core_chunk_free(&formula->literal);
    formula->literal.data = (unsigned char *)data;
    formula->literal.len = len;
    formula->literal.alloc = 0;
    if (!markdown_core_chunk_to_cstr(&formula->literal)) {
        /* THE BORROW MUST NOT SURVIVE THE COPY FAILING. `data` belongs to the
         * caller and dies immediately: `make_formula_span` frees
         * its temporary strbuf after constructing the leaf. Keeping a borrowed pointer past that is a use-after-free
         * that every later read of the literal walks into -- ASan: heap-use-after-free in
         * markdown_core_elements_get_formula_literal -- and `parser->error` stayed 0, so nothing downstream knew. Drop
         * the borrow and say so; the callers turn the 0 into the loss flag. */
        markdown_core_chunk empty = MARKDOWN_CORE_CHUNK_EMPTY;
        formula->literal = empty;
        return 0;
    }
    return 1;
}

/* A literal's storage follows its semantic owner. Trimming an owned chunk
 * compacts in place and preserves its allocation base/NUL invariant. Borrowed
 * chunks acquire ownership before any mutation, leaving the old view intact
 * on allocation failure. */
static int own_trimmed_literal(markdown_core_chunk *literal) {
    bufsize_t from = 0;
    bufsize_t end = literal->len;
    while (from < end && markdown_core_is_whitespace(literal->data[from])) {
        from++;
    }
    while (end > from && markdown_core_is_whitespace(literal->data[end - 1])) {
        end--;
    }
    bufsize_t length = end - from;
    if (literal->alloc) {
        if (from && length) {
            memmove(literal->data, literal->data + from, length);
        }
        literal->data[length] = 0;
        literal->len = length;
    } else {
        markdown_core_chunk owned = {literal->data ? literal->data + from : NULL, length, 0};
        if (!markdown_core_chunk_to_cstr(&owned)) {
            return 0;
        }
        *literal = owned;
    }
    return 1;
}

static int scan_formula_block_open(const unsigned char *data, bufsize_t len, bufsize_t pos) {
    if (pos + 3 <= len && data[pos] == '\\' && data[pos + 1] == '\\' && data[pos + 2] == '[' &&
        markdown_core_is_blank_to_line_end(data, pos + 3, len)) {
        return FORMULA_BLOCK_DELIM_LATEX_BACKSLASH;
    }

    if (pos + 2 <= len && data[pos] == '$' && data[pos + 1] == '$' &&
        markdown_core_is_blank_to_line_end(data, pos + 2, len)) {
        return FORMULA_BLOCK_DELIM_DOLLAR;
    }

    return FORMULA_BLOCK_DELIM_NONE;
}

static int scan_formula_block_close(const unsigned char *data, bufsize_t len, bufsize_t pos, int block_delim) {
    if (block_delim == FORMULA_BLOCK_DELIM_LATEX_BACKSLASH) {
        return pos + 3 <= len && data[pos] == '\\' && data[pos + 1] == '\\' && data[pos + 2] == ']' &&
               markdown_core_is_blank_to_line_end(data, pos + 3, len);
    }

    if (block_delim == FORMULA_BLOCK_DELIM_DOLLAR) {
        return pos + 2 <= len && data[pos] == '$' && data[pos + 1] == '$' &&
               markdown_core_is_blank_to_line_end(data, pos + 2, len);
    }

    return 0;
}

static int probe_formula_block(const markdown_core_element_instance *self, markdown_core_parser *parser,
                               markdown_core_chunk *input, int first, int indent, markdown_core_block_reader *reader) {
    (void)self;
    (void)parser;
    (void)reader;
    return indent < 4 && scan_formula_block_open(input->data, input->len, first) != FORMULA_BLOCK_DELIM_NONE;
}

static markdown_core_node *try_opening_formula_block(const markdown_core_element_instance *self, int indented,
                                                     markdown_core_parser *parser, markdown_core_node *parent_container,
                                                     unsigned char *input, int len) {
    int block_delim;
    markdown_core_node *node;
    node_formula *formula;
    int first_nonspace = markdown_core_parser_get_first_nonspace(parser);

    if (indented) {
        return NULL;
    }

    block_delim = scan_formula_block_open(input, (bufsize_t)len, (bufsize_t)first_nonspace);
    if (block_delim == FORMULA_BLOCK_DELIM_NONE) {
        return NULL;
    }

    node =
        markdown_core_parser_add_child(parser, parent_container, MARKDOWN_CORE_NODE_FORMULA_BLOCK, first_nonspace + 1);
    if (!node) {
        return NULL;
    }

    markdown_core_node_set_element(node, self->element);
    node->opaque = markdown_core_alloc(1, sizeof(node_formula));

    formula = get_formula(node);
    if (!formula) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }

    formula->mode = MARKDOWN_CORE_FORMULA_MODE_STANDALONE;
    formula->block_delim = block_delim;
    markdown_core_parser_advance_offset(parser, (char *)input, len - markdown_core_parser_get_offset(parser), false);
    return node;
}

static int formula_block_matches(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                 unsigned char *input, int len, markdown_core_node *container) {
    node_formula *formula = get_formula(container);
    int first_nonspace = markdown_core_parser_get_first_nonspace(parser);

    if (formula->closed) {
        return 0;
    }

    if (scan_formula_block_close(input, (bufsize_t)len, (bufsize_t)first_nonspace, formula->block_delim)) {
        formula->closed = 1;
        markdown_core_parser_advance_offset(parser, (char *)input, len - markdown_core_parser_get_offset(parser),
                                            false);
    }

    return 1;
}

static markdown_core_node *make_delimiter_text(markdown_core_parser *parser, markdown_core_inline_state *inline_state,
                                               bufsize_t len) {
    bufsize_t offset = (bufsize_t)markdown_core_inline_state_get_offset(inline_state);
    markdown_core_node *node;

    (void)parser;
    /* The cursor is at the run's FIRST byte here, and at its last in
     * `strikethrough` -- which is why each of them used to compute the columns
     * from a different end. The shared constructor is told the range. */
    node = markdown_core_inline_state_make_delimiter_text(inline_state, (int)offset, (int)(offset + len - 1));
    if (!node) {
        return NULL;
    }
    markdown_core_inline_state_set_offset(inline_state, (int)(offset + len));
    return node;
}

static int scan_formula_closer(const unsigned char *data, int length, int at, markdown_core_delimiter_rule rule,
                               bool *closes);

static markdown_core_node *make_formula_span(const markdown_core_element *element, markdown_core_parser *parser,
                                             markdown_core_inline_state *inline_state, markdown_core_node *parent,
                                             markdown_core_delimiter_rule rule, bufsize_t start, bufsize_t from,
                                             bufsize_t close, bufsize_t end);

static bool formula_body_admitted(markdown_core_delimiter_rule rule, const unsigned char *body, bufsize_t len) {
    return rule != FORMULA_DELIM_DOLLAR_INLINE || !len || body[0] != '`' || (len >= 2 && body[len - 1] == '`');
}

static markdown_core_node *match_formula_delimiter(const markdown_core_element_instance *self,
                                                   markdown_core_parser *parser, markdown_core_node *parent,
                                                   markdown_core_inline_state *inline_state,
                                                   markdown_core_delimiter_rule rule, bufsize_t len, int can_open,
                                                   int can_close) {
    if (can_open) {
        int start = markdown_core_inline_state_get_offset(inline_state);
        int from = start + len;
        int close = markdown_core_inline_state_find_opaque_close(inline_state, rule, from, scan_formula_closer);
        markdown_core_chunk *input = markdown_core_inline_state_get_chunk(inline_state);
        if (close >= 0 && formula_body_admitted(rule, input->data + from, close - from)) {
            /* A dynamic parent's policy is observed at delimiter reduction, in
             * source order with other deferred replacements. Keep that lifetime
             * (and exactly one policy decision) rather than asking early and
             * asking again in the token dispatcher. Fixed owners need no such
             * deferred transaction: this opaque production is already complete. */
            if (!(parent->element && parent->element->can_contain_func) &&
                markdown_core_node_can_contain_type(parent, MARKDOWN_CORE_NODE_FORMULA)) {
                markdown_core_node *formula = make_formula_span(self->element, parser, inline_state, parent, rule,
                                                                start, from, close, close + len);
                if (formula) {
                    markdown_core_inline_state_set_offset(inline_state, close + len);
                }
                return formula;
            }
            markdown_core_inline_state_set_opaque_body_end(inline_state, close);
        }
    }
    /* A rejected backtick-body candidate is ordinary inline grammar. Its
     * lexical closer can be hidden by a code span or bracket scope, so retain
     * the alternating delimiter state until that grammar resolves the range.
     * Both recognized and deferred pairs use the same literal constructor. */
    markdown_core_node *node = make_delimiter_text(parser, inline_state, len);

    if (!node) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }

    if (can_open || can_close) {
        /* The push adds the marker to the run. */
        markdown_core_inline_state_push_delimiter(inline_state, self, rule, can_open, can_close, node);
        return NULL;
    }
    return node;
}

static int dollar_inline_can_open(markdown_core_chunk *chunk, bufsize_t offset) {
    return offset + 1 < chunk->len && !markdown_core_is_whitespace(chunk->data[offset + 1]);
}

static int dollar_inline_can_close(markdown_core_chunk *chunk, bufsize_t offset) {
    return offset > 0 && !markdown_core_is_whitespace(chunk->data[offset - 1]) &&
           (offset + 1 >= chunk->len || !markdown_core_isdigit((char)chunk->data[offset + 1]));
}

static bufsize_t scan_backslash_close(const unsigned char *data, bufsize_t len, bufsize_t offset,
                                      unsigned char close_char, int slash_count) {
    int i;

    if (offset + slash_count + 1 > len) {
        return 0;
    }

    for (i = 0; i < slash_count; i++) {
        if (data[offset + i] != '\\') {
            return 0;
        }
    }

    if (data[offset + slash_count] == close_char) {
        return (bufsize_t)(slash_count + 1);
    }

    return 0;
}

/* A body's delimiter units are recognized before any other inline syntax.
 * Skipping paired dollars and escaped punctuation preserves delimiter spelling;
 * code, links and element tokens inside the body are never dispatched. */
static int scan_formula_closer(const unsigned char *data, int length, int at, markdown_core_delimiter_rule rule,
                               bool *closes) {
    if (rule == FORMULA_DELIM_LATEX_BACKSLASH_INLINE || rule == FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY) {
        unsigned char end = rule == FORMULA_DELIM_LATEX_BACKSLASH_INLINE ? ')' : ']';
        int width = scan_backslash_close(data, length, at, end, 2);
        if (width) {
            *closes = true;
            return width;
        }
    } else if (data[at] == '$') {
        bool pair = at + 1 < length && data[at + 1] == '$';
        if (rule == FORMULA_DELIM_DOLLAR_DISPLAY) {
            *closes = pair;
        } else if (!pair) {
            *closes = at > 0 && !markdown_core_is_whitespace(data[at - 1]) &&
                      (at + 1 == length || !markdown_core_isdigit(data[at + 1]));
        }
        return pair ? 2 : 1;
    }
    if (data[at] == '\\' && at + 1 < length && markdown_core_ispunct(data[at + 1])) {
        return 2;
    }
    return 1;
}

/* ONE OPEN FORMULA PER FORM. A delimiter of a form opens only while no opener
 * of that form is waiting on the stack, and closes only while one is, so a
 * form's delimiters alternate opener, closer, opener, closer, and a closer's
 * nearest unmatched opener is always the one directly before it. That is the
 * index's word for step A4 -- a scanner whose body is opaque -- said on the
 * delimiter stack the module names: from its opener on, a body runs to the
 * first closer of its form, and an opener inside it is the body's own bytes.
 * It is also what keeps the pairing linear. Without the gate, `\\(a \\(b\\) c\\)`
 * paired the inner `\\(` first and then let the outer closer take the outer
 * opener across the inner formula, so every level of a nest built a literal
 * the next level threw away, and a paragraph of thousands of nested openers
 * and closers copied bodies of growing length.
 *
 * The gate is the stack's own count, asked at the scan (`has_unmatched_opener`,
 * kept at every push and removal), so it costs nothing and needs no memory of
 * its own. A gated delimiter is still the text it was written as, and a pair
 * that fails its shape test in `insert_formula` releases its bytes the same
 * way.
 *
 * A backslash CLOSER with nothing to close is left to the base language
 * entirely: `\\]` is then CommonMark's escaped backslash followed by a
 * bracket closer, and the base scanner must see that `]`, or `[bar\\]` stops
 * being a reference (CommonMark 0.31.2 example 558). */
static markdown_core_node *match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    markdown_core_chunk *chunk = markdown_core_inline_state_get_chunk(inline_state);
    int offset = markdown_core_inline_state_get_offset(inline_state);
    int len = (int)chunk->len;
    bufsize_t opener_len;
    bufsize_t closer_len;
    int open;

    if (character == '$') {
        if (scan_formula_dollar_display_open(chunk->data, len, offset)) {
            open = markdown_core_inline_state_has_unmatched_opener(inline_state, FORMULA_DELIM_DOLLAR_DISPLAY);
            return match_formula_delimiter(self, parser, parent, inline_state, FORMULA_DELIM_DOLLAR_DISPLAY, 2, !open,
                                           open);
        }

        if (scan_formula_dollar_inline_open(chunk->data, len, offset)) {
            open = markdown_core_inline_state_has_unmatched_opener(inline_state, FORMULA_DELIM_DOLLAR_INLINE);
            return match_formula_delimiter(self, parser, parent, inline_state, FORMULA_DELIM_DOLLAR_INLINE, 1,
                                           !open && dollar_inline_can_open(chunk, (bufsize_t)offset),
                                           open && dollar_inline_can_close(chunk, (bufsize_t)offset));
        }
    } else if (character == '\\') {
        opener_len = scan_formula_latex_backslash_display_open(chunk->data, len, offset);
        if (opener_len) {
            open = markdown_core_inline_state_has_unmatched_opener(inline_state, FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY);
            return match_formula_delimiter(self, parser, parent, inline_state, FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY,
                                           opener_len, !open, 0);
        }

        opener_len = scan_formula_latex_backslash_inline_open(chunk->data, len, offset);
        if (opener_len) {
            open = markdown_core_inline_state_has_unmatched_opener(inline_state, FORMULA_DELIM_LATEX_BACKSLASH_INLINE);
            return match_formula_delimiter(self, parser, parent, inline_state, FORMULA_DELIM_LATEX_BACKSLASH_INLINE,
                                           opener_len, !open, 0);
        }

        closer_len = scan_backslash_close(chunk->data, chunk->len, offset, ']', 2);
        if (closer_len &&
            markdown_core_inline_state_has_unmatched_opener(inline_state, FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY)) {
            return match_formula_delimiter(self, parser, parent, inline_state, FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY,
                                           closer_len, 0, 1);
        }

        closer_len = scan_backslash_close(chunk->data, chunk->len, offset, ')', 2);
        if (closer_len &&
            markdown_core_inline_state_has_unmatched_opener(inline_state, FORMULA_DELIM_LATEX_BACKSLASH_INLINE)) {
            return match_formula_delimiter(self, parser, parent, inline_state, FORMULA_DELIM_LATEX_BACKSLASH_INLINE,
                                           closer_len, 0, 1);
        }
    }

    return NULL;
}

static markdown_core_formula_mode mode_for_delim(markdown_core_delimiter_rule delim_char) {
    return delim_char == FORMULA_DELIM_DOLLAR_DISPLAY || delim_char == FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY
               ? MARKDOWN_CORE_FORMULA_MODE_STANDALONE
               : MARKDOWN_CORE_FORMULA_MODE_EMBEDDED;
}

static int is_backslash_delim(markdown_core_delimiter_rule delim_char) {
    return delim_char == FORMULA_DELIM_LATEX_BACKSLASH_INLINE || delim_char == FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY;
}

static void release_items_through(markdown_core_inline_state *inline_state, markdown_core_inline_item *first,
                                  markdown_core_inline_item *last) {
    markdown_core_inline_item *item = first;

    while (item) {
        markdown_core_inline_item *next = item->next;
        markdown_core_inline_release(inline_state, item);
        if (item == last) {
            break;
        }
        item = next;
    }
}

/* micromark-element-math's padding rule (Q18): when a body BEGINS AND ENDS
 * with a space or line ending and is not all whitespace, strip one from each
 * end. Both or neither -- `$$ mid$$` keeps its leading space.
 *
 * Q18's own phrasing, "strip one leading and one trailing space-or-line-ending",
 * reads as two independent strips and is not: `elements-formula-github.txt`
 * pins `text $$ mid$$ text` as `literal=" mid"`, and the independent reading
 * gives `"mid"`. What separates this rule from CommonMark's code span is only
 * that the code span ALSO converts interior line endings to spaces; this one
 * leaves the interior exactly as written, which is why `$$  x  $$` keeps one
 * space on each side and `$$\nx\n$$` keeps none.
 *
 * `$$ $$` and `$$  $$` are all whitespace and keep every byte: there is no body
 * to pad, only padding. A TAB is not whitespace for that test, exactly as it is
 * not for the code span CommonMark words this after: `$$ \t $$` strips to
 * `"\t"`, and so does `` ` \t ` ``. This engine got that wrong until a mutant
 * that deleted the tab exemption turned out to be the correct code.
 *
 * `\r` counts as padding because the rule says a line ending does, not because
 * one can arrive: the line reader hands inline content LF-only, for a lone CR as
 * well as for a CRLF. Measured, not assumed -- disabling the arms that used to
 * collapse a CRLF to one byte left a `$$\r\nx\r\n$$` document byte-identical,
 * which is why those arms are gone and this one is not. A fixture cannot reach
 * either; the difference is that this one states the rule and they stated an
 * algorithm. If source-line normalization changes, they have to come back. */
static bool formula_pad_byte(unsigned char c) { return c == ' ' || markdown_core_is_line_end(c); }

static void strip_formula_padding(const unsigned char **literal, bufsize_t *len) {
    const unsigned char *data = *literal;
    bufsize_t size = *len;
    bufsize_t i;

    if (size < 2 || !formula_pad_byte(data[0]) || !formula_pad_byte(data[size - 1])) {
        return;
    }
    for (i = 0; i < size; i++) {
        if (!formula_pad_byte(data[i])) {
            break;
        }
    }
    if (i == size) {
        return;
    }

    data++;
    size--;
    size--;
    *literal = data;
    *len = size;
}

static markdown_core_node *make_formula_node(const markdown_core_element *element, markdown_core_parser *parser,
                                             markdown_core_formula_mode mode, const unsigned char *literal,
                                             bufsize_t literal_len) {
    markdown_core_node *node = markdown_core_parser_make_node_with_ext(parser, MARKDOWN_CORE_NODE_FORMULA, element);
    if (!node) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    if (!get_formula(node)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        markdown_core_parser_release_node(parser, node);
        return NULL;
    }

    get_formula(node)->mode = mode;
    if (!set_formula_literal_bytes(node, literal, literal_len)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        markdown_core_parser_release_node(parser, node);
        return NULL;
    }
    return node;
}

/* Construct one opaque production from its recognized source extent. The
 * deferred ordinary-grammar fallback uses exactly the same decoding and scope
 * operation after its delimiters have resolved. */
static markdown_core_node *make_formula_span(const markdown_core_element *element, markdown_core_parser *parser,
                                             markdown_core_inline_state *inline_state, markdown_core_node *parent,
                                             markdown_core_delimiter_rule rule, bufsize_t start, bufsize_t from,
                                             bufsize_t close, bufsize_t end) {
    markdown_core_chunk *chunk = markdown_core_inline_state_get_chunk(inline_state);
    markdown_core_formula_mode mode = mode_for_delim(rule);
    const unsigned char *body = chunk->data + from;
    bufsize_t body_len = close - from;
    markdown_core_strbuf unescaped = MARKDOWN_CORE_BUF_INIT();
    if (rule == FORMULA_DELIM_DOLLAR_INLINE && body_len > 0 && body[0] == '`') {
        assert(body_len >= 2 && body[body_len - 1] == '`');
        body++;
        body_len -= 2;
    }
    if (is_backslash_delim(rule)) {
        unsigned char close_char = mode == MARKDOWN_CORE_FORMULA_MODE_STANDALONE ? ']' : ')';
        bufsize_t i = 0;
        while (i < body_len) {
            if (body[i] == '\\' && i + 1 < body_len && body[i + 1] == close_char) {
                markdown_core_strbuf_putc(&unescaped, close_char);
                i += 2;
            } else {
                markdown_core_strbuf_putc(&unescaped, body[i++]);
            }
        }
        body = unescaped.ptr;
        body_len = unescaped.size;
    }
    strip_formula_padding(&body, &body_len);
    markdown_core_node *formula = unescaped.oom ? NULL : make_formula_node(element, parser, mode, body, body_len);
    markdown_core_strbuf_free(&unescaped);
    if (!formula) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    int line;
    bufsize_t first, last;
    markdown_core_parser_content_place(parser, &parent->content_map, start, &line, &first);
    markdown_core_parser_content_end_place(parser, &parent->content_map, end - 1, &line, &last);
    formula->where.place = (markdown_core_place){(uint32_t)first, (uint32_t)last};
    return formula;
}

static void insert_formula(const markdown_core_element_instance *self, markdown_core_parser *parser,
                           markdown_core_inline_state *inline_state, delimiter *opener, delimiter *closer) {
    markdown_core_chunk *chunk = markdown_core_inline_state_get_chunk(inline_state);
    markdown_core_inline_item *opener_item = markdown_core_delimiter_item(opener);
    markdown_core_inline_item *closer_item = markdown_core_delimiter_item(closer);
    markdown_core_node *parent = inline_state->owner;
    markdown_core_delimiter_rule rule = markdown_core_delimiter_rule_of(opener);
    bufsize_t from = markdown_core_delimiter_position(opener);
    bufsize_t close = markdown_core_delimiter_position(closer) - markdown_core_delimiter_length(closer);
    if (rule != markdown_core_delimiter_rule_of(closer) ||
        (is_backslash_delim(rule) &&
         markdown_core_delimiter_length(opener) != markdown_core_delimiter_length(closer)) ||
        !formula_body_admitted(rule, chunk->data + from, close - from) ||
        !markdown_core_node_can_contain_type(parent, MARKDOWN_CORE_NODE_FORMULA)) {
        return;
    }
    markdown_core_node *formula = make_formula_span(self->element, parser, inline_state, parent, rule,
                                                    from - markdown_core_delimiter_length(opener), from, close,
                                                    markdown_core_delimiter_position(closer));
    if (formula && markdown_core_inline_put(inline_state, opener_item, formula)) {
        release_items_through(inline_state, opener_item, closer_item);
    }
}

static const markdown_core_node_type containment_kinds[] = {MARKDOWN_CORE_NODE_FORMULA,
                                                            MARKDOWN_CORE_NODE_FORMULA_BLOCK, MARKDOWN_CORE_NODE_NONE};

static int accepts_lines(const markdown_core_element *element, markdown_core_node *node) {
    return node && node->kind == MARKDOWN_CORE_NODE_FORMULA_BLOCK;
}

/* An absent info string is not the word `formula`; it is no word at all. */
static int info_is_formula(const markdown_core_optional_chunk *info) {
    return info->has_value && info->value.len == 7 && memcmp(info->value.data, "formula", 7) == 0;
}

/* A NODE BECOMES A FORMULA BLOCK IN PLACE, through its parent, which decides
 * whether it may hold one: a field (no parent) keeps its kind, since it
 * belongs to its owner. The literal is the code block's own, or `donor`'s,
 * the standalone Formula that was a paragraph's only child, whose payload
 * moves to the block before the Formula is released. */
static markdown_core_finish_result become_formula_block(const markdown_core_element *element,
                                                        markdown_core_parser *parser, markdown_core_node *node,
                                                        markdown_core_node *parent, markdown_core_node *donor) {
    if (!parent || !markdown_core_node_can_contain_type(parent, MARKDOWN_CORE_NODE_FORMULA_BLOCK)) {
        return MARKDOWN_CORE_FINISH_CONTINUE;
    }
    node_formula *payload;
    if (donor) {
        /* Inline -> block changes ownership and mode, not the formula
         * payload's lifetime. */
        payload = get_formula(donor);
        if (!own_trimmed_literal(&payload->literal)) {
            goto failed;
        }
        donor = markdown_core_node_take_child(parser->pool, node, 0);
        if (!donor) {
            goto failed;
        }
        donor->opaque = NULL;
        markdown_core_parser_release_node(parser, donor);
    } else {
        payload = markdown_core_alloc(1, sizeof(*payload));
        if (!payload || !own_trimmed_literal(&node->as.code->literal)) {
            markdown_core_free(payload);
            goto failed;
        }
        payload->literal = node->as.code->literal;
        node->as.code->literal = (markdown_core_chunk)MARKDOWN_CORE_CHUNK_EMPTY;
    }
    payload->mode = MARKDOWN_CORE_FORMULA_MODE_STANDALONE;
    if (markdown_core_parser_set_node_kind(parser, parent, node, MARKDOWN_CORE_NODE_FORMULA_BLOCK) !=
        MARKDOWN_CORE_NODE_SET_KIND_OK) {
        markdown_core_chunk_free(&payload->literal);
        markdown_core_free(payload);
        goto failed;
    }
    markdown_core_node_set_element(node, element);
    node->opaque = payload;
    return MARKDOWN_CORE_FINISH_CONTINUE;

failed:
    markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    return MARKDOWN_CORE_FINISH_FAILED;
}

/* The formula element's finish STEP: one node when it completes -- a
 * FormulaBlock or a CodeBlock when it closes, a Paragraph when its inline
 * root's pass ends -- when its children are complete, so a Paragraph's test
 * of its only child sees what consolidation left there. */
static markdown_core_finish_result finish_step(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                               markdown_core_node *node, markdown_core_event_type event,
                                               markdown_core_node *parent, void **state) {
    (void)state;
    (void)event;
    assert(event == MARKDOWN_CORE_EVENT_EXIT);
    if (node->kind == MARKDOWN_CORE_NODE_FORMULA_BLOCK) {
        node_formula *formula = get_formula(node);
        if (formula && !formula->literal.data) {
            formula->literal = markdown_core_chunk_buf_detach(&node->content);
            if (!formula->literal.data || !own_trimmed_literal(&formula->literal)) {
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            }
        }
        return parser->error ? MARKDOWN_CORE_FINISH_FAILED : MARKDOWN_CORE_FINISH_CONTINUE;
    }

    if (node->kind == MARKDOWN_CORE_NODE_CODE_BLOCK && info_is_formula(&node->as.code->info)) {
        return become_formula_block(self->element, parser, node, parent, NULL);
    }

    /* Only an anonymous paragraph is a removable wrapper. A declared anchor
     * or attributes belong to that paragraph, even when its only remaining
     * content is a standalone formula. */
    if (node->kind == MARKDOWN_CORE_NODE_PARAGRAPH && !node->attributes.anchor.len && !node->attributes.class_count &&
        !node->attributes.record_count && markdown_core_node_children_count(node) == 1 &&
        markdown_core_node_first_child(node)->kind == MARKDOWN_CORE_NODE_FORMULA &&
        get_formula(markdown_core_node_first_child(node))->mode == MARKDOWN_CORE_FORMULA_MODE_STANDALONE) {
        return become_formula_block(self->element, parser, node, parent, markdown_core_node_first_child(node));
    }

    return MARKDOWN_CORE_FINISH_CONTINUE;
}

/* `$` and `\\` open a formula, and that is the whole set. `\\` is in the dispatch
 * set and NOT the terminator set: `is_core_special_character` refuses it there
 * anyway, and it must stay in dispatch because `handle_backslash` asks whether any
 * element claims `\\` before taking a core fast path. */

/* What the step acts on -- the gate -- and where it is asked. A Formula's
 * rewrite happens at its PARAGRAPH's EXIT (the paragraph is what becomes a
 * FormulaBlock), so the two lists differ there and agree elsewhere. */
static const markdown_core_node_type FORMULA_ACTS_ON_KINDS[] = {MARKDOWN_CORE_NODE_FORMULA_BLOCK,
                                                                MARKDOWN_CORE_NODE_CODE_BLOCK,
                                                                MARKDOWN_CORE_NODE_FORMULA, MARKDOWN_CORE_NODE_NONE};
static const markdown_core_node_type FORMULA_EXIT_KINDS[] = {MARKDOWN_CORE_NODE_FORMULA_BLOCK,
                                                             MARKDOWN_CORE_NODE_CODE_BLOCK,
                                                             MARKDOWN_CORE_NODE_PARAGRAPH, MARKDOWN_CORE_NODE_NONE};

const markdown_core_element MARKDOWN_CORE_ELEMENT_FORMULA = {
    .interrupts_paragraph = true,

    .name = "formula",
    .match_inline = match,
    .last_block_matches = formula_block_matches,
    .maximum_block_indent = 3,
    .try_opening_block = try_opening_formula_block,
    /* `scan_formula_block_open` accepts only `$$` and `\\[`. */
    .open_block_gate = {.bytes = "$\\"},
    .probe_block = probe_formula_block,
    .finish_step = finish_step,
    /* The step acts on a FormulaBlock, on a CodeBlock whose info string names
     * a formula, and on a standalone Formula -- a document with none of the
     * three has nothing for it, and the gate skips it at every paragraph. It
     * is asked at the EXIT of a FormulaBlock, of a CodeBlock, and of a
     * Paragraph whose only child may be that Formula; so a document with a
     * plain code fence and no formula lets the step in, and it is asked at
     * every Paragraph EXIT there, where it finds no Formula and returns. */
    .finish_acts_on_kinds = FORMULA_ACTS_ON_KINDS,
    .finish_exit_kinds = FORMULA_EXIT_KINDS,
    .containment_kinds = containment_kinds,
    .accepts_lines_func = accepts_lines,
    .opaque_alloc_func = formula_opaque_alloc,
    .opaque_free_func = formula_opaque_free,
    .insert_inline_from_delim = insert_formula,
    .terminates_text = "$",
    .dispatch = "$\\",
};
