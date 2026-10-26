#include "embedded.h"
#include "cross_link.h"
#include "element.h"
#include "inlines.h"
#include "node.h"
#include "parser.h"
#include "inline_internal.h"
#include "parts.h"

/* How a cross link's target goes on past a cursor: its path up to the `#`
 * that starts its anchor, and its anchor, which may hold more parts, up to
 * the separator or the closer. Neither crosses a bracket or a line. */
static bufsize_t target_end(const unsigned char *data, bufsize_t length, bool anchor) {
    bufsize_t i = 0;
    while (i < length && data[i] != '[' && data[i] != ']' && data[i] != '|' && data[i] != '\n' && data[i] != '\r' &&
           !(data[i] == '\\' && i + 1 < length && data[i + 1] == '|') && (anchor || data[i] != '#')) {
        i++;
    }
    return i;
}
static bufsize_t path_end(const unsigned char *data, bufsize_t length) { return target_end(data, length, false); }
static bufsize_t anchor_end(const unsigned char *data, bufsize_t length) { return target_end(data, length, true); }

/* The field a body the content's end cuts short reads, to the run's context
 * sink: the anchor after the first `#`, with the path before it, or else the
 * path; a body past its separator reads the label, which is no slot. */
static void S_report(markdown_core_inline_state *inline_state, bool embedded, bufsize_t start, bufsize_t body,
                     bufsize_t hash, bufsize_t label) {
    const markdown_core_node_kind kind = embedded ? MARKDOWN_CORE_KIND_CROSS_EMBEDDED : MARKDOWN_CORE_KIND_CROSS_LINK;
    if (label >= 0) {
        markdown_core_inline_open(inline_state, kind, start, body, NULL, 0);
    } else if (hash >= 0) {
        /* The anchor keeps a leading `^`: whether it marks a block identifier
         * or starts a heading depends on the anchor's whole text, cursor on. */
        const markdown_core_field anchor = {
            MARKDOWN_CORE_SLOT_ANCHOR, kind, hash + 1, anchor_end, {{body, hash}}, 1, false, 0};
        markdown_core_inline_open(inline_state, kind, start, body, &anchor, 1);
    } else {
        const markdown_core_field path = {MARKDOWN_CORE_SLOT_DEST, kind, body, path_end, {{0}}, 0, false, 0};
        markdown_core_inline_open(inline_state, kind, start, body, &path, 1);
    }
}

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
    bool embedded = character == '!';
    bufsize_t opener_length = embedded ? 3 : 2;
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
                if (inline_state->context) {
                    markdown_core_inline_open(
                        inline_state, embedded ? MARKDOWN_CORE_KIND_CROSS_EMBEDDED : MARKDOWN_CORE_KIND_CROSS_LINK,
                        start, body, NULL, 0);
                }
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
        if (inline_state->context) {
            S_report(inline_state, embedded, start, body, hash, label);
        }
    }
    if (i == input->len || target_end == body || part_empty) {
        return NULL;
    }
    /* A lone ^ is an ordinary nonempty heading part, not a block identifier. */
    block_id = block_id && target_end > hash + 2;
    node = markdown_core_parser_make_node_with_ext(
        parser, embedded ? MARKDOWN_CORE_NODE_CROSS_EMBEDDED : MARKDOWN_CORE_NODE_CROSS_LINK, self->element);
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
        if (embedded &&
            markdown_core_parse_dimensions(cross->label.value, suffix, dimension_separator_length,
                                           &node->as.cross_embedded->dimensions.value, &counts->dimensions)) {
            node->as.cross_embedded->dimensions.has_value = true;
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
    int line;
    bufsize_t first, last;
    markdown_core_parser_content_place(parser, &parent->content_map, start, &line, &first);
    markdown_core_parser_content_end_place(parser, &parent->content_map, i + 1, &line, &last);
    node->place = (markdown_core_place){(uint32_t)first, (uint32_t)last};
    markdown_core_inline_state_set_offset(inline_state, i + 2);
    return node;
}

const markdown_core_element MARKDOWN_CORE_ELEMENT_CROSS_LINK = {
    .name = "cross_link",
    .state_size = sizeof(markdown_core_cross_link_work),
    .match_inline = match,
    .dispatch = "[!",
};

/* THE PARTS OF A CROSS LINK OR A CROSS EMBED (parts.h), as `match` reads
 * its body: the path up to the first `#`, the anchor after it and after
 * the `^` of a block identifier, up to the separator, and the label after
 * the separator, up to an embed's last separator before its dimensions. */
void markdown_core_cross_link_parts(markdown_core_parts *parts) {
    const unsigned char *s = parts->text;
    const bufsize_t n = parts->length;
    const bool embedded = markdown_core_node_get_kind(parts->node) == MARKDOWN_CORE_KIND_CROSS_EMBEDDED;
    const markdown_core_cross_reference *cross = markdown_core_node_cross_reference((markdown_core_node *)parts->node);
    const bufsize_t body = embedded ? 3 : 2, close = n >= 2 ? n - 2 : n;
    bufsize_t target = body, hash = -1;
    while (target < close && s[target] != '|' && !(s[target] == '\\' && target + 1 < close && s[target + 1] == '|')) {
        hash = hash < 0 && s[target] == '#' ? target : hash;
        target++;
    }
    markdown_core_parts_add(parts, MARKDOWN_CORE_SLOT_DEST, body, hash >= 0 ? hash : target);
    if (hash >= 0) {
        const bool block = s[hash + 1] == '^' && cross->anchor.value.len == target - hash - 2;
        markdown_core_parts_add(parts, MARKDOWN_CORE_SLOT_ANCHOR, hash + 1 + block, target);
    }
    if (target >= close) {
        return;
    }
    const bufsize_t label = target + (s[target] == '|' ? 1 : 2);
    bufsize_t end = close;
    if (embedded && parts->node->as.cross_embedded->dimensions.has_value) {
        /* The dimensions follow the label's last separator, or are all of
         * it. */
        bufsize_t separator = -1;
        for (bufsize_t at = label; at < close; at++) {
            if (s[at] == '|' || (s[at] == '\\' && at + 1 < close && s[at + 1] == '|')) {
                separator = at;
                at += s[at] == '\\';
            }
        }
        end = separator >= 0 ? separator : label;
        const bufsize_t dimensions = separator < 0 ? label : separator + (s[separator] == '|' ? 1 : 2);
        markdown_core_parts_add(parts, MARKDOWN_CORE_SLOT_NONE, dimensions, close);
    }
    markdown_core_parts_add(parts, MARKDOWN_CORE_SLOT_NONE, label, end);
}
