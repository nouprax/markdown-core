#include "alloc.h"
#include "formula_scanners.h"
#include "formula.h"
#include "attention.h"
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
    /* Where a FormulaBlock has its literal in its content, read while the
     * literal is pending (node.h, A LITERAL, LAZY). */
    bufsize_t start;
    /* What the fence a FormulaBlock was read as set as it opened, but its
     * info string, which is `formula`: where its literal starts in its
     * content, and its fence, whose character is 0 for a block that read its
     * lines as itself (element.h, THE FORMER KIND). */
    bufsize_t head;
    uint8_t fence_size, fence_offset;
    unsigned char fence_char;
    markdown_core_formula_mode mode;
    int block_delim;
} node_formula;

/* A formula node's payload, its `opaque`. */
static node_formula *get_formula(markdown_core_node *node) { return (node_formula *)node->opaque; }

markdown_core_chunk markdown_core_elements_formula_literal(const markdown_core_node *node) {
    return ((const node_formula *)node->opaque)->literal;
}

void markdown_core_elements_formula_halves(const markdown_core_node *node, const unsigned char **front,
                                           bufsize_t *front_size, const unsigned char **back, bufsize_t *back_size) {
    const node_formula *formula = node->opaque;
    markdown_core_literal_halves(node, &formula->literal, formula->start, front, front_size, back, back_size);
}

bool markdown_core_elements_formula_read(const markdown_core_node *node, const unsigned char **data, bufsize_t *size) {
    node_formula *formula = node->opaque;
    *size = formula->literal.len;
    return markdown_core_literal_read(node, &formula->literal, formula->start, data);
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
static void formula_opaque_alloc(markdown_core_node *node) {
    node->opaque = markdown_core_alloc(1, sizeof(node_formula));
}

static void formula_opaque_free(markdown_core_node_pool *pool, markdown_core_node *node) {
    (void)pool;
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
         * markdown_core_elements_formula_literal -- and `parser->error` stayed 0, so nothing downstream knew. Drop
         * the borrow and say so; the callers turn the 0 into the loss flag. */
        markdown_core_chunk empty = MARKDOWN_CORE_CHUNK_EMPTY;
        formula->literal = empty;
        return 0;
    }
    return 1;
}

/* The whitespace at the start and at the end of bytes in two parts, the
 * first part's then the second's, as many as there are of each. */
typedef struct {
    bufsize_t lead, trail;
} S_trim;

static S_trim S_trim_halves(const unsigned char *front, bufsize_t front_size, const unsigned char *back,
                            bufsize_t back_size) {
    S_trim held = {0, 0};
    const bufsize_t size = front_size + back_size;
    while (held.lead < size &&
           markdown_core_is_whitespace(held.lead < front_size ? front[held.lead] : back[held.lead - front_size])) {
        held.lead++;
    }
    while (held.lead + held.trail < size) {
        const bufsize_t at = size - 1 - held.trail;
        if (!markdown_core_is_whitespace(at < front_size ? front[at] : back[at - front_size])) {
            break;
        }
        held.trail++;
    }
    return held;
}

/* The whitespace at the ends of the bytes from `start` to `end` of a content
 * in two parts (markdown_core_strbuf_span), out of line so that the promote
 * of a content in one part stays light. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) S_trim
    S_trim_span(const markdown_core_strbuf *content, bufsize_t start, bufsize_t end) {
    const unsigned char *front, *back;
    bufsize_t front_size, back_size;
    markdown_core_strbuf_span(content, start, end, &front, &front_size, &back, &back_size);
    return S_trim_halves(front, front_size, back, back_size);
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

static markdown_core_node *try_opening_formula_block(const markdown_core_element_instance *self, int indented,
                                                     markdown_core_parser *parser, markdown_core_node *parent_container,
                                                     markdown_core_reader *reader) {
    int block_delim;
    markdown_core_node *node;
    node_formula *formula;
    int first_nonspace = reader->nonspace;

    if (indented) {
        return NULL;
    }

    block_delim = scan_formula_block_open(markdown_core_reader_rest(reader, 0), markdown_core_reader_length(reader),
                                          (bufsize_t)first_nonspace);
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
    /* The rest of the line, spaces and its ending, is the first line of
     * the block's content, as a fence's rest is a code block's (blocks.c,
     * THE LEAP): the literal, trimmed, starts past it. */
    const int past = first_nonspace + (block_delim == FORMULA_BLOCK_DELIM_DOLLAR ? 2 : 3);
    const markdown_core_field_to fields = {parser, node, NULL, reader, 0, NULL};
    markdown_core_field_put(&fields, MARKDOWN_CORE_SLOT_NONE, MARKDOWN_CORE_FIELD_FOUND, past, past, true);
    markdown_core_reader_advance(reader, past - reader->offset, false);
    return node;
}

