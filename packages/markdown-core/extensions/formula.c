#include "formula.h"
#include "syntax_extension.h"

#include <assert.h>
#include <string.h>

#include <buffer.h>
#include <chunk.h>
#include <markdown_core_ctype.h>
#include <node.h>
#include <parser.h>

#include "ext_scanners.h"

/* These were four SENTINEL BYTES -- 1, 2, 3, 4 -- because a delimiter carried a
 * byte and four kinds of formula opener had to be told apart by it. They were
 * ordinary file bytes: a literal 0x01 in a document ended the text run in front
 * of it and was offered to this extension's inline hook. They are now four of
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

static int is_formula_node(markdown_core_node *node) {
    if (!node) {
        return 0;
    }

    return node->type == MARKDOWN_CORE_NODE_FORMULA || node->type == MARKDOWN_CORE_NODE_FORMULA_BLOCK;
}

static node_formula *get_formula(markdown_core_node *node) {
    if (!is_formula_node(node)) {
        return NULL;
    }

    return (node_formula *)node->as.opaque;
}

static int is_standalone_formula_node(markdown_core_node *node) {
    node_formula *formula = get_formula(node);

    if (!formula) {
        return 0;
    }

    return formula->mode == MARKDOWN_CORE_FORMULA_MODE_STANDALONE;
}

const char *markdown_core_extensions_get_formula_literal(markdown_core_node *node) {
    node_formula *formula = get_formula(node);
    if (!formula) {
        return NULL;
    }

    return markdown_core_chunk_to_cstr(markdown_core_node_mem(node), &formula->literal);
}

int markdown_core_extensions_set_formula_literal(markdown_core_node *node, const char *literal) {
    node_formula *formula = get_formula(node);
    if (!formula) {
        return 0;
    }

    markdown_core_chunk_set_cstr(markdown_core_node_mem(node), &formula->literal, literal);
    return 1;
}

markdown_core_formula_mode markdown_core_extensions_get_formula_mode(markdown_core_node *node) {
    node_formula *formula = get_formula(node);
    if (!formula) {
        return MARKDOWN_CORE_FORMULA_MODE_NONE;
    }

    return formula->mode;
}

int markdown_core_extensions_set_formula_mode(markdown_core_node *node, markdown_core_formula_mode mode) {
    node_formula *formula = get_formula(node);
    if (!formula) {
        return 0;
    }

    if (mode != MARKDOWN_CORE_FORMULA_MODE_EMBEDDED && mode != MARKDOWN_CORE_FORMULA_MODE_STANDALONE) {
        return 0;
    }

    if (node->type == MARKDOWN_CORE_NODE_FORMULA_BLOCK && mode != MARKDOWN_CORE_FORMULA_MODE_STANDALONE) {
        return 0;
    }

    formula->mode = mode;
    return 1;
}

static void formula_opaque_alloc(const markdown_core_syntax_extension *extension, markdown_core_mem *mem,
                                 markdown_core_node *node) {
    /* A NULL payload is tolerated: every accessor goes through get_formula
     * and treats the node as formula-less. */
    if (is_formula_node(node)) {
        node->as.opaque = mem->calloc(1, sizeof(node_formula));
    }
}

static void formula_opaque_free(const markdown_core_syntax_extension *extension, markdown_core_mem *mem,
                                markdown_core_node *node) {
    node_formula *formula = (node_formula *)node->as.opaque;
    if (!formula) {
        return;
    }

    markdown_core_chunk_free(mem, &formula->literal);
    mem->free(formula);
}

