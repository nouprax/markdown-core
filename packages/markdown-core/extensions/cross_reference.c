#include "cross_reference.h"

#include <chunk.h>
#include <node.h>
#include <parser.h>

#include "extension.h"
#include "inline_util.h"

/*
 * Internal delimiter bytes let the shared delimiter stack pair every opener
 * and closer in a single pass. They never match source bytes.
 */
#define CROSS_LINK_DELIM 5
#define EMBED_DELIM 6

typedef struct {
    markdown_core_chunk reference;
} node_cross_reference;

static void opaque_alloc(markdown_core_extension *extension, markdown_core_mem *mem, markdown_core_node *node);
static void opaque_free(markdown_core_extension *extension, markdown_core_mem *mem, markdown_core_node *node);
static markdown_core_node *match(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_node *parent,
    unsigned char character,
    markdown_core_inline_parser *inline_parser
);
static delimiter *insert(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_inline_parser *inline_parser,
    delimiter *opener,
    delimiter *closer
);
static const char *get_type_string(markdown_core_extension *extension, markdown_core_node *node);

static const unsigned char cross_link_special_chars[] = {'[', ']', CROSS_LINK_DELIM};
static const unsigned char embed_special_chars[] = {'!', ']', EMBED_DELIM};
static const unsigned char cross_link_flanking_skip_chars[] = {CROSS_LINK_DELIM};
static const unsigned char embed_flanking_skip_chars[] = {EMBED_DELIM};

static const markdown_core_extension cross_link_extension = {
    .name = "cross_link",
    .match_inline = match,
    .insert_inline_from_delim = insert,
    .get_type_string = get_type_string,
    .alloc_opaque = opaque_alloc,
    .free_opaque = opaque_free,
    .special_inline_chars = cross_link_special_chars,
    .special_inline_char_count = sizeof(cross_link_special_chars),
    .flanking_skip_chars = cross_link_flanking_skip_chars,
    .flanking_skip_char_count = sizeof(cross_link_flanking_skip_chars),
};

static const markdown_core_extension embed_extension = {
    .name = "embed",
    .match_inline = match,
    .insert_inline_from_delim = insert,
    .get_type_string = get_type_string,
    .alloc_opaque = opaque_alloc,
    .free_opaque = opaque_free,
    .special_inline_chars = embed_special_chars,
    .special_inline_char_count = sizeof(embed_special_chars),
    .flanking_skip_chars = embed_flanking_skip_chars,
    .flanking_skip_char_count = sizeof(embed_flanking_skip_chars),
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

static unsigned char delimiter_for(const markdown_core_extension *extension) {
    return extension == &cross_link_extension ? CROSS_LINK_DELIM : EMBED_DELIM;
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

static markdown_core_node *delimiter_text(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_inline_parser *inline_parser,
    markdown_core_bufsize offset,
    markdown_core_bufsize length,
    int can_open,
    int can_close
) {
    markdown_core_node *node = markdown_core_ext_make_delimiter_text(parser, inline_parser, offset, length);
    if (!node) {
        parser->oom = true;
        return NULL;
    }
    markdown_core_inline_parser_push_delimiter(inline_parser, delimiter_for(extension), can_open, can_close, node);
    return node;
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
    unsigned char delimiter = delimiter_for(extension);

    if (extension == &cross_link_extension && character == '[' && offset + 2 <= chunk->len &&
        chunk->data[offset + 1] == '[') {
        return delimiter_text(extension, parser, inline_parser, offset, 2, 1, 0);
    }
    if (extension == &embed_extension && character == '!' && offset + 3 <= chunk->len &&
        chunk->data[offset + 1] == '[' && chunk->data[offset + 2] == '[') {
        return delimiter_text(extension, parser, inline_parser, offset, 3, 1, 0);
    }
    if (character == ']' && offset + 2 <= chunk->len && chunk->data[offset + 1] == ']') {
        if (!markdown_core_inline_parser_get_last_open_delimiter(inline_parser, delimiter)) {
            return NULL;
        }
        return delimiter_text(extension, parser, inline_parser, offset, 2, 0, 1);
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

static delimiter *insert(
    markdown_core_extension *extension,
    markdown_core_parser *parser,
    markdown_core_inline_parser *inline_parser,
    delimiter *opener,
    delimiter *closer
) {
    markdown_core_chunk *chunk = markdown_core_inline_parser_get_chunk(inline_parser);
    markdown_core_node *opener_node = opener->inl_text;
    markdown_core_node *closer_node = closer->inl_text;
    delimiter *next = closer->next;
    markdown_core_bufsize body_start = opener->position;
    markdown_core_bufsize body_end = closer->position - closer->length;
    markdown_core_node *node = NULL;
    node_cross_reference *payload;

    // Cross references are single-line, and an empty reference has no identity.
    // Invalid pairs remain their original literal AST.
    if (body_start >= body_end || opener_node->start_line != closer_node->end_line) {
        goto done;
    }

    node = markdown_core_node_new_with_mem_and_ext(node_type_for(extension), parser->mem, extension);
    payload = get_cross_reference(node);
    if (!node || !payload) {
        parser->oom = true;
        if (node) {
            markdown_core_node_free(node);
        }
        goto done;
    }

    payload->reference = markdown_core_chunk_dup(chunk, body_start, body_end - body_start);
    if (!markdown_core_chunk_to_cstr(parser->mem, &payload->reference)) {
        parser->oom = true;
        markdown_core_node_free(node);
        goto done;
    }
    node->start_line = opener_node->start_line;
    node->start_column = opener_node->start_column;
    node->end_line = closer_node->end_line;
    node->end_column = closer_node->end_column;

    if (markdown_core_node_insert_before(opener_node, node)) {
        free_nodes_through(opener_node, closer_node);
    } else {
        markdown_core_node_free(node);
    }

done:
    markdown_core_ext_remove_delimiters(inline_parser, opener, closer);
    return next;
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
