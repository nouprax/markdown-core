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
#include "inline_util.h"

enum {
    FORMULA_DELIMITER_DOLLAR_INLINE = 0,
    FORMULA_DELIMITER_DOLLAR_DISPLAY,
    FORMULA_DELIMITER_LATEX_BACKSLASH_INLINE,
    FORMULA_DELIMITER_LATEX_BACKSLASH_DISPLAY,
    FORMULA_DELIMITER_COUNT,
};

#define FORMULA_BLOCK_DELIM_NONE 0
#define FORMULA_BLOCK_DELIM_LATEX_BACKSLASH 1
#define FORMULA_BLOCK_DELIM_DOLLAR 2

typedef struct {
    markdown_core_chunk literal;
    markdown_core_formula_mode mode;
    /* A borrowed inline payload drops the escape before this close byte when
     * it is materialized. Zero means a byte-for-byte copy. */
    unsigned char escaped_close;
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

static int clone_formula_literal(
    markdown_core_mem *mem,
    const unsigned char *data,
    markdown_core_bufsize len,
    unsigned char escaped_close,
    markdown_core_chunk *result
) {
    markdown_core_bufsize input_offset;
    markdown_core_bufsize output_offset = 0;
    markdown_core_bufsize escaped_count = 0;
    markdown_core_bufsize output_len;
    unsigned char *copy;

    if (escaped_close) {
        for (input_offset = 0; input_offset + 1 < len; input_offset++) {
            if (data[input_offset] == '\\' && data[input_offset + 1] == escaped_close) {
                escaped_count++;
                input_offset++;
            }
        }
    }

    output_len = len - escaped_count;
    copy = (unsigned char *)mem->calloc(mem, (size_t)output_len + 1, 1);
    if (!copy) {
        return 0;
    }

    for (input_offset = 0; input_offset < len; input_offset++) {
        if (escaped_close && data[input_offset] == '\\' && input_offset + 1 < len &&
            data[input_offset + 1] == escaped_close) {
            copy[output_offset++] = escaped_close;
            input_offset++;
        } else {
            copy[output_offset++] = data[input_offset];
        }
    }
    assert(output_offset == output_len);

    result->data = copy;
    result->len = output_len;
    result->alloc = 1;
    return 1;
}

static int materialize_formula_literal(markdown_core_node *node) {
    node_formula *formula = get_formula(node);
    markdown_core_chunk materialized = MARKDOWN_CORE_CHUNK_EMPTY;
    markdown_core_chunk previous;

    if (!formula) {
        return 0;
    }
    if (formula->literal.alloc && !formula->escaped_close) {
        return 1;
    }
    if (!clone_formula_literal(
            markdown_core_node_mem(node),
            formula->literal.data,
            formula->literal.len,
            formula->escaped_close,
            &materialized
        )) {
        return 0;
    }

    previous = formula->literal;
    formula->literal = materialized;
    formula->escaped_close = 0;
    if (previous.alloc) {
        markdown_core_node_mem(node)->free(markdown_core_node_mem(node), previous.data);
    }
    return 1;
}

static int is_standalone_formula_node(markdown_core_node *node) {
    node_formula *formula = get_formula(node);

    if (!formula) {
        return 0;
    }

    return formula->mode == MARKDOWN_CORE_FORMULA_MODE_STANDALONE;
}

const char *markdown_core_extensions_get_formula_literal(markdown_core_node *node) {
    if (!materialize_formula_literal(node)) {
        return NULL;
    }

    return (const char *)get_formula(node)->literal.data;
}

int markdown_core_extensions_set_formula_literal(markdown_core_node *node, const char *literal) {
    node_formula *formula = get_formula(node);
    if (!formula) {
        return 0;
    }

    if (!markdown_core_chunk_set_cstr(markdown_core_node_mem(node), &formula->literal, literal)) {
        return 0;
    }
    formula->escaped_close = 0;
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
        node->as.opaque = mem->calloc(mem, 1, sizeof(node_formula));
    }
}