static int formula_block_matches(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                 markdown_core_reader *reader, markdown_core_node *container) {
    node_formula *formula = get_formula(container);
    int first_nonspace = reader->nonspace;

    /* The closer is the block's end condition: its line closes it and is
     * no line of its content. */
    if (scan_formula_block_close(markdown_core_reader_rest(reader, 0), markdown_core_reader_length(reader),
                                 (bufsize_t)first_nonspace, formula->block_delim)) {
        markdown_core_reader_advance(reader, markdown_core_reader_length(reader) - reader->offset, false);
        return MARKDOWN_CORE_BLOCK_CLOSED;
    }

    return 1;
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

static markdown_core_formula_mode mode_for_delim(markdown_core_delimiter_rule delim_char) {
    return delim_char == FORMULA_DELIM_DOLLAR_DISPLAY || delim_char == FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY
               ? MARKDOWN_CORE_FORMULA_MODE_STANDALONE
               : MARKDOWN_CORE_FORMULA_MODE_EMBEDDED;
}

static int is_backslash_delim(markdown_core_delimiter_rule delim_char) {
    return delim_char == FORMULA_DELIM_LATEX_BACKSLASH_INLINE || delim_char == FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY;
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
    markdown_core_inline_state_place(inline_state, formula, (int)start, (int)end - 1);
    const markdown_core_field_to fields = markdown_core_inline_fields(inline_state, formula);
    markdown_core_field_put(&fields, MARKDOWN_CORE_SLOT_NONE, MARKDOWN_CORE_FIELD_FOUND, from, from, true);
    return formula;
}

/* THE FORMULA'S RUNS (plan symbols-cleanroom.md 23.10): a delimiter is a
 * sequence of the attention (attention.h) whose rule is its form, and the
 * forms alternate as `match` above says, by the counts the run record
 * keeps. An opener whose closer the opaque scan finds, of an admitted body
 * in a fixed owner that takes a formula, is the formula at once, a SPAN
 * token over the two and the body; any other pair the attention's pass
 * pairs is one when its body is admitted (`S_covers`). */
enum { FORMULA_SPAN = MARKDOWN_CORE_ATTENTION_KINDS, FORMULA_KINDS };
enum { FORMULA_FORMS = 4 };

/* A run's record: where the opaque scan of each form found no closer
 * from, and where the body of an opener whose pair is left to the pass
 * ends. */
typedef struct formula_record {
    uint32_t failed[FORMULA_FORMS];
    uint32_t opaque;
} formula_record;

static unsigned S_form(markdown_core_delimiter_rule rule) { return (unsigned)(rule - FORMULA_DELIM_DOLLAR_INLINE); }

/* A span's word: its rule and the lengths of its opener and closer. */
static uint32_t S_span_word(markdown_core_delimiter_rule rule, uint32_t open, uint32_t close) {
    return (uint32_t)rule | open << 8 | close << 16;
}

/* The first closer of `rule` from `from`, or -1. */
static int64_t S_find(markdown_core_tokenizer *tokenizer, formula_record *record, markdown_core_delimiter_rule rule,
                      uint32_t from) {
    const unsigned form = S_form(rule);
    if (record->failed[form] && from >= record->failed[form] - 1) {
        markdown_core_met_end(tokenizer);
        return -1;
    }
    const int size = (int)tokenizer->size;
    for (int at = (int)from; at < size;) {
        bool closes = false;
        const int width = scan_formula_closer(tokenizer->data, size, at, rule, &closes);
        if (closes) {
            if (at + width >= size) {
                markdown_core_met_end(tokenizer);
            }
            return at;
        }
        at += width;
    }
    markdown_core_met_end(tokenizer);
    record->failed[form] = from + 1;
    return -1;
}

/* Whether a run of `rule` on the stack opened that none closed: the runs
 * of a form alternate. */
static bool S_open(const markdown_core_tokenizer *tokenizer, markdown_core_delimiter_rule rule) {
    const markdown_core_inline_state *inline_state = tokenizer->reader;
    return inline_state->delim_openers[rule] > inline_state->delim_closers[rule];
}

static markdown_core_state S_run(markdown_core_tokenizer *tokenizer, markdown_core_delimiter_rule rule, uint32_t length,
                                 bool opens, bool closes) {
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    formula_record *record = markdown_core_read_record(tokenizer, self->run_offset);
    const markdown_core_inline_state *inline_state = tokenizer->reader;
    const uint32_t start = tokenizer->position, from = start + length;
    if (opens) {
        const int64_t close = S_find(tokenizer, record, rule, from);
        /* An opener with no closer opens a formula once one follows. */
        if (close < 0) {
            markdown_core_text_give_up(tokenizer, MARKDOWN_CORE_KIND_FORMULA, (bufsize_t)start, (bufsize_t)from, NULL,
                                       0);
        }
        if (close >= 0 && formula_body_admitted(rule, tokenizer->data + from, (bufsize_t)((uint32_t)close - from))) {
            /* A dynamic owner's policy is asked as the pass pairs, in
             * order with the other pairs; a fixed one takes it now. */
            markdown_core_node *owner = inline_state->owner;
            if (!markdown_core_node_allows(owner) &&
                markdown_core_node_can_contain_type(owner, MARKDOWN_CORE_NODE_FORMULA)) {
                const uint32_t token = markdown_core_enter(tokenizer, (uint16_t)(self->token_base + FORMULA_SPAN));
                markdown_core_consume_to(tokenizer, (uint32_t)close + length);
                markdown_core_exit(tokenizer, (uint16_t)(self->token_base + FORMULA_SPAN));
                if (token != UINT32_MAX) {
                    tokenizer->events.tokens[token].data = S_span_word(rule, length, length);
                }
                return markdown_core_ok(tokenizer);
            }
            record->opaque = (uint32_t)close;
        }
    }
    /* A run that can do neither is its text. */
    const unsigned kind = opens || closes ? MARKDOWN_CORE_ATTENTION_SEQUENCE : MARKDOWN_CORE_ATTENTION_LITERAL;
    const uint32_t token = markdown_core_enter(tokenizer, (uint16_t)(self->token_base + kind));
    markdown_core_consume_to(tokenizer, from);
    markdown_core_exit(tokenizer, (uint16_t)(self->token_base + kind));
    if (token != UINT32_MAX && kind == MARKDOWN_CORE_ATTENTION_SEQUENCE) {
        tokenizer->events.tokens[token].data = markdown_core_attention_word(opens, closes, (unsigned)rule, length);
        markdown_core_attention_count(tokenizer, (unsigned)rule, opens, closes);
    }
    return markdown_core_ok(tokenizer);
}

static bool S_can_open(const unsigned char *data, uint32_t size, uint32_t at) {
    return at + 1 < size && !markdown_core_is_whitespace(data[at + 1]);
}

static bool S_can_close(const unsigned char *data, uint32_t size, uint32_t at) {
    return at > 0 && !markdown_core_is_whitespace(data[at - 1]) &&
           (at + 1 >= size || !markdown_core_isdigit((char)data[at + 1]));
}

/* The forms as `match` reads them. */
static markdown_core_state S_start(markdown_core_tokenizer *tokenizer, int code) {
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    (void)self;
    const unsigned char *data = tokenizer->data;
    const bufsize_t size = (bufsize_t)tokenizer->size, at = (bufsize_t)tokenizer->position;
    /* Every delimiter is at most three bytes, and the byte after a `$`
     * says whether it opens or closes. */
    if (at + 3 > size) {
        markdown_core_met_end(tokenizer);
    }
    if (code == '$') {
        if (scan_formula_dollar_display_open(data, size, at)) {
            const bool open = S_open(tokenizer, FORMULA_DELIM_DOLLAR_DISPLAY);
            return S_run(tokenizer, FORMULA_DELIM_DOLLAR_DISPLAY, 2, !open, open);
        }
        if (scan_formula_dollar_inline_open(data, size, at)) {
            const bool open = S_open(tokenizer, FORMULA_DELIM_DOLLAR_INLINE);
            return S_run(tokenizer, FORMULA_DELIM_DOLLAR_INLINE, 1,
                         !open && S_can_open(data, (uint32_t)size, (uint32_t)at),
                         open && S_can_close(data, (uint32_t)size, (uint32_t)at));
        }
        return markdown_core_nok(tokenizer);
    }
    bufsize_t length = scan_formula_latex_backslash_display_open(data, size, at);
    if (length) {
        return S_run(tokenizer, FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY, (uint32_t)length,
                     !S_open(tokenizer, FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY), false);
    }
    length = scan_formula_latex_backslash_inline_open(data, size, at);
    if (length) {
        return S_run(tokenizer, FORMULA_DELIM_LATEX_BACKSLASH_INLINE, (uint32_t)length,
                     !S_open(tokenizer, FORMULA_DELIM_LATEX_BACKSLASH_INLINE), false);
    }
    length = scan_backslash_close(data, size, at, ']', 2);
    if (length && S_open(tokenizer, FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY)) {
        return S_run(tokenizer, FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY, (uint32_t)length, false, true);
    }
    length = scan_backslash_close(data, size, at, ')', 2);
    if (length && S_open(tokenizer, FORMULA_DELIM_LATEX_BACKSLASH_INLINE)) {
        return S_run(tokenizer, FORMULA_DELIM_LATEX_BACKSLASH_INLINE, (uint32_t)length, false, true);
    }
    return markdown_core_nok(tokenizer);
}

/* No settle point inside the body of an opener whose pair is left to the
 * pass. */
static bool S_holds(const markdown_core_tokenizer *tokenizer, const void *context) {
    const markdown_core_element_instance *self = context;
    const formula_record *record = markdown_core_read_record(tokenizer, self->run_offset);
    return tokenizer->position < record->opaque;
}

/* A pair the pass made is a formula when its body is admitted and the
 * owner takes one. */
static bool S_covers(const markdown_core_element_instance *self, markdown_core_tokenizer *tokenizer,
                     markdown_core_node_type holder, const markdown_core_token *opener,
                     const markdown_core_token *closer, uint32_t *word) {
    (void)self;
    const markdown_core_delimiter_rule rule = (markdown_core_delimiter_rule)markdown_core_attention_rule(opener->data);
    const uint32_t open = markdown_core_attention_length(opener->data);
    const uint32_t close = markdown_core_attention_length(closer->data);
    if ((is_backslash_delim(rule) && open != close) ||
        !formula_body_admitted(rule, tokenizer->data + opener->end, (bufsize_t)(closer->start - opener->end)) ||
        !markdown_core_attention_allows(tokenizer, holder, MARKDOWN_CORE_NODE_FORMULA)) {
        return false;
    }
    *word = S_span_word(rule, open, close);
    return true;
}

static void S_span(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    const markdown_core_element_instance *self = markdown_core_compile_self(compiler);
    const markdown_core_delimiter_rule rule = (markdown_core_delimiter_rule)(token->data & 0xffu);
    const uint32_t open = token->data >> 8 & 0xffu, close = token->data >> 16;
    markdown_core_node *formula =
        make_formula_span(self->element, inline_state->owner_parser, inline_state,
                          markdown_core_compile_parent(compiler), rule, (bufsize_t)token->start,
                          (bufsize_t)(token->start + open), (bufsize_t)(token->end - close), (bufsize_t)token->end);
    if (!formula && !inline_state->error) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
    }
    markdown_core_compile_append(compiler, formula);
}

