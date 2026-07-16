#include "formula.h"

#include <assert.h>
#include <string.h>

#include <buffer.h>
#include <chunk.h>
#include <markdown_core_ctype.h>
#include <node.h>
#include <parser.h>

#include "ext_scanners.h"
#include "extension.h"

#define FORMULA_DELIM_DOLLAR_INLINE 1
#define FORMULA_DELIM_DOLLAR_DISPLAY 2
#define FORMULA_DELIM_LATEX_BACKSLASH_INLINE 3
#define FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY 4

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

static void formula_opaque_alloc(markdown_core_extension *extension, markdown_core_mem *mem, markdown_core_node *node) {
    /* A NULL payload is tolerated: every accessor goes through get_formula
     * and treats the node as formula-less. */
    if (is_formula_node(node)) {
        node->as.opaque = mem->calloc(1, sizeof(node_formula));
    }
}

static void formula_opaque_free(markdown_core_extension *extension, markdown_core_mem *mem, markdown_core_node *node) {
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
    markdown_core_chunk_to_cstr(markdown_core_node_mem(node), &formula->literal);
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

static markdown_core_node *make_formula_node(markdown_core_extension *extension, markdown_core_parser *parser,
                                             markdown_core_node_type node_type, markdown_core_formula_mode mode,
                                             const unsigned char *literal, bufsize_t literal_len) {
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
    set_formula_literal_bytes(node, literal, literal_len);
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

static int scan_formula_block_open(const unsigned char *data, bufsize_t len, bufsize_t pos,
                                   int latex_formula_delimiters, int dollar_formula_delimiters) {
    if (latex_formula_delimiters && pos + 3 <= len && data[pos] == '\\' && data[pos + 1] == '\\' &&
        data[pos + 2] == '[' && has_only_spaces_until_line_end(data, len, pos + 3)) {
        return FORMULA_BLOCK_DELIM_LATEX_BACKSLASH;
    }

    if (dollar_formula_delimiters && pos + 2 <= len && data[pos] == '$' && data[pos + 1] == '$' &&
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

static markdown_core_node *try_opening_formula_block(markdown_core_extension *extension, int indented,
                                                     markdown_core_parser *parser, markdown_core_node *parent_container,
                                                     unsigned char *input, int len) {
    int block_delim;
    markdown_core_node *node;
    node_formula *formula;
    int first_nonspace = markdown_core_parser_get_first_nonspace(parser);

    if (indented) {
        return NULL;
    }

    block_delim = scan_formula_block_open(input, (bufsize_t)len, (bufsize_t)first_nonspace,
                                          parser->options & MARKDOWN_CORE_OPT_LATEX_FORMULA_DELIMITERS,
                                          parser->options & MARKDOWN_CORE_OPT_DOLLAR_FORMULA_DELIMITERS);
    if (block_delim == FORMULA_BLOCK_DELIM_NONE) {
        return NULL;
    }

    node =
        markdown_core_parser_add_child(parser, parent_container, MARKDOWN_CORE_NODE_FORMULA_BLOCK, first_nonspace + 1);
    if (!node) {
        return NULL;
    }

    markdown_core_node_set_extension(node, extension);
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

static int formula_block_matches(markdown_core_extension *extension, markdown_core_parser *parser, unsigned char *input,
                                 int len, markdown_core_node *container) {
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

static markdown_core_node *match_formula_delimiter(markdown_core_parser *parser,
                                                   markdown_core_inline_parser *inline_parser, unsigned char delim_char,
                                                   bufsize_t len, int can_open, int can_close) {
    markdown_core_node *node = make_delimiter_text(parser, inline_parser, len);

    if (!node) {
        parser->oom = true;
        return NULL;
    }

    if (can_open || can_close) {
        markdown_core_inline_parser_push_delimiter(inline_parser, delim_char, can_open, can_close, node);
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

static int latex_formula_delimiters_enabled(markdown_core_parser *parser) {
    return parser->options & MARKDOWN_CORE_OPT_LATEX_FORMULA_DELIMITERS;
}

static int dollar_formula_delimiters_enabled(markdown_core_parser *parser) {
    return parser->options & MARKDOWN_CORE_OPT_DOLLAR_FORMULA_DELIMITERS;
}

static markdown_core_node *match(markdown_core_extension *extension, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_parser *inline_parser) {
    markdown_core_chunk *chunk = markdown_core_inline_parser_get_chunk(inline_parser);
    int offset = markdown_core_inline_parser_get_offset(inline_parser);
    int len = (int)chunk->len;
    bufsize_t opener_len;
    bufsize_t closer_len;

    if (character == '$') {
        if (!dollar_formula_delimiters_enabled(parser)) {
            return NULL;
        }

        if (scan_formula_dollar_display_open(chunk->data, len, offset)) {
            return match_formula_delimiter(parser, inline_parser, FORMULA_DELIM_DOLLAR_DISPLAY, 2, 1, 1);
        }

        if (scan_formula_dollar_inline_open(chunk->data, len, offset)) {
            return match_formula_delimiter(parser, inline_parser, FORMULA_DELIM_DOLLAR_INLINE, 1,
                                           dollar_inline_can_open(chunk, (bufsize_t)offset),
                                           dollar_inline_can_close(chunk, (bufsize_t)offset));
        }
    } else if (character == '\\') {
        if (latex_formula_delimiters_enabled(parser)) {
            opener_len = scan_formula_latex_backslash_display_open(chunk->data, len, offset);
            if (opener_len) {
                return match_formula_delimiter(parser, inline_parser, FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY, opener_len,
                                               1, 0);
            }

            opener_len = scan_formula_latex_backslash_inline_open(chunk->data, len, offset);
            if (opener_len) {
                return match_formula_delimiter(parser, inline_parser, FORMULA_DELIM_LATEX_BACKSLASH_INLINE, opener_len,
                                               1, 0);
            }
        }

        if (latex_formula_delimiters_enabled(parser)) {
            closer_len = scan_backslash_close(chunk->data, chunk->len, offset, ']', 2);
            if (closer_len) {
                return match_formula_delimiter(parser, inline_parser, FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY, closer_len,
                                               0, 1);
            }

            closer_len = scan_backslash_close(chunk->data, chunk->len, offset, ')', 2);
            if (closer_len) {
                return match_formula_delimiter(parser, inline_parser, FORMULA_DELIM_LATEX_BACKSLASH_INLINE, closer_len,
                                               0, 1);
            }
        }
    }

    return NULL;
}

static markdown_core_formula_mode mode_for_delim(unsigned char delim_char) {
    return delim_char == FORMULA_DELIM_DOLLAR_DISPLAY || delim_char == FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY
               ? MARKDOWN_CORE_FORMULA_MODE_STANDALONE
               : MARKDOWN_CORE_FORMULA_MODE_EMBEDDED;
}

static int is_backslash_delim(unsigned char delim_char) {
    return delim_char == FORMULA_DELIM_LATEX_BACKSLASH_INLINE || delim_char == FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY;
}

static void remove_delimiters(markdown_core_inline_parser *inline_parser, delimiter *opener, delimiter *closer) {
    delimiter *delim = closer;

    while (delim != NULL && delim != opener) {
        delimiter *previous = delim->previous;
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

static markdown_core_node *make_backslash_delimited_formula(markdown_core_extension *extension,
                                                            markdown_core_parser *parser,
                                                            markdown_core_formula_mode mode, const unsigned char *data,
                                                            bufsize_t body_start, bufsize_t body_end, int slash_count,
                                                            unsigned char close_char) {
    markdown_core_strbuf literal;
    bufsize_t i = body_start;
    markdown_core_node *node;

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

    node = make_formula_node(extension, parser, MARKDOWN_CORE_NODE_FORMULA, mode, literal.ptr, literal.size);
    markdown_core_strbuf_free(&literal);
    return node;
}

static delimiter *insert_formula(markdown_core_extension *extension, markdown_core_parser *parser,
                                 markdown_core_inline_parser *inline_parser, delimiter *opener, delimiter *closer) {
    markdown_core_chunk *chunk = markdown_core_inline_parser_get_chunk(inline_parser);
    markdown_core_node *opener_node = opener->inl_text;
    markdown_core_node *closer_node = closer->inl_text;
    delimiter *res = closer->next;
    markdown_core_node *formula = NULL;
    bufsize_t body_start = opener->position;
    bufsize_t body_end = closer->position - closer->length;
    markdown_core_formula_mode mode = mode_for_delim((unsigned char)opener->delim_char);
    const unsigned char *literal = chunk->data + body_start;
    bufsize_t literal_len = body_end - body_start;

    if (opener->delim_char != closer->delim_char) {
        goto done;
    }

    if (opener->length != closer->length && is_backslash_delim((unsigned char)opener->delim_char)) {
        goto done;
    }

    if (opener->delim_char == FORMULA_DELIM_DOLLAR_INLINE && literal_len > 0 && literal[0] == '`') {
        if (literal_len < 2 || literal[literal_len - 1] != '`') {
            goto done;
        }

        literal++;
        literal_len -= 2;
    }

    if (is_backslash_delim((unsigned char)opener->delim_char)) {
        formula = make_backslash_delimited_formula(extension, parser, mode, chunk->data, body_start, body_end, 2,
                                                   mode == MARKDOWN_CORE_FORMULA_MODE_STANDALONE ? ']' : ')');
    } else {
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

static const char *get_type_string(markdown_core_extension *extension, markdown_core_node *node) {
    if (node->type == MARKDOWN_CORE_NODE_FORMULA) {
        return "formula";
    }

    if (node->type == MARKDOWN_CORE_NODE_FORMULA_BLOCK) {
        return "formula_block";
    }

    return "<unknown>";
}

static int can_contain(markdown_core_extension *extension, markdown_core_node *node,
                       markdown_core_node_type child_type) {
    if (is_formula_node(node)) {
        return 0;
    }

    return 0;
}

static int accepts_lines(markdown_core_extension *extension, markdown_core_node *node) {
    return node && node->type == MARKDOWN_CORE_NODE_FORMULA_BLOCK;
}

static int info_is_formula(markdown_core_chunk *info) {
    return info->len == 7 && memcmp(info->data, "formula", 7) == 0;
}

static markdown_core_node *new_formula_block_from_literal(markdown_core_extension *extension, markdown_core_mem *mem,
                                                          markdown_core_node *oldnode, const unsigned char *literal,
                                                          bufsize_t literal_len) {
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
    set_formula_literal_trimmed(formula, literal, literal_len);
    return formula;
}

static int replace_with_formula_block(markdown_core_extension *extension, markdown_core_mem *mem,
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

static void postprocess_node(markdown_core_extension *extension, markdown_core_parser *parser,
                             markdown_core_node *node) {
    markdown_core_node *child;
    markdown_core_node *next;

    if (node->type == MARKDOWN_CORE_NODE_FORMULA_BLOCK) {
        node_formula *formula = get_formula(node);
        if (formula && !formula->literal.data) {
            set_formula_literal_trimmed(node, node->content.ptr, node->content.size);
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

static markdown_core_node *postprocess(markdown_core_extension *extension, markdown_core_parser *parser,
                                       markdown_core_node *root) {
    postprocess_node(extension, parser, root);
    return root;
}

static const unsigned char formula_special_chars[] = {
    '$',
    '\\',
    FORMULA_DELIM_DOLLAR_INLINE,
    FORMULA_DELIM_DOLLAR_DISPLAY,
    FORMULA_DELIM_LATEX_BACKSLASH_INLINE,
    FORMULA_DELIM_LATEX_BACKSLASH_DISPLAY,
};

static const markdown_core_extension formula_extension = {
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
    .special_inline_chars = formula_special_chars,
    .special_inline_char_count = sizeof(formula_special_chars),
    .emphasis = true,
};

markdown_core_extension *markdown_core_formula_extension(void) {
    // Immutable descriptor; the cast keeps the pre-existing pointer plumbing
    // without permitting writes (see extension.h).
    return (markdown_core_extension *)&formula_extension;
}
