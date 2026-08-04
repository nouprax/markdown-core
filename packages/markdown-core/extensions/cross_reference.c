#include "cross_reference.h"

#include <assert.h>
#include <chunk.h>
#include <node.h>
#include <parser.h>

#include "extension.h"

enum { CROSS_REFERENCE_RULE = 0 };

typedef struct {
    markdown_core_chunk reference;
} node_cross_reference;

static void opaque_alloc(markdown_core_extension *extension, markdown_core_mem *mem, markdown_core_node *node);
static void opaque_free(markdown_core_extension *extension, markdown_core_mem *mem, markdown_core_node *node);
static int materialize_inline(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node *node
);
static markdown_core_bufsize probe_cross_reference_close(
    uint16_t kind,
    const unsigned char *data,
    markdown_core_bufsize len,
    markdown_core_bufsize offset
);
static markdown_core_node *match(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node *parent,
    unsigned char character,
    markdown_core_inline_parser *inline_parser
);
static markdown_core_delimiter_result insert(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_inline_parser *inline_parser,
    const markdown_core_delimiter_match *match
);
static const char *get_type_string(markdown_core_extension *extension, markdown_core_node *node);

static const unsigned char cross_link_special_chars[] = {'['};
static const unsigned char embed_special_chars[] = {'!'};

static const markdown_core_delimiter_rule cross_reference_delimiter_rules[] = {
    {
        .pairing = MARKDOWN_CORE_DELIMITER_PAIR_NEAREST,
        .reduction = MARKDOWN_CORE_DELIMITER_REDUCE_RANGE,
        .close_trigger = ']',
        .close_probe = probe_cross_reference_close,
    },
};

static const markdown_core_extension cross_link_extension = {
    .name = "cross_link",
    .match_inline = match,
    .insert_inline_from_delim = insert,
    .delimiter_rules = cross_reference_delimiter_rules,
    .delimiter_rule_count = sizeof(cross_reference_delimiter_rules) / sizeof(cross_reference_delimiter_rules[0]),
    .get_type_string = get_type_string,
    .alloc_opaque = opaque_alloc,
    .free_opaque = opaque_free,
    .materialize_inline = materialize_inline,
    .special_inline_chars = cross_link_special_chars,
    .special_inline_char_count = sizeof(cross_link_special_chars),
};

static const markdown_core_extension embed_extension = {
    .name = "embed",
    .match_inline = match,
    .insert_inline_from_delim = insert,
    .delimiter_rules = cross_reference_delimiter_rules,
    .delimiter_rule_count = sizeof(cross_reference_delimiter_rules) / sizeof(cross_reference_delimiter_rules[0]),
    .get_type_string = get_type_string,
    .alloc_opaque = opaque_alloc,
    .free_opaque = opaque_free,
    .materialize_inline = materialize_inline,
    .special_inline_chars = embed_special_chars,
    .special_inline_char_count = sizeof(embed_special_chars),
};

static int is_cross_reference_node(const markdown_core_node *node) {
    return node && (node->type == MARKDOWN_CORE_NODE_CROSS_LINK || node->type == MARKDOWN_CORE_NODE_EMBED);
}

static node_cross_reference *get_cross_reference(markdown_core_node *node) {
    if (!is_cross_reference_node(node)) {
        return NULL;
    }
    return (node_cross_reference *)node->as.opaque;
}

const markdown_core_chunk *markdown_core_cross_reference_value(markdown_core_node *node) {
    node_cross_reference *reference = get_cross_reference(node);
    return reference ? &reference->reference : NULL;
}

static markdown_core_node_type node_type_for(const markdown_core_extension *extension) {
    return extension == &cross_link_extension ? MARKDOWN_CORE_NODE_CROSS_LINK : MARKDOWN_CORE_NODE_EMBED;
}

static void opaque_alloc(markdown_core_extension *extension, markdown_core_mem *mem, markdown_core_node *node) {
    if (is_cross_reference_node(node)) {
        node->as.opaque = mem->calloc(mem, 1, sizeof(node_cross_reference));
    }
}

static void opaque_free(markdown_core_extension *extension, markdown_core_mem *mem, markdown_core_node *node) {
    node_cross_reference *reference = (node_cross_reference *)node->as.opaque;
    if (!reference) {
        return;
    }
    markdown_core_chunk_free(mem, &reference->reference);
    mem->free(mem, reference);
}

static int materialize_inline(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node *node
) {
    node_cross_reference *reference = get_cross_reference(node);
    if (!reference || reference->reference.alloc) {
        return 1;
    }
    return markdown_core_chunk_to_cstr(parser->mem, &reference->reference) != NULL;
}