static const markdown_core_text_construct FORMULA_CONSTRUCTS[] = {
    {"$\\", {.start = S_start, .resolve_all = markdown_core_attention_resolve, .late = true, .holds = S_holds}}};
static const markdown_core_compile_ops FORMULA_TOKENS[FORMULA_KINDS] = {
    MARKDOWN_CORE_ATTENTION_OPS, [FORMULA_SPAN] = {.exit = S_span, .opaque = true, .asked = true}};

static const markdown_core_node_type containment_kinds[] = {MARKDOWN_CORE_NODE_FORMULA,
                                                            MARKDOWN_CORE_NODE_FORMULA_BLOCK, MARKDOWN_CORE_NODE_NONE};

/* The literal of `formula`, a block's, which holds none: the bytes from
 * `from` to `end` of the block's content, trimmed of whitespace at both
 * ends, lazy as a literal block's (node.h, A LITERAL, LAZY): a content in
 * two parts (buffer.h, markdown_core_split) is read through them. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void S_trim_into(markdown_core_node *node, node_formula *formula,
                                                                        bufsize_t from, bufsize_t end) {
    if (!node->content.split) {
        const unsigned char *data = node->content.ptr;
        while (from < end && markdown_core_is_whitespace(data[from])) {
            from++;
        }
        while (end > from && markdown_core_is_whitespace(data[end - 1])) {
            end--;
        }
        formula->literal.data = node->content.ptr + from;
    } else {
        const S_trim held = S_trim_span(&node->content, from, end);
        end -= held.trail;
        from += held.lead;
        formula->literal.alloc = 1;
    }
    formula->literal.len = end - from;
    formula->start = from;
}

/* An absent info string is not the word `formula`; it is no word at all. */
static int info_is_formula(const markdown_core_optional_chunk *info) {
    return info->has_value && info->value.len == 7 && memcmp(info->value.data, "formula", 7) == 0;
}