static int set_formula_literal_bytes(markdown_core_node *node, const unsigned char *data, bufsize_t len) {
    node_formula *formula = get_formula(node);
    if (!formula) {
        return 0;
    }

    markdown_core_chunk_free(markdown_core_node_mem(node), &formula->literal);
    formula->literal.data = (unsigned char *)data;
    formula->literal.len = len;
    formula->literal.alloc = 0;
    if (!markdown_core_chunk_to_cstr(markdown_core_node_mem(node), &formula->literal)) {
        /* THE BORROW MUST NOT SURVIVE THE COPY FAILING. `data` belongs to the
         * caller and dies immediately: `make_backslash_delimited_formula` frees
         * its strbuf on the next statement, `replace_with_formula_block` frees
         * the whole old code block, and `postprocess_node` clears the node's own
         * content. Keeping a borrowed pointer past that is a use-after-free that
         * every later read of the literal walks into -- ASan: heap-use-after-free
         * in markdown_core_extensions_get_formula_literal -- and `parser->oom`
         * stayed 0, so nothing downstream knew. Drop the borrow and say so; the
         * callers turn the 0 into the loss flag. */
        markdown_core_chunk empty = MARKDOWN_CORE_CHUNK_EMPTY;
        formula->literal = empty;
        return 0;
    }
    return 1;
}

static int set_formula_literal_trimmed(markdown_core_node *node, const unsigned char *data, bufsize_t len) {
    while (len > 0 && markdown_core_isspace(data[0])) {
        data++;
        len--;
    }

    while (len > 0 && markdown_core_isspace(data[len - 1])) {
        len--;
    }

    return set_formula_literal_bytes(node, data, len);
}

static markdown_core_node *make_formula_node(const markdown_core_syntax_extension *extension,
                                             markdown_core_parser *parser, markdown_core_node_type node_type,
                                             markdown_core_formula_mode mode, const unsigned char *literal,
                                             bufsize_t literal_len) {
    markdown_core_node *node = markdown_core_node_new_with_mem_and_ext(node_type, parser->mem, extension);
    if (!node) {
        parser->oom = true;
        return NULL;
    }
    if (!get_formula(node)) {
        parser->oom = true;
        markdown_core_node_free(node);
        return NULL;
    }

    get_formula(node)->mode = mode;
    if (!set_formula_literal_bytes(node, literal, literal_len)) {
        parser->oom = true;
        markdown_core_node_free(node);
        return NULL;
    }
    return node;
}

static int is_line_end(const unsigned char *data, bufsize_t len, bufsize_t pos) {
    return pos >= len || data[pos] == '\n' || data[pos] == '\r';
}

static int has_only_spaces_until_line_end(const unsigned char *data, bufsize_t len, bufsize_t pos) {
    while (pos < len && (data[pos] == ' ' || data[pos] == '\t')) {
        pos++;
    }

    return is_line_end(data, len, pos);
}

static int scan_formula_block_open(const unsigned char *data, bufsize_t len, bufsize_t pos) {
    if (pos + 3 <= len && data[pos] == '\\' && data[pos + 1] == '\\' && data[pos + 2] == '[' &&
        has_only_spaces_until_line_end(data, len, pos + 3)) {
        return FORMULA_BLOCK_DELIM_LATEX_BACKSLASH;
    }

    if (pos + 2 <= len && data[pos] == '$' && data[pos + 1] == '$' &&
        has_only_spaces_until_line_end(data, len, pos + 2)) {
        return FORMULA_BLOCK_DELIM_DOLLAR;
    }

    return FORMULA_BLOCK_DELIM_NONE;
}

static int scan_formula_block_close(const unsigned char *data, bufsize_t len, bufsize_t pos, int block_delim) {
    if (block_delim == FORMULA_BLOCK_DELIM_LATEX_BACKSLASH) {
        return pos + 3 <= len && data[pos] == '\\' && data[pos + 1] == '\\' && data[pos + 2] == ']' &&
               has_only_spaces_until_line_end(data, len, pos + 3);
    }

    if (block_delim == FORMULA_BLOCK_DELIM_DOLLAR) {
        return pos + 2 <= len && data[pos] == '$' && data[pos + 1] == '$' &&
               has_only_spaces_until_line_end(data, len, pos + 2);
    }

    return 0;
}

