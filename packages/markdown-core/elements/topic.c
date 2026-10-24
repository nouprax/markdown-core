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

/* The ASCII segment characters, by what they count as: a letter, `_` or
 * `-` is a word character and a digit only a digit. Every other byte, and
 * every byte of a multi-byte character, is 0. */
enum { TOPIC_DIGIT = 1, TOPIC_WORD = 2 };
#define TOPIC_TEN(first, value)                                                                                        \
    [(first)] = (value), [(first) + 1] = (value), [(first) + 2] = (value), [(first) + 3] = (value),                    \
    [(first) + 4] = (value), [(first) + 5] = (value), [(first) + 6] = (value), [(first) + 7] = (value),                \
    [(first) + 8] = (value), [(first) + 9] = (value)
#define TOPIC_LETTERS(first)                                                                                           \
    TOPIC_TEN((first), TOPIC_WORD), TOPIC_TEN((first) + 10, TOPIC_WORD),                                               \
        [(first) + 20] = TOPIC_WORD, [(first) + 21] = TOPIC_WORD, [(first) + 22] = TOPIC_WORD,                         \
                   [(first) + 23] = TOPIC_WORD, [(first) + 24] = TOPIC_WORD, [(first) + 25] = TOPIC_WORD
static const uint8_t SEGMENT_BYTES[256] = {
    TOPIC_TEN('0', TOPIC_DIGIT), TOPIC_LETTERS('A'), TOPIC_LETTERS('a'), ['_'] = TOPIC_WORD, ['-'] = TOPIC_WORD,
};

/* THE WIDTH OF A SEGMENT CHARACTER at `str`, or 0: a letter, a mark, an
 * emoji, `_`, `-`, a number, or a joiner, with letter, mark, number and emoji
 * meaning the Unicode categories and property. `word` is set when the
 * character is one of the first five, which is what a name must contain at
 * least one of. */
static int segment_width(const unsigned char *str, bufsize_t len, bool *word) {
    if (len <= 0) {
        return 0;
    }
    if (str[0] < 0x80) {
        *word = *word || SEGMENT_BYTES[str[0]] == TOPIC_WORD;
        return SEGMENT_BYTES[str[0]] != 0;
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
    if (data[at - 1] < 0x80) {
        return data[at - 1] == '-' || (!SEGMENT_BYTES[data[at - 1]] && data[at - 1] != '&');
    }
    do {
        before--;
    } while (before && (data[before] & 0xc0) == 0x80);
    int32_t scalar;
    markdown_core_utf8proc_decode(data + before, at - before, &scalar);
    return !(markdown_core_utf8proc_classes(scalar) &
             (MARKDOWN_CORE_UNICODE_LETTER | MARKDOWN_CORE_UNICODE_MARK | MARKDOWN_CORE_UNICODE_DECIMAL)) &&
           !topic_joiner(scalar);
}

/* Where a topic's name goes on from its start: segment characters, and a
 * `/` between two. */
static bufsize_t name_end(const unsigned char *data, bufsize_t length) {
    bool word = false;
    bufsize_t end = 0;
    for (;;) {
        int width = segment_width(data + end, length - end, &word);
        if (width) {
            end += width;
        } else if (end && end < length && data[end] == '/' && segment_width(data + end + 1, length - end - 1, &word)) {
            end++;
        } else {
            return end;
        }
    }
}

static bool identifier_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
}

/* Where a block identifier goes on past a cursor. */
static bufsize_t identifier_end(const unsigned char *data, bufsize_t length) {
    bufsize_t end = 0;
    while (end < length && identifier_byte(data[end])) {
        end++;
    }
    return end;
}

/* The name of the topic whose `#` is at `at`, which the content's end cuts
 * short, to the run's context sink. At the end of a paragraph the `#` may
 * open the block identifier that names the paragraph, which the document
 * must close. */
static void S_report(markdown_core_inline_state *inline_state, bufsize_t at) {
    const bufsize_t len = inline_state->input.len;
    markdown_core_field fields[2] = {
        {MARKDOWN_CORE_SLOT_NAME, MARKDOWN_CORE_KIND_TOPIC, at + 1, name_end, {{0}}, 0, false}};
    size_t count = 1;
    if (markdown_core_node_get_kind(inline_state->owner) == MARKDOWN_CORE_KIND_PARAGRAPH &&
        at + 1 + identifier_end(inline_state->input.data + at + 1, len - at - 1) == len) {
        fields[count++] = (markdown_core_field){
            MARKDOWN_CORE_SLOT_ID, MARKDOWN_CORE_KIND_PARAGRAPH, at + 1, identifier_end, {{0}}, 0, true};
    }
    markdown_core_inline_open(inline_state, MARKDOWN_CORE_KIND_TOPIC, at, at + 1, fields, count);
}

/* THE END OF THE TOPIC whose `#` is at `at`, or 0 when there is none. The
 * name is runs of segment characters joined by single `/` signs: a `/`
 * belongs to it only when a segment character follows, so a name neither
 * starts nor ends with one. A name of numbers alone, or one followed at once
 * by `#` or `://`, is not a topic. */
static bufsize_t topic_end(markdown_core_inline_state *inline_state, bufsize_t at) {
    const unsigned char *data = inline_state->input.data;
    const bufsize_t len = inline_state->input.len;
    if (!topic_opener(inline_state, at)) {
        return 0;
    }
    bool word = false;
    bufsize_t end = at + 1;
    for (;;) {
        /* A run of ASCII segment characters is read a byte at a time,
         * looking for a word character only until one is found. */
        while (!word && end < len && SEGMENT_BYTES[data[end]]) {
            word = SEGMENT_BYTES[data[end++]] == TOPIC_WORD;
        }
        while (end < len && SEGMENT_BYTES[data[end]]) {
            end++;
        }
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
    /* The name may go on with the input, and what follows it decides
     * whether it is one; a name the input's end cuts short is a field. */
    if (end + 3 > len) {
        markdown_core_inline_saw_end(inline_state);
    }
    if (end == len) {
        S_report(inline_state, at);
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

/* THE TOPIC A RUN'S TEXT FOUND (the element's run record): the text scan
 * asks at a `#` whether a topic starts there, and the match at that `#`
 * reads the end the question found, `end` past the `#` at `at`. */
typedef struct {
    bufsize_t at, end;
} topic_run;

static bool is_inline_start(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state,
                            bufsize_t at) {
    const bufsize_t end = topic_end(inline_state, at);
    topic_run *run = markdown_core_run_state(inline_state, self);
    if (run) {
        *run = (topic_run){at, end};
    }
    return end != 0;
}

static markdown_core_node *match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    if (character != '#') {
        return NULL;
    }
    const bufsize_t start = inline_state->pos;
    const topic_run *run = markdown_core_run_state(inline_state, self);
    const bufsize_t end = run && run->end && run->at == start ? run->end : topic_end(inline_state, start);
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
    .run_state_size = sizeof(topic_run),
    .match_inline = match,
    .is_inline_start = is_inline_start,
    .terminates_text = "#",
    .dispatch = "#",
};