/* The fence `node`, in `parent`, a FormulaBlock (markdown_core_formula_promote_fence). */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) void S_promote(const markdown_core_element_instance *formula,
                                                                      markdown_core_parser *parser,
                                                                      markdown_core_node *node,
                                                                      markdown_core_node *parent) {
    /* The formula's literal is the code's trimmed, lazy as the code's was
     * (node.h, A LITERAL, LAZY): its ends read through the content's parts. */
    node_formula *payload = markdown_core_alloc(1, sizeof(node_formula));
    if (!payload) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    markdown_core_code *code = node->as.code;
    S_trim_into(node, payload, code->literal_start, code->literal_start + code->literal.len);
    payload->head = code->literal_start;
    payload->fence_size = code->fence_length;
    payload->fence_offset = code->fence_offset;
    payload->fence_char = code->fence_char;
    payload->mode = MARKDOWN_CORE_FORMULA_MODE_STANDALONE;
    if (markdown_core_parser_set_node_kind(parser, node, parent, MARKDOWN_CORE_NODE_FORMULA_BLOCK) !=
        MARKDOWN_CORE_NODE_SET_KIND_OK) {
        markdown_core_free(payload);
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    markdown_core_attributes_free(&node->attributes);
    /* The literal, trimmed, is found past the fence line; the info string
     * is no field of a formula. */
    markdown_core_node_keep_anchors(node, MARKDOWN_CORE_FIELD_FOUND);
    node->content_map = (markdown_core_content_map){0};
    markdown_core_node_set_element(node, formula->element);
    node->opaque = payload;
}

/* A FENCE WHOSE INFO STRING NAMES A FORMULA becomes a FormulaBlock as
 * completion completes it (element.h, markdown_core_follow), inside the
 * block it settled into: before its explicit anchor is reserved, so the
 * anchor of the fence, which the FormulaBlock does not hold, is never
 * taken. Rewriting a valid block is optional: a parent that cannot hold a
 * FormulaBlock keeps the code block. */
static bufsize_t S_follow_fence(const markdown_core_element_instance *formula, markdown_core_parser *parser,
                                markdown_core_lead *lead) {
    markdown_core_node *node = lead->node, *parent = lead->parent;
    if (node->as.code->fenced && info_is_formula(&node->as.code->info) && parent &&
        markdown_core_node_can_contain_type(parent, MARKDOWN_CORE_NODE_FORMULA_BLOCK)) {
        S_promote(formula, parser, node, parent);
    }
    return 0;
}

/* A FormulaBlock's literal is the trimmed view of the lines it read. A
 * paragraph whose only content is a standalone formula stays a paragraph that
 * holds that Formula. */
static bool close_formula_block(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                markdown_core_node *node, markdown_core_node *parent, markdown_core_reader *reader) {
    (void)self;
    (void)parser;
    (void)parent;
    node_formula *formula = get_formula(node);
    markdown_core_chunk_free(&formula->literal);
    S_trim_into(node, formula, 0, node->content.size);
    return true;
}

/* `$` and `\\` open a formula, and that is the whole set. `\\` is in the dispatch
 * set and NOT the terminator set: the escape element terminates text there
 * anyway, and it must stay in dispatch because the escape element asks whether
 * any other element claims `\\` before taking its fast path for a run of
 * escaped backslashes (escape.c). */

static markdown_core_node_type formula_former(const markdown_core_element_instance *self,
                                              const markdown_core_node *node) {
    (void)self;
    return node->kind == MARKDOWN_CORE_NODE_FORMULA_BLOCK && ((const node_formula *)node->opaque)->fence_char
               ? MARKDOWN_CORE_NODE_CODE_BLOCK
               : MARKDOWN_CORE_NODE_NONE;
}

/* The fence it was, whose info string names a formula, as its opening left
 * it, which its completion promotes again (markdown_core_formula_promote_fence). */
static bool formula_revert(const markdown_core_element_instance *self, markdown_core_parser *parser,
                           markdown_core_node *node, markdown_core_node *parent, uint32_t *kept) {
    (void)kept;
    const node_formula *formula = get_formula(node);
    const markdown_core_code fence = {.literal_start = formula->head,
                                      .fence_length = formula->fence_size,
                                      .fence_offset = formula->fence_offset,
                                      .fence_char = formula->fence_char,
                                      .fenced = 1};
    const markdown_core_node_set_kind_result result =
        markdown_core_parser_set_node_kind(parser, node, parent, MARKDOWN_CORE_NODE_CODE_BLOCK);
    assert(result != MARKDOWN_CORE_NODE_SET_KIND_REJECTED);
    if (result != MARKDOWN_CORE_NODE_SET_KIND_OK) {
        return false;
    }
    *node->as.code = fence;
    node->as.code->info = markdown_core_optional_chunk_present(markdown_core_chunk_literal("formula"));
    formula_opaque_free(NULL, node);
    node->opaque = NULL;
    markdown_core_node_set_element(node, NULL);
    return true;
}

static const markdown_core_record_ops FORMULA_PAYLOAD = {
    .init = formula_opaque_alloc,
    .free = formula_opaque_free,
};

static const markdown_core_contain_ops FORMULA_CONTAIN = {
    .kinds = containment_kinds,
};

static const markdown_core_content_ops FORMULA_CONTENT = {
    .mode = MARKDOWN_CORE_CONTENT_LITERAL,
};

static const markdown_core_inline_ops FORMULA_INLINES = {
    .constructs = FORMULA_CONSTRUCTS,
    .construct_count = 1,
    .tokens = FORMULA_TOKENS,
    .token_count = FORMULA_KINDS,
    .covers = S_covers,
    .cover = FORMULA_SPAN,
};

static const markdown_core_open_ops FORMULA_OPENS = {
    .indent = 3,
    .open = try_opening_formula_block,
    /* `scan_formula_block_open` accepts only `$$` and `\\[`. */
    .open_needs = {.bytes = "$\\"},
};

static const markdown_core_line_ops FORMULA_LINES = {
    .matches = formula_block_matches,
    .close = close_formula_block,
};

static const markdown_core_node_type FORMULA_LEADERS[] = {MARKDOWN_CORE_NODE_CODE_BLOCK, MARKDOWN_CORE_NODE_NONE};

static const markdown_core_follow FORMULA_FOLLOWS[] = {
    {.at = MARKDOWN_CORE_FOLLOW_COMPLETE,
     .kinds = FORMULA_LEADERS,
     .follow = S_follow_fence,
     .makes = MARKDOWN_CORE_NODE_FORMULA_BLOCK,
     .former = formula_former,
     .revert = formula_revert},
    {0},
};

/* A formula's literal and placement (element.h, markdown_core_facet). */
static markdown_core_chunk *S_literal(const markdown_core_node *node, bufsize_t *start) {
    node_formula *formula = node->opaque;
    *start = formula->start;
    return &formula->literal;
}

static markdown_core_placement S_placement(const markdown_core_node *node) {
    return ((const node_formula *)node->opaque)->mode == MARKDOWN_CORE_FORMULA_MODE_EMBEDDED
               ? MARKDOWN_CORE_PLACEMENT_EMBEDDED
               : MARKDOWN_CORE_PLACEMENT_STANDALONE;
}

/* A formula reads as its literal in a name. */
static bool S_words(const markdown_core_node *node, const markdown_core_words *words) {
    const unsigned char *front, *back;
    bufsize_t front_size, back_size;
    markdown_core_elements_formula_halves(node, &front, &front_size, &back, &back_size);
    return words->text(words->context, front, front_size) && words->text(words->context, back, back_size);
}

const markdown_core_facet MARKDOWN_CORE_FORMULA_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_FORMULA, .literal = S_literal, .placement = S_placement, .words = S_words},
    {.kind = MARKDOWN_CORE_KIND_FORMULA_BLOCK, .literal = S_literal, .placement = S_placement},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_FORMULA = {
    .facets = MARKDOWN_CORE_FORMULA_FACETS,
    .name = "formula",
    .follows = FORMULA_FOLLOWS,
    .payload = &FORMULA_PAYLOAD,
    .contain = &FORMULA_CONTAIN,
    .content = &FORMULA_CONTENT,
    .inlines = &FORMULA_INLINES,
    .run_state_size = sizeof(formula_record),
    .opens = &FORMULA_OPENS,
    .lines = &FORMULA_LINES,
};