static void formula_opaque_free(markdown_core_extension *extension, markdown_core_mem *mem, markdown_core_node *node) {
    node_formula *formula = (node_formula *)node->as.opaque;
    if (!formula) {
        return;
    }

    markdown_core_chunk_free(mem, &formula->literal);
    mem->free(mem, formula);
}

static int set_formula_literal_bytes(markdown_core_node *node, const unsigned char *data, markdown_core_bufsize len) {
    node_formula *formula = get_formula(node);
    markdown_core_chunk literal = MARKDOWN_CORE_CHUNK_EMPTY;
    if (!formula) {
        return 0;
    }

    if (!clone_formula_literal(markdown_core_node_mem(node), data, len, 0, &literal)) {
        return 0;
    }
    markdown_core_chunk_free(markdown_core_node_mem(node), &formula->literal);
    formula->literal = literal;
    formula->escaped_close = 0;
    return 1;
}

static int set_formula_literal_trimmed(markdown_core_node *node, const unsigned char *data, markdown_core_bufsize len) {
    while (len > 0 && markdown_core_isspace(data[0])) {
        data++;
        len--;
    }

    while (len > 0 && markdown_core_isspace(data[len - 1])) {
        len--;
    }

    return set_formula_literal_bytes(node, data, len);
}

static markdown_core_node *make_borrowed_formula_node(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_formula_mode mode,
    markdown_core_chunk *source,
    markdown_core_bufsize literal_start,
    markdown_core_bufsize literal_len,
    unsigned char escaped_close
) {
    markdown_core_node *node =
        markdown_core_node_new_with_mem_and_ext(MARKDOWN_CORE_NODE_FORMULA, parser->mem, extension);
    node_formula *formula;

    if (!node) {
        parser->oom = true;
        return NULL;
    }
    formula = get_formula(node);
    if (!formula) {
        parser->oom = true;
        markdown_core_node_free(node);
        return NULL;
    }

    formula->mode = mode;
    formula->literal = markdown_core_chunk_borrow(source, literal_start, literal_len);
    formula->escaped_close = escaped_close;
    return node;
}

static int scan_formula_block_open(const unsigned char *data, markdown_core_bufsize len, markdown_core_bufsize pos) {
    if (pos + 3 <= len && data[pos] == '\\' && data[pos + 1] == '\\' && data[pos + 2] == '[' &&
        markdown_core_ext_has_only_spaces_until_line_end(data, len, pos + 3)) {
        return FORMULA_BLOCK_DELIM_LATEX_BACKSLASH;
    }

    if (pos + 2 <= len && data[pos] == '$' && data[pos + 1] == '$' &&
        markdown_core_ext_has_only_spaces_until_line_end(data, len, pos + 2)) {
        return FORMULA_BLOCK_DELIM_DOLLAR;
    }

    return FORMULA_BLOCK_DELIM_NONE;
}

static int scan_formula_block_close(
    const unsigned char *data,
    markdown_core_bufsize len,
    markdown_core_bufsize pos,
    int block_delim
) {
    if (block_delim == FORMULA_BLOCK_DELIM_LATEX_BACKSLASH) {
        return pos + 3 <= len && data[pos] == '\\' && data[pos + 1] == '\\' && data[pos + 2] == ']' &&
               markdown_core_ext_has_only_spaces_until_line_end(data, len, pos + 3);
    }

    if (block_delim == FORMULA_BLOCK_DELIM_DOLLAR) {
        return pos + 2 <= len && data[pos] == '$' && data[pos + 1] == '$' &&
               markdown_core_ext_has_only_spaces_until_line_end(data, len, pos + 2);
    }

    return 0;
}

