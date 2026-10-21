#include "topic.h"
#include "inline_internal.h"
#include "utf8.h"

/* THE JOINERS, the characters twitter-text lets a hashtag contain besides
 * letters, marks, numbers and `_`: zero width non-joiner and joiner, Cyrillic kavyka, Hebrew
 * maqaf, geresh and gershayim, fullwidth tilde, wave dash, katakana-hiragana
 * sound marks, double hyphen and middle dot, ditto mark, Tibetan tsheg and
 * non-breaking tsheg, and middle dot. */
static bool topic_joiner(int32_t scalar) {
    switch (scalar) {
    case 0x200C:
    case 0x200D:
    case 0xA67E:
    case 0x05BE:
    case 0x05F3:
    case 0x05F4:
    case 0xFF5E:
    case 0x301C:
    case 0x309B:
    case 0x309C:
    case 0x30A0:
    case 0x30FB:
    case 0x3003:
    case 0x0F0B:
    case 0x0F0C:
    case 0x00B7:
        return true;
    default:
        return false;
    }
}

/* THE WIDTH OF A SEGMENT CHARACTER at `str`, or 0: a letter, a mark, an
 * emoji, `_`, `-`, a number, or a joiner, with letter, mark, number and emoji
 * meaning the Unicode categories and property. `word` is set when the
 * character is one of the first five, which is what a name must contain at
 * least one of. */
static int segment_width(const unsigned char *str, bufsize_t len, bool *word) {
    if (len <= 0) {
        return 0;
    }
    if (str[0] == '_' || str[0] == '-') {
        *word = true;
        return 1;
    }
    int32_t scalar;
    const int width = markdown_core_utf8proc_decode(str, len, &scalar);
    const uint8_t classes = markdown_core_utf8proc_classes(scalar);
    if (classes & (MARKDOWN_CORE_UNICODE_LETTER | MARKDOWN_CORE_UNICODE_MARK | MARKDOWN_CORE_UNICODE_EMOJI)) {
        *word = true;
        return width;
    }
    return (classes & MARKDOWN_CORE_UNICODE_NUMBER) || topic_joiner(scalar) ? width : 0;
}

/* A `#` opens a topic at the start of the input or after a character that is
 * none of a letter, a mark, a decimal digit, `_`, a joiner, and `&`, which
 * begins a character reference with it, as twitter-text bounds a hashtag;
 * that character begins at the last non-continuation byte before `at`. */
static bool topic_opener(const markdown_core_inline_state *inline_state, bufsize_t at) {
    const unsigned char *data = inline_state->input.data;
    bufsize_t before = at;
    if (!at) {
        return true;
    }
    do {
        before--;
    } while (before && (data[before] & 0xc0) == 0x80);
    if (data[before] == '_' || data[before] == '&') {
        return false;
    }
    int32_t scalar;
    markdown_core_utf8proc_decode(data + before, at - before, &scalar);
    return !(markdown_core_utf8proc_classes(scalar) &
             (MARKDOWN_CORE_UNICODE_LETTER | MARKDOWN_CORE_UNICODE_MARK | MARKDOWN_CORE_UNICODE_DECIMAL)) &&
           !topic_joiner(scalar);
}

/* THE END OF THE TOPIC whose `#` is at `at`, or 0 when there is none. The
 * name is runs of segment characters joined by single `/` signs: a `/`
 * belongs to it only when a segment character follows, so a name neither
 * starts nor ends with one. A name of numbers alone, or one followed at once
 * by `#` or `://`, is not a topic. */
static bufsize_t topic_end(const markdown_core_inline_state *inline_state, bufsize_t at) {
    const unsigned char *data = inline_state->input.data;
    const bufsize_t len = inline_state->input.len;
    if (!topic_opener(inline_state, at)) {
        return 0;
    }
    bool word = false;
    bufsize_t end = at + 1;
    for (;;) {
        int width = segment_width(data + end, len - end, &word);
        if (width) {
            end += width;
        } else if (end > at + 1 && end < len && data[end] == '/' &&
                   segment_width(data + end + 1, len - end - 1, &word)) {
            end++;
        } else {
            break;
        }
    }
    if (!word) {
        return 0;
    }
    if (end < len && data[end] == '#') {
        return 0;
    }
    if (len - end >= 3 && data[end] == ':' && data[end + 1] == '/' && data[end + 2] == '/') {
        return 0;
    }
    return end;
}

static bool is_inline_start(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state,
                            bufsize_t at) {
    return topic_end(inline_state, at) != 0;
}

static markdown_core_node *match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    if (character != '#') {
        return NULL;
    }
    const bufsize_t start = inline_state->pos;
    const bufsize_t end = topic_end(inline_state, start);
    if (!end) {
        return NULL;
    }
    inline_state->pos = end;
    /* The name views the input without its `#`; the node covers both. */
    return markdown_core_inline_make_literal(inline_state, MARKDOWN_CORE_NODE_TOPIC, start, end - 1,
                                             markdown_core_chunk_dup(&inline_state->input, start + 1, end - start - 1));
}

const markdown_core_element MARKDOWN_CORE_ELEMENT_TOPIC = {
    .name = "topic",
    .match_inline = match,
    .is_inline_start = is_inline_start,
    .terminates_text = "#",
    .dispatch = "#",
};
