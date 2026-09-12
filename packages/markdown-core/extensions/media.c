#include "link.h"
#include "media.h"
#include "inline_internal.h"
#include "block_internal.h"
static bool dimension_component(const unsigned char *s, bufsize_t *pos, bufsize_t end, int32_t *value, size_t *work) {
    if (*pos == end || s[*pos] < '1' || s[*pos] > '9') {
        return false;
    }
    int32_t number = 0;
    while (*pos < end && s[*pos] >= '0' && s[*pos] <= '9') {
        (*work)++;
        int digit = s[(*pos)++] - '0';
        if (number > (INT32_MAX - digit) / 10) {
            return false;
        }
        number = number * 10 + digit;
    }
    *value = number;
    return true;
}

bool markdown_core_parse_dimensions(markdown_core_chunk label, bufsize_t suffix, bufsize_t separator_length,
                                    markdown_core_dimensions *value, size_t *work) {
    bufsize_t pos = suffix + separator_length;
    markdown_core_dimensions parsed = {0};
    (*work)++;
    if ((suffix > 0 && markdown_core_isspace(label.data[suffix - 1])) ||
        !dimension_component(label.data, &pos, label.len, &parsed.width, work)) {
        return false;
    }
    if (pos < label.len && label.data[pos] == 'x') {
        int32_t height;
        pos++;
        if (!dimension_component(label.data, &pos, label.len, &height, work)) {
            return false;
        }
        parsed.height = (markdown_core_optional_i64){true, height};
    }
    if (pos != label.len) {
        return false;
    }
    *value = parsed;
    return true;
}

void markdown_core_inline_apply_image_dimensions(markdown_core_inline_state *inline_state, const bracket *opener,
                                                 markdown_core_node *image, bufsize_t end) {
    /* Earlier inline allocation failure may have omitted the final text run.
     * The transaction is already failed; do not consume its incomplete tree. */
    if (inline_state->oom || inline_state->owner_parser->oom) {
        return;
    }
    bufsize_t suffix = opener->image_pipe >= 0 ? opener->image_pipe : opener->position;
    markdown_core_dimensions dimensions;
    markdown_core_chunk label = markdown_core_chunk_dup(&inline_state->input, opener->position, end - opener->position);
    if (!markdown_core_parse_dimensions(label, suffix - opener->position, opener->image_pipe >= 0 ? 1 : 0, &dimensions,
                                        &inline_state->owner_parser->dimension_work)) {
        return;
    }

    /* A successful suffix contains only ordinary ASCII text and belongs to
     * the final text run at this bracket depth. Remove it before delimiter
     * reduction; the prefix keeps its nodes and its original source map. */
    markdown_core_node *tail = image->last_child;
    assert(tail && tail->kind == MARKDOWN_CORE_NODE_TEXT && tail->as.literal->len >= end - suffix);
    bufsize_t start = end - tail->as.literal->len;
    tail->as.literal->len -= end - suffix;
    if (tail->as.literal->len == 0) {
        markdown_core_node_free(tail);
    } else {
        markdown_core_inline_state_place(inline_state, tail, start, suffix - 1);
    }
    image->as.link->dimensions.value = dimensions;
    image->as.link->dimensions.has_value = true;
}

void markdown_core_media_record_text(markdown_core_parser *parser, markdown_core_inline_state *inline_state,
                                     bufsize_t endpos) {
    if (inline_state->last_bracket && inline_state->last_bracket->kind == BRACKET_IMAGE) {
        for (bufsize_t i = inline_state->pos; i < endpos; i++) {
            parser->dimension_work++;
            if (inline_state->input.data[i] == '|') {
                inline_state->last_bracket->image_pipe = i;
            }
        }
    }
}

static markdown_core_node *match(const markdown_core_extension *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    if (character != '!') {
        return NULL;
    }
    inline_state->pos++;
    if (markdown_core_inline_peek_char(inline_state) == '[' &&
        markdown_core_inline_peek_char_n(inline_state, 1) != '^') {
        inline_state->pos++;
        markdown_core_node *text =
            make_str(inline_state, inline_state->pos - 2, inline_state->pos - 1, markdown_core_chunk_literal("!["));
        if (text) {
            markdown_core_inline_push_bracket(inline_state, BRACKET_IMAGE, text);
        }
        return text;
    }
    return make_str(inline_state, inline_state->pos - 1, inline_state->pos - 1, markdown_core_chunk_literal("!"));
}

const markdown_core_extension MARKDOWN_CORE_EXTENSION_MEDIA = {
    .inline_precedence = MARKDOWN_CORE_INLINE_FALLBACK,

    .name = "media",
    .match_inline = match,
    .terminates_text = "!",
    .dispatch = "!",
};