static markdown_core_node *try_opening_formula_block(const markdown_core_syntax_extension *extension, int indented,
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

    markdown_core_node_set_syntax_extension(node, extension);
    node->as.opaque = parser->mem->calloc(1, sizeof(node_formula));

    formula = get_formula(node);
    if (!formula) {
        parser->oom = true;
        return NULL;
    }

    formula->mode = MARKDOWN_CORE_FORMULA_MODE_STANDALONE;
    formula->block_delim = block_delim;
    markdown_core_parser_advance_offset(parser, (char *)input, len - markdown_core_parser_get_offset(parser), false);
    return node;
}

static int formula_block_matches(const markdown_core_syntax_extension *extension, markdown_core_parser *parser,
                                 unsigned char *input, int len, markdown_core_node *container) {
    node_formula *formula = get_formula(container);
    int first_nonspace = markdown_core_parser_get_first_nonspace(parser);

    if (!formula || formula->closed) {
        return 0;
    }

    if (scan_formula_block_close(input, (bufsize_t)len, (bufsize_t)first_nonspace, formula->block_delim)) {
        formula->closed = 1;
        markdown_core_parser_advance_offset(parser, (char *)input, len - markdown_core_parser_get_offset(parser),
                                            false);
    }

    return 1;
}

static markdown_core_node *make_delimiter_text(markdown_core_parser *parser, markdown_core_inline_parser *inline_parser,
                                               bufsize_t len) {
    markdown_core_chunk *chunk = markdown_core_inline_parser_get_chunk(inline_parser);
    bufsize_t offset = (bufsize_t)markdown_core_inline_parser_get_offset(inline_parser);
    markdown_core_node *node = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_TEXT, parser->mem);

    if (!node) {
        return NULL;
    }

    node->as.literal = markdown_core_chunk_dup(chunk, offset, len);
    node->start_line = node->end_line = markdown_core_inline_parser_get_line(inline_parser);
    node->start_column = markdown_core_inline_parser_get_column(inline_parser);
    node->end_column = node->start_column + (int)len - 1;
    markdown_core_inline_parser_set_offset(inline_parser, (int)(offset + len));
    return node;
}

static markdown_core_node *match_formula_delimiter(const markdown_core_syntax_extension *self,
                                                   markdown_core_parser *parser,
                                                   markdown_core_inline_parser *inline_parser,
                                                   markdown_core_delimiter_rule rule, bufsize_t len, int can_open,
                                                   int can_close) {
    markdown_core_node *node = make_delimiter_text(parser, inline_parser, len);

    if (!node) {
        parser->oom = true;
        return NULL;
    }

    if (can_open || can_close) {
        markdown_core_inline_parser_push_delimiter(inline_parser, self, rule, can_open, can_close, node);
    }
    return node;
}

static int dollar_inline_can_open(markdown_core_chunk *chunk, bufsize_t offset) {
    return offset + 1 < chunk->len && !markdown_core_isspace((char)chunk->data[offset + 1]);
}

