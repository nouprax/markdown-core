#include "embed.h"
#include "cross_link.h"
#include "element.h"
#include "inlines.h"
#include "node.h"
#include "parser.h"
#include "inline_internal.h"
#include "attributes.h"

/* The elements whose syntax this element asks, as `self->peers` holds
 * them. */
enum { CROSS_LINK_EMBED, CROSS_LINK_ATTRIBUTES };
static const markdown_core_element *const CROSS_LINK_PEERS[] = {[CROSS_LINK_EMBED] = &MARKDOWN_CORE_ELEMENT_EMBED,
                                                                [CROSS_LINK_ATTRIBUTES] =
                                                                    &MARKDOWN_CORE_ELEMENT_ATTRIBUTES,
                                                                NULL};

/* No speculative allocation and no cursor movement until recognition succeeds.
 * A candidate stops at the first bracket or line ending. Thus candidates cannot
 * nest or share a suffix: at most the ![[ and [[ attempts inspect the same body.
 * Recognition inspects each byte at most twice, including failed and unclosed
 * forms. A successful embed then inspects only its bounded numeric suffix. */
static markdown_core_node *match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    markdown_core_chunk *input = markdown_core_inline_state_get_chunk(inline_state);
    const unsigned char *s = input->data;
    bufsize_t start = markdown_core_inline_state_get_offset(inline_state);
    bool embed = character == '!';
    bufsize_t opener_length = embed ? 3 : 2;
    bufsize_t body;
    bufsize_t i, hash = -1, separator = -1, label = -1, target_end = -1;
    bufsize_t dimension_separator = -1, dimension_separator_length = 0;
    bool part_empty = false, block_id = false;
    markdown_core_node *node;
    markdown_core_cross_reference *cross;
    markdown_core_cross_link_work *counts = self->state;
    counts->scan++;
    if (input->len - start < opener_length) {
        markdown_core_inline_state_saw_end(inline_state);
        return NULL;
    }
    body = start + opener_length;
    if (s[body - 2] != '[' || s[body - 1] != '[') {
        return NULL;
    }
    for (i = body; i < input->len; i++) {
        unsigned char c = s[i];
        counts->scan++;
        if (c == '[' || c == '\n' || c == '\r') {
            return NULL;
        }
        if (c == ']') {
            if (i + 1 >= input->len) {
                markdown_core_inline_state_saw_end(inline_state);
            }
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
                counts->scan += (size_t)dimension_separator_length - 1;
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
    /* A body the end cuts short may close once the input goes on. */
    if (i == input->len) {
        markdown_core_inline_state_saw_end(inline_state);
    }
    if (i == input->len || target_end == body || part_empty) {
        return NULL;
    }
    /* A lone ^ is an ordinary nonempty heading part, not a block identifier. */
    block_id = block_id && target_end > hash + 2;
    node = markdown_core_parser_make_node_with_ext(
        parser, embed ? MARKDOWN_CORE_NODE_CROSS_EMBED : MARKDOWN_CORE_NODE_CROSS_LINK, self->element);
    if (!node) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
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
        if (embed && self->peers[CROSS_LINK_EMBED] &&
            markdown_core_parse_dimensions(cross->label.value, suffix, dimension_separator_length,
                                           &node->as.cross_embed->dimensions.value, &counts->dimensions)) {
            node->as.cross_embed->dimensions.has_value = true;
            cross->label.value.len = suffix;
        }
    }
    if (!markdown_core_chunk_to_cstr(&cross->path) ||
        (cross->anchor.has_value && !markdown_core_chunk_to_cstr(&cross->anchor.value)) ||
        (cross->label.has_value && !markdown_core_chunk_to_cstr(&cross->label.value))) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        markdown_core_parser_release_node(parser, node);
        return NULL;
    }
    markdown_core_inline_state_place(inline_state, node, (int)start, (int)i + 1);
    markdown_core_inline_state_set_offset(inline_state, i + 2);
    /* What follows a cross link or an embed may update it (no-flush.md
     * 10.13). */
    markdown_core_inline_made(inline_state, node);
    return node;
}

static const markdown_core_inline_ops CROSS_LINK_INLINES = {
    .match = match,
    .dispatch = "[!",
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_CROSS_LINK = {
    .peers = CROSS_LINK_PEERS,
    .name = "cross_link",
    .state_size = sizeof(markdown_core_cross_link_work),
    .inlines = &CROSS_LINK_INLINES,
};

/* THE PARTS OF A CROSS LINK OR A CROSS EMBED (parts.h), as `match` reads
 * its body: the path up to the first `#`, the anchor after it and after
 * the `^` of a block identifier, up to the separator, and the label after
 * the separator, up to an embed's last separator before its dimensions,
 * and the attribute block that may follow the `]]`. */

/* A cross reference's parts (markdown_core_record_ops), which view its text
 * until it settles: both cross kinds hold one, a CrossEmbed's beside its
 * dimensions. */
static void free_reference_parts(markdown_core_cross_reference *cross) {
    markdown_core_chunk_free(&cross->path);
    markdown_core_optional_chunk_free(&cross->anchor);
    markdown_core_optional_chunk_free(&cross->label);
}

static bool own_reference_parts(markdown_core_cross_reference *cross) {
    return markdown_core_chunk_own_viewed(&cross->path) &&
           (!cross->anchor.has_value || markdown_core_chunk_own_viewed(&cross->anchor.value)) &&
           (!cross->label.has_value || markdown_core_chunk_own_viewed(&cross->label.value));
}

static void free_cross_link(markdown_core_node_pool *pool, markdown_core_node *node) {
    (void)pool;
    free_reference_parts(node->as.cross_link);
}

static bool viewed_cross_link(markdown_core_node *node) { return own_reference_parts(node->as.cross_link); }

const markdown_core_record_ops MARKDOWN_CORE_RECORD_CROSS_LINK = {
    .size = sizeof(markdown_core_cross_reference),
    .free = free_cross_link,
    .viewed = viewed_cross_link,
};

static void free_cross_embed(markdown_core_node_pool *pool, markdown_core_node *node) {
    (void)pool;
    free_reference_parts(&node->as.cross_embed->reference);
}

static bool viewed_cross_embed(markdown_core_node *node) {
    return own_reference_parts(&node->as.cross_embed->reference);
}

const markdown_core_record_ops MARKDOWN_CORE_RECORD_CROSS_EMBED = {
    .size = sizeof(markdown_core_cross_embed),
    .free = free_cross_embed,
    .viewed = viewed_cross_embed,
};