static markdown_core_node *try_opening_formula_block(
    markdown_core_extension *extension,
    int indented,
    markdown_core_parser *parser,
    markdown_core_node *parent_container,
    unsigned char *input,
    int len
) {
    int block_delim;
    markdown_core_node *node;
    node_formula *formula;
    int first_nonspace = markdown_core_parser_get_first_nonspace(parser);

    if (indented) {
        return NULL;
    }

    block_delim = scan_formula_block_open(input, (markdown_core_bufsize)len, (markdown_core_bufsize)first_nonspace);
    if (block_delim == FORMULA_BLOCK_DELIM_NONE) {
        return NULL;
    }

    node =
        markdown_core_parser_add_child(parser, parent_container, MARKDOWN_CORE_NODE_FORMULA_BLOCK, first_nonspace + 1);
    if (!node) {
        return NULL;
    }

    markdown_core_node_set_extension(node, extension);
    node->as.opaque = parser->mem->calloc(parser->mem, 1, sizeof(node_formula));

    formula = get_formula(node);
    if (!formula) {
        parser->oom = true;
        return NULL;
    }

    formula->mode = MARKDOWN_CORE_FORMULA_MODE_STANDALONE;
    formula->block_delim = block_delim;
    /* `$$` spells two bytes, `\\[` three; the CodeBlock fence precedent,
     * minus the info record the scanners accept no text for. */
    markdown_core_parser_capture_marker(
        parser,
        node,
        MARKDOWN_CORE_CONCRETE_FENCE_OPEN,
        first_nonspace,
        block_delim == FORMULA_BLOCK_DELIM_LATEX_BACKSLASH ? 3 : 2
    );
    markdown_core_parser_advance_offset(parser, (char *)input, len - markdown_core_parser_get_offset(parser), false);
    return node;
}

static int formula_block_matches(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    unsigned char *input,
    int len,
    markdown_core_node *container
) {
    node_formula *formula = get_formula(container);
    int first_nonspace = markdown_core_parser_get_first_nonspace(parser);

    if (!formula || formula->closed) {
        return 0;
    }

    if (scan_formula_block_close(
            input,
            (markdown_core_bufsize)len,
            (markdown_core_bufsize)first_nonspace,
            formula->block_delim
        )) {
        formula->closed = 1;
        markdown_core_parser_capture_marker(
            parser,
            container,
            MARKDOWN_CORE_CONCRETE_FENCE_CLOSE,
            first_nonspace,
            formula->block_delim == FORMULA_BLOCK_DELIM_LATEX_BACKSLASH ? 3 : 2
        );
        markdown_core_parser_advance_offset(
            parser,
            (char *)input,
            len - markdown_core_parser_get_offset(parser),
            false
        );
    }

    return 1;
}

static markdown_core_node *match_formula_delimiter(
    markdown_core_inline_parser *inline_parser,
    uint16_t kind,
    markdown_core_bufsize len,
    int can_open,
    int can_close
) {
    markdown_core_bufsize end_offset =
        (markdown_core_bufsize)markdown_core_inline_parser_get_offset(inline_parser) + len;

    if (can_open || can_close) {
        return markdown_core_inline_parser_consume_delimiter(inline_parser, kind, can_open, can_close, end_offset);
    }
    return markdown_core_inline_parser_consume_text(inline_parser, end_offset);
}

static int dollar_inline_can_open(markdown_core_chunk *chunk, markdown_core_bufsize offset) {
    return offset + 1 < chunk->len && !markdown_core_isspace((char)chunk->data[offset + 1]);
}

static int dollar_inline_can_close(markdown_core_chunk *chunk, markdown_core_bufsize offset) {
    return offset > 0 && !markdown_core_isspace((char)chunk->data[offset - 1]) &&
           (offset + 1 >= chunk->len || !markdown_core_isdigit((char)chunk->data[offset + 1]));
}

static markdown_core_bufsize scan_backslash_close(
    const unsigned char *data,
    markdown_core_bufsize len,
    markdown_core_bufsize offset,
    unsigned char close_char,
    int slash_count
) {
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
        return (markdown_core_bufsize)(slash_count + 1);
    }

    return 0;
}

