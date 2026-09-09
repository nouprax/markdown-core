#include "cross_link.h"
#include "extension.h"
#include "inlines.h"
#include "node.h"
#include "parser.h"

/* No speculative allocation and no cursor movement until recognition succeeds.
 * A candidate stops at the first bracket or line ending. Thus candidates cannot
 * nest or share a suffix: at most the ![[ and [[ attempts inspect the same body.
 * Recognition inspects each byte at most twice, including failed and unclosed
 * forms. A successful embed then inspects only its bounded numeric suffix. */
static markdown_core_node *match(const markdown_core_extension *extension, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_parser *inlines) {
    markdown_core_chunk *input = markdown_core_inline_parser_get_chunk(inlines);
    const unsigned char *s = input->data;
    bufsize_t start = markdown_core_inline_parser_get_offset(inlines);
    bool embedded = character == '!';
    bufsize_t opener_length = embedded ? 3 : 2;
    bufsize_t body;
    bufsize_t i, hash = -1, separator = -1, label = -1, target_end = -1;
    bufsize_t dimension_separator = -1, dimension_separator_length = 0;
    bool part_empty = false, block_id = false;
    markdown_core_node *node;
    markdown_core_cross_reference *cross;
    parser->cross_link_scan_work++;
    if (input->len - start < opener_length) {
        return NULL;
    }
    body = start + opener_length;
    if (s[body - 2] != '[' || s[body - 1] != '[') {
        return NULL;
    }
    for (i = body; i < input->len; i++) {
        unsigned char c = s[i];
        parser->cross_link_scan_work++;
        if (c == '[' || c == '\n' || c == '\r') {
            return NULL;
        }
        if (c == ']') {
            if (i + 1 >= input->len || s[i + 1] != ']') {
                return NULL;
            }
            break;
        }
        if (label >= 0) {
            /* Labels are raw, not inline-parsed. Both authored separator forms
             * retain the cross-link grammar, including contracted table pipes. */
            if (c == '|' || (c == '\\' && i + 1 < input->len && s[i + 1] == '|')) {
                dimension_separator = i;
                dimension_separator_length = c == '|' ? 1 : 2;
                i += dimension_separator_length - 1;
                parser->cross_link_scan_work += (size_t)dimension_separator_length - 1;
            }
            continue;
        }
        if (c == '|' || (c == '\\' && i + 1 < input->len && s[i + 1] == '|')) {
            separator = i;
            label = i + (c == '|' ? 1 : 2);
            i = label - 1;
            continue;
        }
        if (c == '#') {
            if (part_empty) {
                return NULL;
            }
            if (hash < 0) {
                hash = i;
                block_id = i + 1 < input->len && s[i + 1] == '^';
            } else {
                block_id = false;
            }
            part_empty = true;
        } else {
            part_empty = false;
            if (hash >= 0 && i != hash + 1 &&
                !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) {
                block_id = false;
            }
        }
    }
    target_end = separator >= 0 ? separator : i;
    if (i == input->len || target_end == body || part_empty) {
        return NULL;
    }
    /* A lone ^ is an ordinary nonempty heading part, not a block identifier. */
    block_id = block_id && target_end > hash + 2;
    node = markdown_core_node_new_with_mem_and_ext(
        embedded ? MARKDOWN_CORE_NODE_CROSS_EMBEDDED : MARKDOWN_CORE_NODE_CROSS_LINK, parser->mem, extension);
    if (!node) {
        parser->oom = true;
        return NULL;
    }
    cross = markdown_core_node_cross_reference(node);
    cross->path = markdown_core_chunk_dup(input, body, (hash >= 0 ? hash : target_end) - body);
    if (hash >= 0) {
        bufsize_t anchor = hash + (block_id ? 2 : 1);
        cross->anchor =
            markdown_core_optional_chunk_present(markdown_core_chunk_dup(input, anchor, target_end - anchor));
    }
    if (label >= 0) {
        cross->label = markdown_core_optional_chunk_present(markdown_core_chunk_dup(input, label, i - label));
        bufsize_t suffix = dimension_separator >= 0 ? dimension_separator - label : 0;
        if (embedded &&
            markdown_core_parse_dimensions(cross->label.value, suffix, dimension_separator_length,
                                           &node->as.cross_embedded->dimensions.value, &parser->dimension_work)) {
            node->as.cross_embedded->dimensions.has_value = true;
            cross->label.value.len = suffix;
        }
    }
    if (!markdown_core_chunk_to_cstr(parser->mem, &cross->path) ||
        (cross->anchor.has_value && !markdown_core_chunk_to_cstr(parser->mem, &cross->anchor.value)) ||
        (cross->label.has_value && !markdown_core_chunk_to_cstr(parser->mem, &cross->label.value))) {
        parser->oom = true;
        markdown_core_node_free(node);
        return NULL;
    }
    markdown_core_parser_content_place(parser, parent, start, &node->start_line, &node->start_column);
    markdown_core_parser_content_end_place(parser, parent, i + 1, &node->end_line, &node->end_column);
    markdown_core_inline_parser_set_offset(inlines, i + 2);
    return node;
}

static const char *type_string(const markdown_core_extension *extension, markdown_core_node *node) {
    return node->kind == MARKDOWN_CORE_NODE_CROSS_EMBEDDED ? "cross_embedded" : "cross_link";
}

const markdown_core_extension MARKDOWN_CORE_EXTENSION_CROSS_LINK = {
    .name = "cross_link",
    .match_inline = match,
    .get_type_string_func = type_string,
    .dispatch = "[!",
};
