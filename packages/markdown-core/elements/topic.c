#include "topic.h"
#include "services.h"
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

/* Where a name read from `data` stops: runs of segment characters joined by
 * single `/` signs, a `/` belonging to it only when a segment character
 * follows. `word` is set when it holds a word character. */
static bufsize_t name_end(const unsigned char *data, bufsize_t length, bool *word) {
    bufsize_t end = 0;
    for (;;) {
        /* A run of ASCII segment characters is read a byte at a time,
         * looking for a word character only until one is found. */
        while (!*word && end < length && SEGMENT_BYTES[data[end]]) {
            *word = SEGMENT_BYTES[data[end++]] == TOPIC_WORD;
        }
        while (end < length && SEGMENT_BYTES[data[end]]) {
            end++;
        }
        const int width = segment_width(data + end, length - end, word);
        if (width) {
            end += width;
        } else if (end && end < length && data[end] == '/' && segment_width(data + end + 1, length - end - 1, word)) {
            end++;
        } else {
            return end;
        }
    }
}

/* THE TOPIC A RUN'S TEXT FOUND (the element's run record): the text scan
 * asks at a `#` whether a topic starts there, and the match at that `#`
 * reads the end the question found, `end` past the `#` at `at`. */
typedef struct {
    bufsize_t at, end;
} topic_run;

/* THE TOPIC AS A CONSTRUCT (plan symbols-cleanroom.md 23.10): a `#` where
 * a topic starts reads it whole; at any other the `#` is text, which the
 * data it meets runs on over (`passes`). */
static bool S_opener(const unsigned char *data, uint32_t at) {
    if (!at) {
        return true;
    }
    if (data[at - 1] < 0x80) {
        return data[at - 1] == '-' || (!SEGMENT_BYTES[data[at - 1]] && data[at - 1] != '&');
    }
    uint32_t before = at;
    do {
        before--;
    } while (before && (data[before] & 0xc0) == 0x80);
    int32_t scalar;
    markdown_core_utf8proc_decode(data + before, (bufsize_t)(at - before), &scalar);
    return !(markdown_core_utf8proc_classes(scalar) &
             (MARKDOWN_CORE_UNICODE_LETTER | MARKDOWN_CORE_UNICODE_MARK | MARKDOWN_CORE_UNICODE_DECIMAL)) &&
           !topic_joiner(scalar);
}

/* topic_end, read by the tokenizer. */
static uint32_t S_end(markdown_core_tokenizer *tokenizer, uint32_t at) {
    const unsigned char *data = tokenizer->data;
    const uint32_t len = tokenizer->size;
    if (!S_opener(data, at)) {
        return 0;
    }
    bool word = false;
    const uint32_t end = at + 1 + (uint32_t)name_end(data + at + 1, (bufsize_t)(len - at - 1), &word);
    if (end + (end < len && data[end] == '/' ? 5 : 4) > len) {
        markdown_core_met_end(tokenizer);
    }
    if (!word || (end < len && data[end] == '#') ||
        (len - end >= 3 && data[end] == ':' && data[end + 1] == '/' && data[end + 2] == '/')) {
        const markdown_core_field name = {MARKDOWN_CORE_SLOT_NAME, MARKDOWN_CORE_KIND_TOPIC, (bufsize_t)at + 1,
                                          (bufsize_t)end};
        markdown_core_text_give_up(tokenizer, MARKDOWN_CORE_KIND_TOPIC, (bufsize_t)at, (bufsize_t)at + 1, &name, 1);
        return 0;
    }
    return end;
}

enum { TOPIC_NAME, TOPIC_KINDS };

static markdown_core_state S_topic(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    const uint32_t end = S_end(tokenizer, tokenizer->position);
    if (!end) {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_enter(tokenizer, (uint16_t)(self->token_base + TOPIC_NAME));
    markdown_core_consume_to(tokenizer, end);
    markdown_core_exit(tokenizer, (uint16_t)(self->token_base + TOPIC_NAME));
    return markdown_core_ok(tokenizer);
}

static uint32_t S_passes(markdown_core_tokenizer *tokenizer, const void *context, uint32_t at) {
    (void)context;
    return S_end(tokenizer, at) ? at : at + 1;
}

static const markdown_core_text_construct TOPIC_CONSTRUCTS[] = {{"#", {.start = S_topic, .passes = S_passes}}};

/* The name views the input without its `#`; the node covers both. */
static void S_made(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    const bufsize_t start = (bufsize_t)token->start, end = (bufsize_t)token->end;
    markdown_core_node *topic =
        markdown_core_inline_make_literal(inline_state, MARKDOWN_CORE_NODE_TOPIC, start, end - 1,
                                          markdown_core_inline_take(inline_state, start + 1, end - start - 1));
    if (topic) {
        const markdown_core_field_to fields = markdown_core_inline_fields(inline_state, topic);
        markdown_core_field_add(&fields, MARKDOWN_CORE_SLOT_NAME, start + 1, end);
    }
    markdown_core_compile_append(compiler, topic);
}

static const markdown_core_compile_ops TOPIC_TOKENS[TOPIC_KINDS] = {[TOPIC_NAME] = {.exit = S_made}};

static const markdown_core_inline_ops TOPIC_INLINES = {
    .constructs = TOPIC_CONSTRUCTS,
    .construct_count = 1,
    .tokens = TOPIC_TOKENS,
    .token_count = TOPIC_KINDS,
};

/* A topic reads as its `#` and its name in a name. */
static bool S_words(const markdown_core_node *node, const markdown_core_words *words) {
    return words->text(words->context, (const unsigned char *)"#", 1) && markdown_core_words_literal(node, words);
}

const markdown_core_facet MARKDOWN_CORE_TOPIC_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_TOPIC, .words = S_words},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_TOPIC = {
    .facets = MARKDOWN_CORE_TOPIC_FACETS,
    .name = "topic",
    .run_state_size = sizeof(topic_run),
    .inlines = &TOPIC_INLINES,
};