static markdown_core_node *match(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node *parent,
    unsigned char character,
    markdown_core_inline_parser *inline_parser
) {
    markdown_core_chunk *chunk = markdown_core_inline_parser_get_chunk(inline_parser);
    int offset = markdown_core_inline_parser_get_offset(inline_parser);
    int len = (int)chunk->len;
    markdown_core_bufsize opener_len;
    markdown_core_bufsize closer_len;

    if (character == '$') {
        if (scan_formula_dollar_display_open(chunk->data, len, offset)) {
            return match_formula_delimiter(inline_parser, FORMULA_DELIMITER_DOLLAR_DISPLAY, 2, 1, 1);
        }

        if (scan_formula_dollar_inline_open(chunk->data, len, offset)) {
            return match_formula_delimiter(
                inline_parser,
                FORMULA_DELIMITER_DOLLAR_INLINE,
                1,
                dollar_inline_can_open(chunk, (markdown_core_bufsize)offset),
                dollar_inline_can_close(chunk, (markdown_core_bufsize)offset)
            );
        }
    } else if (character == '\\') {
        opener_len = scan_formula_latex_backslash_display_open(chunk->data, len, offset);
        if (opener_len) {
            return match_formula_delimiter(inline_parser, FORMULA_DELIMITER_LATEX_BACKSLASH_DISPLAY, opener_len, 1, 0);
        }

        opener_len = scan_formula_latex_backslash_inline_open(chunk->data, len, offset);
        if (opener_len) {
            return match_formula_delimiter(inline_parser, FORMULA_DELIMITER_LATEX_BACKSLASH_INLINE, opener_len, 1, 0);
        }

        closer_len = scan_backslash_close(chunk->data, chunk->len, offset, ']', 2);
        if (closer_len) {
            return match_formula_delimiter(inline_parser, FORMULA_DELIMITER_LATEX_BACKSLASH_DISPLAY, closer_len, 0, 1);
        }

        closer_len = scan_backslash_close(chunk->data, chunk->len, offset, ')', 2);
        if (closer_len) {
            return match_formula_delimiter(inline_parser, FORMULA_DELIMITER_LATEX_BACKSLASH_INLINE, closer_len, 0, 1);
        }
    }

    return NULL;
}

static markdown_core_formula_mode mode_for_kind(uint16_t kind) {
    return kind == FORMULA_DELIMITER_DOLLAR_DISPLAY || kind == FORMULA_DELIMITER_LATEX_BACKSLASH_DISPLAY
               ? MARKDOWN_CORE_FORMULA_MODE_STANDALONE
               : MARKDOWN_CORE_FORMULA_MODE_EMBEDDED;
}