static markdown_core_bufsize probe_cross_reference_close(
    uint16_t kind,
    const unsigned char *data,
    markdown_core_bufsize len,
    markdown_core_bufsize offset
) {
    return kind == CROSS_REFERENCE_RULE && offset >= 0 && offset <= len - 2 && data[offset] == ']' &&
                   data[offset + 1] == ']'
               ? 2
               : 0;
}

static markdown_core_node *match(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node *parent,
    unsigned char character,
    markdown_core_inline_parser *inline_parser
) {
    markdown_core_chunk *chunk = markdown_core_inline_parser_get_chunk(inline_parser);
    markdown_core_bufsize offset = (markdown_core_bufsize)markdown_core_inline_parser_get_offset(inline_parser);

    if (extension == &cross_link_extension && character == '[' && offset + 2 <= chunk->len &&
        chunk->data[offset + 1] == '[') {
        return markdown_core_inline_parser_consume_delimiter(inline_parser, CROSS_REFERENCE_RULE, 1, 0, offset + 2);
    }
    if (extension == &embed_extension && character == '!' && offset + 3 <= chunk->len &&
        chunk->data[offset + 1] == '[' && chunk->data[offset + 2] == '[') {
        return markdown_core_inline_parser_consume_delimiter(inline_parser, CROSS_REFERENCE_RULE, 1, 0, offset + 3);
    }
    return NULL;
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

static markdown_core_delimiter_result insert(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_inline_parser *inline_parser,
    const markdown_core_delimiter_match *match
) {
    markdown_core_chunk *chunk = markdown_core_inline_parser_get_chunk(inline_parser);
    markdown_core_node *opener_node = match->opener_node;
    markdown_core_node *closer_node = match->closer_node;
    markdown_core_bufsize body_start = match->opener_end;
    markdown_core_bufsize body_end = match->closer_start;
    markdown_core_node *node = NULL;
    node_cross_reference *payload;

    if (match->kind != CROSS_REFERENCE_RULE) {
        assert(0 && "cross-reference reducer received an unsupported delimiter rule");
        return MARKDOWN_CORE_DELIMITER_INVALID;
    }
    if (!opener_node || !closer_node || opener_node->type != MARKDOWN_CORE_NODE_TEXT ||
        closer_node->type != MARKDOWN_CORE_NODE_TEXT) {
        assert(0 && "cross-reference reducer received invalid delimiter endpoints");
        return MARKDOWN_CORE_DELIMITER_INVALID;
    }

    // Cross references are single-line, and an empty reference has no identity.
    // Invalid pairs remain their original literal AST.
    if (body_start >= body_end || opener_node->start_line != closer_node->end_line) {
        return MARKDOWN_CORE_DELIMITER_OK;
    }

    node = markdown_core_node_new_with_mem_and_ext(node_type_for(extension), parser->mem, extension);
    payload = get_cross_reference(node);
    if (!node || !payload) {
        if (node) {
            markdown_core_node_free(node);
        }
        return MARKDOWN_CORE_DELIMITER_OOM;
    }

    /*
     * Borrow during delimiter reduction. Nested matches that are subsequently
     * swallowed by an outer opaque reference then allocate nothing; the
     * survivor materialization pass copies only nodes in the final AST.
     */
    payload->reference = markdown_core_chunk_borrow(chunk, body_start, body_end - body_start);
    node->start_line = opener_node->start_line;
    node->start_column = opener_node->start_column;
    node->end_line = closer_node->end_line;
    node->end_column = closer_node->end_column;

    markdown_core_node_insert_before_unchecked(opener_node, node);
    free_nodes_through(opener_node, closer_node);
    /* The empty/multi-line OKs above consume nothing; this path consumed
     * both bracket runs and re-borrowed the body raw, so the interior's
     * parsed records must be retracted along with the report. */
    markdown_core_inline_parser_concrete_use_endpoints(inline_parser, match);
    markdown_core_inline_parser_concrete_reinterpret(inline_parser, match->opener_end, match->closer_start);
    return MARKDOWN_CORE_DELIMITER_OK;
}

static const char *get_type_string(markdown_core_extension *extension, markdown_core_node *node) {
    if (node->type == MARKDOWN_CORE_NODE_CROSS_LINK) {
        return "cross_link";
    }
    if (node->type == MARKDOWN_CORE_NODE_EMBED) {
        return "embed";
    }
    return "<unknown>";
}

markdown_core_extension *markdown_core_cross_link_extension(void) {
    return (markdown_core_extension *)&cross_link_extension;
}

markdown_core_extension *markdown_core_embed_extension(void) { return (markdown_core_extension *)&embed_extension; }
