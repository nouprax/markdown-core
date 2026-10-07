#include "link.h"
#include "embedded.h"
#include "inline_internal.h"
#include "block_internal.h"

/* The elements whose state this element reads, as `self->peers` holds them. */
enum { EMBEDDED_LINK };
static const markdown_core_element *const EMBEDDED_PEERS[] = {[EMBEDDED_LINK] = &MARKDOWN_CORE_ELEMENT_LINK, NULL};
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
    if ((suffix > 0 && markdown_core_is_whitespace(label.data[suffix - 1])) ||
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

void markdown_core_inline_apply_image_dimensions(const markdown_core_element_instance *self,
                                                 markdown_core_inline_state *inline_state, const bracket *opener,
                                                 markdown_core_member *image, bufsize_t end) {
    /* Earlier inline allocation failure may have omitted the final text run.
     * The transaction is already failed; do not consume its incomplete tree. */
    if (inline_state->error || inline_state->owner_parser->error) {
        return;
    }
    bufsize_t suffix = opener->image_pipe >= 0 ? opener->image_pipe : opener->position;
    markdown_core_dimensions dimensions;
    markdown_core_chunk label = markdown_core_chunk_dup(&inline_state->input, opener->position, end - opener->position);
    if (!markdown_core_parse_dimensions(label, suffix - opener->position, opener->image_pipe >= 0 ? 1 : 0, &dimensions,
                                        &((markdown_core_embedded_work *)self->state)->dimensions)) {
        return;
    }

    /* A successful suffix contains only ordinary ASCII text and belongs to
     * the final text run at this bracket depth. Remove it before delimiter
     * reduction; the prefix keeps its nodes and its original source map. */
    markdown_core_member *last = image->last;
    markdown_core_node *tail = last->node;
    assert(tail->kind == MARKDOWN_CORE_NODE_TEXT && tail->as.literal->len >= end - suffix);
    bufsize_t start = end - tail->as.literal->len;
    tail->as.literal->len -= end - suffix;
    if (tail->as.literal->len == 0) {
        markdown_core_parser_release_member(inline_state->owner_parser, last);
    } else {
        markdown_core_inline_state_place(inline_state, tail, start, suffix - 1);
    }
    image->node->as.link->dimensions.value = dimensions;
    image->node->as.link->dimensions.has_value = true;
}

void markdown_core_embedded_record_text(const markdown_core_element_instance *self,
                                        markdown_core_inline_state *inline_state, bufsize_t endpos) {
    const markdown_core_element_instance *link = self->peers[EMBEDDED_LINK];
    bracket *image = link ? markdown_core_brackets(link, inline_state)->last : NULL;
    if (image && image->kind == BRACKET_IMAGE) {
        markdown_core_embedded_work *counts = self->state;
        for (bufsize_t i = inline_state->pos; i < endpos; i++) {
            counts->dimensions++;
            if (inline_state->input.data[i] == '|') {
                image->image_pipe = i;
            }
        }
    }
}

static markdown_core_member *match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                   markdown_core_member *parent, unsigned char character,
                                   markdown_core_inline_state *inline_state) {
    if (character != '!') {
        return NULL;
    }
    inline_state->pos++;
    /* An image opens a bracket, which only a dialect with links reads. */
    if (self->peers[EMBEDDED_LINK] && markdown_core_inline_peek_char(inline_state) == '[' &&
        markdown_core_inline_peek_char_n(inline_state, 1) != '^') {
        inline_state->pos++;
        markdown_core_member *text = markdown_core_inline_state_append(
            inline_state, make_str(inline_state, inline_state->pos - 2, inline_state->pos - 1,
                                   markdown_core_chunk_dup(&inline_state->input, inline_state->pos - 2, 2)));
        if (text) {
            markdown_core_inline_push_bracket(self->peers[EMBEDDED_LINK], inline_state, BRACKET_IMAGE, text);
        }
        return text;
    }
    return markdown_core_inline_state_append(
        inline_state, make_str(inline_state, inline_state->pos - 1, inline_state->pos - 1,
                               markdown_core_chunk_dup(&inline_state->input, inline_state->pos - 1, 1)));
}

const markdown_core_element MARKDOWN_CORE_ELEMENT_EMBEDDED = {
    .peers = EMBEDDED_PEERS,
    .inline_precedence = MARKDOWN_CORE_INLINE_FALLBACK,
    .state_size = sizeof(markdown_core_embedded_work),

    .name = "embedded",
    .match_inline = match,
    .terminates_text = "!",
    .dispatch = "!",
};