static int dollar_inline_can_close(markdown_core_chunk *chunk, bufsize_t offset) {
    return offset > 0 && !markdown_core_isspace((char)chunk->data[offset - 1]) &&
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

static markdown_core_node *match(const markdown_core_syntax_extension *extension, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_parser *inline_parser) {
    markdown_core_chunk *chunk = markdown_core_inline_parser_get_chunk(inline_parser);
    int offset = markdown_core_inline_parser_get_offset(inline_parser);
    int len = (int)chunk->len;
    bufsize_t opener_len;
    bufsize_t closer_len;

    if (character == '$') {
        if (scan_formula_dollar_display_open(chunk->data, len, offset)) {
            return match_formula_delimiter(extension, parser, inline_parser, FORMULA_DELIM_DOLLAR_DISPLAY, 2, 1, 1);
        }

        if (scan_formula_dollar_inline_open(chunk->data, len, offset)) {
            return match_formula_delimiter(extension, parser, inline_parser, FORMULA_DELIM_DOLLAR_INLINE, 1,
                                           dollar_inline_can_open(chunk, (bufsize_t)offset),
                                           dollar_inline_can_close(chunk, (bufsize_t)offset));
        }
    } else if (character == '\\') {
        opener_len = scan_formula_latex_backslash_display_open(chunk->data, len, offset);
        if (opener_len) {
            return match_formula_delimiter(extension, parser, inline_parser, FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY,
                                           opener_len, 1, 0);
        }

        opener_len = scan_formula_latex_backslash_inline_open(chunk->data, len, offset);
        if (opener_len) {
            return match_formula_delimiter(extension, parser, inline_parser, FORMULA_DELIM_LATEX_BACKSLASH_INLINE,
                                           opener_len, 1, 0);
        }

        closer_len = scan_backslash_close(chunk->data, chunk->len, offset, ']', 2);
        if (closer_len) {
            return match_formula_delimiter(extension, parser, inline_parser, FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY,
                                           closer_len, 0, 1);
        }

        closer_len = scan_backslash_close(chunk->data, chunk->len, offset, ')', 2);
        if (closer_len) {
            return match_formula_delimiter(extension, parser, inline_parser, FORMULA_DELIM_LATEX_BACKSLASH_INLINE,
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

static void remove_delimiters(markdown_core_inline_parser *inline_parser, delimiter *opener, delimiter *closer) {
    delimiter *delim = closer;

    while (delim != NULL && delim != opener) {
        delimiter *previous = markdown_core_delimiter_previous(delim);
        markdown_core_inline_parser_remove_delimiter(inline_parser, delim);
        delim = previous;
    }

    markdown_core_inline_parser_remove_delimiter(inline_parser, opener);
}

static void free_nodes_through(markdown_core_node *first, markdown_core_node *last) {
    markdown_core_node *node = first;

    while (node) {
        markdown_core_node *next = markdown_core_node_next(node);
        markdown_core_node_free(node);
        if (node == last) {
            break;
        }
        node = next;
    }
}

/* micromark-extension-math's padding rule (Q18): when a body BEGINS AND ENDS
 * with a space or line ending and is not all whitespace, strip one from each
 * end. Both or neither -- `$$ mid$$` keeps its leading space.
 *
 * Q18's own phrasing, "strip one leading and one trailing space-or-line-ending",
 * reads as two independent strips and is not: `extensions-formula-github.txt`
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
 * algorithm. If the feed ever stops normalising, they have to come back. */
static bool formula_pad_byte(unsigned char c) { return c == ' ' || c == '\n' || c == '\r'; }

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

static markdown_core_node *make_backslash_delimited_formula(const markdown_core_syntax_extension *extension,
                                                            markdown_core_parser *parser,
                                                            markdown_core_formula_mode mode, const unsigned char *data,
                                                            bufsize_t body_start, bufsize_t body_end, int slash_count,
                                                            unsigned char close_char) {
    markdown_core_strbuf literal;
    bufsize_t i = body_start;
    markdown_core_node *node;
    const unsigned char *body;
    bufsize_t body_len;

    markdown_core_strbuf_init(parser->mem, &literal, 0);

    while (i < body_end) {
        if (slash_count > 1 && data[i] == '\\' && i + 1 < body_end && data[i + 1] == close_char) {
            markdown_core_strbuf_putc(&literal, close_char);
            i += 2;
            continue;
        }

        markdown_core_strbuf_putc(&literal, data[i]);
        i++;
    }

    /* The same padding rule as the dollar forms: Q18 says it applies to
     * `\(...\)` and `\[...\]` too, and no oracle row covered that until this
     * step added two. */
    body = literal.ptr;
    body_len = literal.size;
    strip_formula_padding(&body, &body_len);
    node = make_formula_node(extension, parser, MARKDOWN_CORE_NODE_FORMULA, mode, body, body_len);
    markdown_core_strbuf_free(&literal);
    return node;
}

static delimiter *insert_formula(const markdown_core_syntax_extension *extension, markdown_core_parser *parser,
                                 markdown_core_inline_parser *inline_parser, delimiter *opener, delimiter *closer) {
    markdown_core_chunk *chunk = markdown_core_inline_parser_get_chunk(inline_parser);
    markdown_core_node *opener_node = markdown_core_delimiter_node(opener);
    markdown_core_node *closer_node = markdown_core_delimiter_node(closer);
    delimiter *res = markdown_core_delimiter_next(closer);
    markdown_core_node *formula = NULL;
    bufsize_t body_start = markdown_core_delimiter_position(opener);
    bufsize_t body_end = markdown_core_delimiter_position(closer) - markdown_core_delimiter_length(closer);
    markdown_core_formula_mode mode = mode_for_delim(markdown_core_delimiter_rule_of(opener));
    const unsigned char *literal = chunk->data + body_start;
    bufsize_t literal_len = body_end - body_start;

    if (markdown_core_delimiter_rule_of(opener) != markdown_core_delimiter_rule_of(closer)) {
        goto done;
    }

    if (markdown_core_delimiter_length(opener) != markdown_core_delimiter_length(closer) &&
        is_backslash_delim(markdown_core_delimiter_rule_of(opener))) {
        goto done;
    }

    if (markdown_core_delimiter_rule_of(opener) == FORMULA_DELIM_DOLLAR_INLINE && literal_len > 0 &&
        literal[0] == '`') {
        if (literal_len < 2 || literal[literal_len - 1] != '`') {
            goto done;
        }

        literal++;
        literal_len -= 2;
    }

    if (is_backslash_delim(markdown_core_delimiter_rule_of(opener))) {
        formula = make_backslash_delimited_formula(extension, parser, mode, chunk->data, body_start, body_end, 2,
                                                   mode == MARKDOWN_CORE_FORMULA_MODE_STANDALONE ? ']' : ')');
    } else {
        strip_formula_padding(&literal, &literal_len);
        formula = make_formula_node(extension, parser, MARKDOWN_CORE_NODE_FORMULA, mode, literal, literal_len);
    }

    if (!formula) {
        goto done;
    }

    formula->start_line = opener_node->start_line;
    formula->end_line = closer_node->end_line;
    formula->start_column = opener_node->start_column;
    formula->end_column = closer_node->end_column;

    if (markdown_core_node_insert_before(opener_node, formula)) {
        free_nodes_through(opener_node, closer_node);
    } else {
        markdown_core_node_free(formula);
    }

done:
    remove_delimiters(inline_parser, opener, closer);
    return res;
}

static const char *get_type_string(const markdown_core_syntax_extension *extension, markdown_core_node *node) {
    if (node->type == MARKDOWN_CORE_NODE_FORMULA) {
        return "formula";
    }

    if (node->type == MARKDOWN_CORE_NODE_FORMULA_BLOCK) {
        return "formula_block";
    }

    return "<unknown>";
}

static int can_contain(const markdown_core_syntax_extension *extension, markdown_core_node *node,
                       markdown_core_node_type child_type) {
    if (is_formula_node(node)) {
        return 0;
    }

    return 0;
}

static int accepts_lines(const markdown_core_syntax_extension *extension, markdown_core_node *node) {
    return node && node->type == MARKDOWN_CORE_NODE_FORMULA_BLOCK;
}

static int info_is_formula(markdown_core_chunk *info) {
    return info->len == 7 && memcmp(info->data, "formula", 7) == 0;
}

static markdown_core_node *new_formula_block_from_literal(const markdown_core_syntax_extension *extension,
                                                          markdown_core_mem *mem, markdown_core_node *oldnode,
                                                          const unsigned char *literal, bufsize_t literal_len) {
    markdown_core_node *formula =
        markdown_core_node_new_with_mem_and_ext(MARKDOWN_CORE_NODE_FORMULA_BLOCK, mem, extension);
    if (!formula) {
        return NULL;
    }
    if (!get_formula(formula)) {
        markdown_core_node_free(formula);
        return NULL;
    }

    get_formula(formula)->mode = MARKDOWN_CORE_FORMULA_MODE_STANDALONE;
    formula->start_line = oldnode->start_line;
    formula->start_column = oldnode->start_column;
    formula->end_line = oldnode->end_line;
    formula->end_column = oldnode->end_column;
    if (!set_formula_literal_trimmed(formula, literal, literal_len)) {
        markdown_core_node_free(formula);
        return NULL;
    }
    return formula;
}

static int replace_with_formula_block(const markdown_core_syntax_extension *extension, markdown_core_mem *mem,
                                      markdown_core_node *oldnode, const unsigned char *literal,
                                      bufsize_t literal_len) {
    markdown_core_node *formula = new_formula_block_from_literal(extension, mem, oldnode, literal, literal_len);
    if (!formula) {
        return 0;
    }

    if (markdown_core_node_replace(oldnode, formula)) {
        markdown_core_node_free(oldnode);
        return 1;
    }
    markdown_core_node_free(formula);
    return 0;
}

static void postprocess_node(const markdown_core_syntax_extension *extension, markdown_core_parser *parser,
                             markdown_core_node *node) {
    markdown_core_node *child;
    markdown_core_node *next;

    if (node->type == MARKDOWN_CORE_NODE_FORMULA_BLOCK) {
        node_formula *formula = get_formula(node);
        if (formula && !formula->literal.data) {
            /* The literal is copied OUT of `node->content` and the content is
             * then cleared, so a failed copy would leave the chunk borrowing a
             * buffer this very statement empties. */
            if (!set_formula_literal_trimmed(node, node->content.ptr, node->content.size)) {
                parser->oom = true;
            }
            markdown_core_strbuf_clear(&node->content);
        }
        return;
    }

    if (node->type == MARKDOWN_CORE_NODE_CODE_BLOCK && info_is_formula(&node->as.code.info)) {
        if (!replace_with_formula_block(extension, parser->mem, node, node->as.code.literal.data,
                                        node->as.code.literal.len)) {
            parser->oom = true;
        }
        return;
    }

    if (node->type == MARKDOWN_CORE_NODE_PARAGRAPH && node->first_child && node->first_child == node->last_child &&
        node->first_child->type == MARKDOWN_CORE_NODE_FORMULA && is_standalone_formula_node(node->first_child)) {
        node_formula *formula = get_formula(node->first_child);
        if (formula) {
            if (!replace_with_formula_block(extension, parser->mem, node, formula->literal.data,
                                            formula->literal.len)) {
                parser->oom = true;
            }
            return;
        }
    }

    child = node->first_child;
    while (child) {
        next = child->next;
        postprocess_node(extension, parser, child);
        child = next;
    }
}

static markdown_core_node *postprocess(const markdown_core_syntax_extension *extension, markdown_core_parser *parser,
                                       markdown_core_node *root) {
    postprocess_node(extension, parser, root);
    return root;
}

/* `$` and `\\` open a formula, and that is the whole set. `\\` is in the dispatch
 * set and NOT the terminator set: `is_core_special_character` refuses it there
 * anyway, and it must stay in dispatch because `handle_backslash` asks whether any
 * extension claims `\\` before taking a core fast path. */
const markdown_core_syntax_extension MARKDOWN_CORE_EXTENSION_FORMULA = {
    .name = "formula",
    .match_inline = match,
    .last_block_matches = formula_block_matches,
    .try_opening_block = try_opening_formula_block,
    .postprocess_func = postprocess,
    .get_type_string_func = get_type_string,
    .can_contain_func = can_contain,
    .accepts_lines_func = accepts_lines,
    .opaque_alloc_func = formula_opaque_alloc,
    .opaque_free_func = formula_opaque_free,
    .insert_inline_from_delim = insert_formula,
    .terminates_text = "$",
    .dispatch = "$\\",
};