static int is_backslash_kind(uint16_t kind) {
    return kind == FORMULA_DELIMITER_LATEX_BACKSLASH_INLINE || kind == FORMULA_DELIMITER_LATEX_BACKSLASH_DISPLAY;
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

/* Inline math carries the same padding rule as a code span: when the content
 * both begins and ends with a space and is not entirely spaces, one is removed
 * from each end, so `$ x $` and `$x$` mean the same thing. Line endings count
 * as spaces for this purpose.
 *
 * The rule is the ecosystem's, not an invention here — micromark-extension-math
 * states it, and this repository's own code spans already implement it in
 * S_normalize_code. Inline math not following it was the inconsistency.
 *
 * The span is only narrowed: the literal borrows the source chunk, so there is
 * nothing to rewrite. */
/* A carriage return never reaches inline content: the parser normalizes line
 * endings while reading input, so by this point a CRLF source has already
 * become LF. Checking for one would be an unreachable branch. */
static bool is_inline_math_pad(unsigned char c) { return c == ' ' || c == '\n'; }

static void strip_inline_math_padding(
    const markdown_core_chunk *chunk,
    markdown_core_bufsize *start,
    markdown_core_bufsize *len
) {
    markdown_core_bufsize i;
    bool contains_nonspace = false;

    if (*len < 2) {
        return;
    }
    for (i = 0; i < *len; i++) {
        if (!is_inline_math_pad(chunk->data[*start + i])) {
            contains_nonspace = true;
            break;
        }
    }
    if (!contains_nonspace) {
        return;
    }
    if (is_inline_math_pad(chunk->data[*start]) && is_inline_math_pad(chunk->data[*start + *len - 1])) {
        *start += 1;
        *len -= 2;
    }
}

static markdown_core_delimiter_result insert_formula(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_inline_parser *inline_parser,
    const markdown_core_delimiter_match *match
) {
    markdown_core_chunk *chunk = markdown_core_inline_parser_get_chunk(inline_parser);
    markdown_core_bufsize literal_start = match->opener_end;
    markdown_core_bufsize literal_end = match->closer_start;
    markdown_core_bufsize literal_len;
    markdown_core_formula_mode mode;
    unsigned char escaped_close = 0;
    markdown_core_node *formula;

    if (match->kind >= FORMULA_DELIMITER_COUNT || literal_end < literal_start || literal_end > chunk->len) {
        return MARKDOWN_CORE_DELIMITER_INVALID;
    }
    if (is_backslash_kind(match->kind) && match->opener_length != match->closer_length) {
        return MARKDOWN_CORE_DELIMITER_OK;
    }

    literal_len = literal_end - literal_start;
    if (match->kind == FORMULA_DELIMITER_DOLLAR_INLINE && literal_len > 0 && chunk->data[literal_start] == '`') {
        if (literal_len < 2 || chunk->data[literal_end - 1] != '`') {
            return MARKDOWN_CORE_DELIMITER_OK;
        }
        literal_start++;
        literal_len -= 2;
    }

    strip_inline_math_padding(chunk, &literal_start, &literal_len);

    mode = mode_for_kind(match->kind);
    if (is_backslash_kind(match->kind)) {
        escaped_close = mode == MARKDOWN_CORE_FORMULA_MODE_STANDALONE ? ']' : ')';
    }

    formula = make_borrowed_formula_node(extension, parser, mode, chunk, literal_start, literal_len, escaped_close);
    if (!formula) {
        return MARKDOWN_CORE_DELIMITER_OOM;
    }

    formula->start_line = match->opener_node->start_line;
    formula->end_line = match->closer_node->end_line;
    formula->start_column = match->opener_node->start_column;
    formula->end_column = match->closer_node->end_column;

    markdown_core_node_insert_before_unchecked(match->opener_node, formula);
    free_nodes_through(match->opener_node, match->closer_node);
    /* Concrete bookkeeping the engine cannot infer for a RANGE reduce: the
     * delimiter runs were consumed here (the OK returns above consume
     * nothing), and the interior's parsed records now claim consumption the
     * borrowed raw literal no longer shows — retract them. */
    markdown_core_inline_parser_concrete_use_endpoints(inline_parser, match);
    markdown_core_inline_parser_concrete_reinterpret(inline_parser, match->opener_end, match->closer_start);
    return MARKDOWN_CORE_DELIMITER_OK;
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

static int accepts_lines(markdown_core_extension *extension, markdown_core_node *node) {
    return node && node->type == MARKDOWN_CORE_NODE_FORMULA_BLOCK;
}

static int materialize_inline(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node *node
) {
    (void)parser;
    if (!node || node->extension != extension || node->type != MARKDOWN_CORE_NODE_FORMULA) {
        return 1;
    }
    return materialize_formula_literal(node);
}

static int info_is_formula(markdown_core_chunk *info) {
    return info->len == 7 && memcmp(info->data, "formula", 7) == 0;
}

static markdown_core_node *new_formula_block_shell(
    markdown_core_extension *extension,
    markdown_core_mem *mem,
    markdown_core_node *oldnode
) {
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
    return formula;
}

static markdown_core_node *new_formula_block_from_literal(
    markdown_core_extension *extension,
    markdown_core_mem *mem,
    markdown_core_node *oldnode,
    const unsigned char *literal,
    markdown_core_bufsize literal_len
) {
    markdown_core_node *formula = new_formula_block_shell(extension, mem, oldnode);
    if (!formula) {
        return NULL;
    }
    if (!set_formula_literal_trimmed(formula, literal, literal_len)) {
        markdown_core_node_free(formula);
        return NULL;
    }
    return formula;
}

static markdown_core_node *replace_with_formula_block(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node *oldnode,
    const unsigned char *literal,
    markdown_core_bufsize literal_len
) {
    markdown_core_node *formula = new_formula_block_from_literal(extension, parser->mem, oldnode, literal, literal_len);
    if (!formula) {
        parser->oom = true;
        return NULL;
    }

    markdown_core_node_insert_before_unchecked(oldnode, formula);
    markdown_core_node_free(oldnode);
    return formula;
}

static markdown_core_node *promote_standalone_formula(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node *paragraph,
    markdown_core_node *inline_formula
) {
    node_formula *source = get_formula(inline_formula);
    node_formula *destination;
    markdown_core_node *formula_block;

    if (!source || !materialize_formula_literal(inline_formula)) {
        parser->oom = true;
        return NULL;
    }

    formula_block = new_formula_block_shell(extension, parser->mem, paragraph);
    if (!formula_block) {
        parser->oom = true;
        return NULL;
    }
    destination = get_formula(formula_block);

    destination->literal = source->literal;
    destination->escaped_close = source->escaped_close;
    source->literal.data = NULL;
    source->literal.len = 0;
    source->literal.alloc = 0;
    source->escaped_close = 0;

    markdown_core_node_insert_before_unchecked(paragraph, formula_block);
    markdown_core_node_free(paragraph);
    return formula_block;
}

// Block-local promotion; the parser drives it once per block in document
// order, so no case recurses into child blocks.
static markdown_core_node *postprocess_block(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node *block
) {
    if (block->type == MARKDOWN_CORE_NODE_FORMULA_BLOCK) {
        node_formula *formula = get_formula(block);
        if (formula && !formula->literal.data) {
            if (set_formula_literal_trimmed(block, block->content.ptr, block->content.size)) {
                markdown_core_strbuf_clear(&block->content);
            } else {
                parser->oom = true;
            }
        }
        return block;
    }

    if (block->type == MARKDOWN_CORE_NODE_CODE_BLOCK && info_is_formula(&block->as.code.info)) {
        markdown_core_node *formula = replace_with_formula_block(
            extension,
            parser,
            block,
            block->as.code.literal.data,
            block->as.code.literal.len
        );
        if (!formula) {
            return block;
        }
        return formula;
    }

    if (block->type == MARKDOWN_CORE_NODE_PARAGRAPH && block->first_child && block->first_child == block->last_child &&
        block->first_child->type == MARKDOWN_CORE_NODE_FORMULA && is_standalone_formula_node(block->first_child)) {
        markdown_core_node *promoted = promote_standalone_formula(extension, parser, block, block->first_child);
        if (!promoted) {
            return block;
        }
        return promoted;
    }

    return block;
}

static const markdown_core_delimiter_rule formula_delimiter_rules[] = {
    {
        .pairing = MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        .reduction = MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        .close_trigger = 0,
        .close_probe = NULL,
    },
    {
        .pairing = MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        .reduction = MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        .close_trigger = 0,
        .close_probe = NULL,
    },
    {
        .pairing = MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        .reduction = MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        .close_trigger = 0,
        .close_probe = NULL,
    },
    {
        .pairing = MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        .reduction = MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        .close_trigger = 0,
        .close_probe = NULL,
    },
};

static const unsigned char formula_special_chars[] = {'$', '\\'};

static const markdown_core_extension formula_extension = {
    .name = "formula",
    .match_inline = match,
    .last_block_matches = formula_block_matches,
    .try_opening_block = try_opening_formula_block,
    .postprocess_block = postprocess_block,
    .get_type_string = get_type_string,
    .accepts_lines = accepts_lines,
    .alloc_opaque = formula_opaque_alloc,
    .free_opaque = formula_opaque_free,
    .insert_inline_from_delim = insert_formula,
    .materialize_inline = materialize_inline,
    .delimiter_rules = formula_delimiter_rules,
    .delimiter_rule_count = sizeof(formula_delimiter_rules) / sizeof(formula_delimiter_rules[0]),
    .special_inline_chars = formula_special_chars,
    .special_inline_char_count = sizeof(formula_special_chars),
};

markdown_core_extension *markdown_core_formula_extension(void) {
    // Immutable descriptor; the cast keeps the pre-existing pointer plumbing
    // without permitting writes (see extension.h).
    return (markdown_core_extension *)&formula_extension;
}
