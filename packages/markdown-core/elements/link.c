#include <string.h>

#include "alloc.h"
#include "services.h"
#define advance(inline_state) markdown_core_inline_seek((inline_state), markdown_core_inline_offset(inline_state) + 1)
#include "link.h"
#include "attention.h"
#include "label.h"
#include "inline_internal.h"
#include "block_internal.h"

/* The services this element asks, as `self->needs` holds them. */
enum { LINK_DIMENSIONS, LINK_ATTRIBUTES, LINK_ESCAPES, LINK_REFERENCES };
static const char *const LINK_NEEDS[] = {[LINK_DIMENSIONS] = MARKDOWN_CORE_SERVICE_DIMENSIONS,
                                         [LINK_ATTRIBUTES] = MARKDOWN_CORE_SERVICE_ATTRIBUTES,
                                         [LINK_ESCAPES] = MARKDOWN_CORE_SERVICE_ESCAPES,
                                         [LINK_REFERENCES] = MARKDOWN_CORE_SERVICE_REFERENCES,
                                         NULL};

/* A link's label, opened by a `[` (label.h). */
static const markdown_core_label_shape MARKDOWN_CORE_LINK_BRACKET = {
    .width = 1, .mark = 0, .closer = ']', .kind = MARKDOWN_CORE_KIND_LINK};

const markdown_core_space MARKDOWN_CORE_REFERENCE_SPACE = {.name = "reference", .folds = true};
/* The spaces this element files labels in, as `self->spaces` numbers them. */
enum { LINK_REFERENCE };
static const markdown_core_space *const LINK_SPACES[] = {[LINK_REFERENCE] = &MARKDOWN_CORE_REFERENCE_SPACE, NULL};

/* WHAT THE DESTINATION AND TITLE STATES READ (a definition's and a
 * resource's alike, micromark's destination and title factories), as
 * offsets of the reading's data: the destination's bytes, inside its angle
 * brackets when it has them, and the title with its quotes, `titled` when
 * one closed; the title's opener seen, 0 when none was. What follows a
 * label's closer (S_after) reads besides: the label's token, where the
 * resource's destination starts (`from`), and a full reference's label
 * inside its brackets; whether a resource was read, through `past`, a
 * label was, through `past`, or a destination was begun and given up. */
typedef struct S_read {
    uint32_t dest, dest_end, title, title_end;
    uint32_t label, from, label_start, label_end, past;
    int opener;
    bool titled, resource, labeled, given;
} S_read;

/* WHAT A PARSE HOLDS OF THE LINK ELEMENT: the recognizer of the attribute
 * blocks of the text a front cut reads (element.h, markdown_core_cut), kept
 * across its items, so that the many blocks of one text are read linearly;
 * and what the element's states read, as the element's readings run one
 * at a time, each to its end. */
typedef struct {
    S_read read;
} S_state;

static inline S_read *S_read_of(const markdown_core_tokenizer *tokenizer) {
    return &((S_state *)((const markdown_core_element_instance *)markdown_core_self(tokenizer))->state)->read;
}

/* Reads the link reference definition at the front of `input`, its bytes
 * `before` past the front of `b`'s content, into a Reference node settled
 * into `parent`, the block that holds `b`, which starts on input line
 * `*line`, an earlier one or a later one, and leaves `*line` the line it ends
 * on. Its length, 0 when there is none. */

/* A destination is its source less the angle brackets, with escapes and
 * references decoded. Nothing is trimmed: an unbracketed destination cannot
 * hold a space, tab or line ending, and CommonMark keeps the spaces a
 * bracketed one encloses (`<  u >` is `  u `). */
markdown_core_chunk markdown_core_clean_url(const markdown_core_element_instance *link, markdown_core_chunk *url,
                                            int *lost) {
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT();

    if (url->len == 0) {
        return markdown_core_chunk_literal("");
    }

    markdown_core_decoder_write(link->needs[LINK_REFERENCES], &buf, url->data, url->len);

    markdown_core_decoder_strip(link->needs[LINK_ESCAPES], &buf);
    if (buf.oom && lost) {
        *lost = 1;
    }
    return markdown_core_chunk_buf_detach(&buf);
}

markdown_core_optional_chunk markdown_core_clean_title(const markdown_core_element_instance *link,
                                                       markdown_core_chunk *title, int *lost) {
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT();
    unsigned char first, last;

    if (title->len == 0) {
        return markdown_core_optional_chunk_absent();
    }

    first = title->data[0];
    last = title->data[title->len - 1];

    // remove surrounding quotes if any:
    if ((first == '\'' && last == '\'') || (first == '(' && last == ')') || (first == '"' && last == '"')) {
        markdown_core_decoder_write(link->needs[LINK_REFERENCES], &buf, title->data + 1, title->len - 2);
    } else {
        markdown_core_decoder_write(link->needs[LINK_REFERENCES], &buf, title->data, title->len);
    }

    markdown_core_decoder_strip(link->needs[LINK_ESCAPES], &buf);
    if (buf.oom && lost) {
        *lost = 1;
    }
    return markdown_core_optional_chunk_present(markdown_core_chunk_buf_detach(&buf));
}

/* The only bytes that decide where a link label ends: the brackets, and the
 * backslash that may escape one. */
enum { LABEL_STOP = 1 };
static const uint8_t LABEL_BYTES[256] = {['['] = LABEL_STOP, [']'] = LABEL_STOP, ['\\'] = LABEL_STOP};

/* Where a link label written at `data` ends: the first unescaped `[` or `]`,
 * or `length` when there is none. Past MAX_LINK_LABEL_LENGTH the answer is
 * only that it is too long, so the search stops there and the length it
 * returns exceeds the maximum. A backslash escapes the punctuation character
 * after it, which is stepped over with it; every other byte, of any script,
 * is skipped by the class scan. */
bufsize_t markdown_core_inline_reference_label_length(const unsigned char *data, bufsize_t length) {
    const bufsize_t end = length <= MAX_LINK_LABEL_LENGTH ? length : MAX_LINK_LABEL_LENGTH + 1;
    bufsize_t at = 0;
    while (at < end) {
        at = markdown_core_scan_to_class(LABEL_BYTES, LABEL_STOP, data, at, end);
        if (at == end || data[at] != '\\') {
            break;
        }
        at += at + 1 < length && markdown_core_ispunct((char)data[at + 1]) ? 2 : 1;
    }
    return at;
}

/* An unbracketed destination includes no ASCII control character (U+0000 to
 * U+001F, U+007F) and no space; tab and the line endings are controls. */
static bool destination_excludes(unsigned char c) { return c <= 0x20 || c == 0x7F; }

/* What a definition's attribute tail is read with: the attribute grammar
 * (services.h, markdown_core_attribute_ops) and its provider. */
typedef struct {
    const markdown_core_attribute_ops *ops;
    const markdown_core_element_instance *self;
} S_tails;

/* A link reference definition at the front of `input`: its length, 0 when
 * there is none, and its parts, the attributes `value` holds when there is
 * one. `wide` when its reading may have read past its end and the first
 * byte after it: a title or an attribute list looked for, which reads on
 * to where it closes, or to the text's end. */
typedef struct {
    markdown_core_chunk label, url, title;
    markdown_core_attributes value;
    bool wide;
    /* Its attribute list was read, the last the recognizer read. */
    bool listed;
    /* How far its reading went, whether it reads one or not: through
     * `[label]:`, its label's `]` at `label_end`, then its destination. */
    uint8_t reached;
    bufsize_t label_end;
    /* The `{` of an attribute list looked for after its destination or
     * title, -1 when none was, and the title's opener seen, 0 when none
     * was; text offsets. */
    bufsize_t brace;
    int opener;
} reference_definition;

static void S_report_unread(markdown_core_parser *parser, markdown_core_node *b, const markdown_core_chunk *input,
                            bufsize_t before, const reference_definition *definition);
static void S_report_tail(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_node *b, markdown_core_chunk *input, bufsize_t before,
                          const reference_definition *definition);

static bufsize_t field_end(const unsigned char *data, bufsize_t length, bool angled, bool fragment);

/* The fields of the definition `definition` read in `text`, the content of
 * the block `b`, on `reference`: its label, destination and fragment, its
 * title inside its quotes and its attribute list's members. */
static void S_reference_fields(markdown_core_parser *parser, markdown_core_node *reference, markdown_core_node *b,
                               const unsigned char *text, const reference_definition *definition,
                               const S_tails *tails) {
    const markdown_core_field_to to = {parser, reference, &b->content_map, NULL, 0, NULL};
    markdown_core_field_spot spots[4];
    size_t count = 0;
    const bufsize_t label = (bufsize_t)(definition->label.data - text);
    spots[count++] = (markdown_core_field_spot){label, label + definition->label.len, MARKDOWN_CORE_SLOT_LABEL, false,
                                                MARKDOWN_CORE_FIELD};
    const bufsize_t url = (bufsize_t)(definition->url.data - text), url_end = url + definition->url.len;
    const bool angled = url > 0 && text[url - 1] == '<';
    /* A destination with no `#` is all path. */
    const bufsize_t hash = memchr(text + url, '#', (size_t)definition->url.len)
                               ? url + field_end(text + url, definition->url.len, angled, false)
                               : url_end;
    /* A destination with no path, `<>` or `#t`, holds an empty one, as
     * one with an empty fragment, `t#`, holds an empty anchor. */
    spots[count++] = (markdown_core_field_spot){url, hash < url_end ? hash : url_end, MARKDOWN_CORE_SLOT_DEST, true,
                                                MARKDOWN_CORE_FIELD};
    if (hash < url_end) {
        spots[count++] =
            (markdown_core_field_spot){hash + 1, url_end, MARKDOWN_CORE_SLOT_ANCHOR, true, MARKDOWN_CORE_FIELD};
    }
    if (definition->title.data) {
        const bufsize_t title = (bufsize_t)(definition->title.data - text);
        spots[count++] = (markdown_core_field_spot){title + 1, title + definition->title.len - 1,
                                                    MARKDOWN_CORE_SLOT_NONE, false, MARKDOWN_CORE_FIELD};
    }
    markdown_core_field_many(&to, spots, count);
    if (definition->listed && tails->ops) {
        tails->ops->fields(tails->self, &to);
    }
}

/* THE FRONT CUT'S ITEM (element.h, markdown_core_open_ops `item`): the
 * link reference definition at the front of `cut->rest`, `cut->at` bytes
 * into the block's text, which the construct reads and the handler makes a
 * Reference node, which the engine settles before the block. A cut its
 * item ends, with none or the last of the text, lets its recognizer go,
 * which adds what it read to the parse's work. */
typedef struct {
    reference_definition definition;
} S_item_found;
MARKDOWN_CORE_BLOCK_FOUND_FITS(S_item_found, S_item_found_fits);

enum { LINK_ITEM_REFERENCE, LINK_BLOCKS };

/* THE DEFINITION, code by code (micromark's definition and its label,
 * whitespace, destination and title factories): `[label]:`, whitespace,
 * the destination, then a title after whitespace when the line ends after
 * it, else none, then, after whitespace, an attribute list the attribute
 * grammar reads, and the line's end. The reading's offsets are the block's
 * text's; what it read is written in the start's record as it reads,
 * which outlives an attempt that fails, as the reports of a definition
 * given up need. The frame's words: a raw destination's open parentheses,
 * a title's closing byte, and where a whitespace or a tail began. */
enum { WORD_DEPTH, WORD_CLOSE = 0, WORD_FROM = 1, WORD_END = 2 };

static inline S_item_found *S_found_of(const markdown_core_tokenizer *tokenizer) {
    return MARKDOWN_CORE_BLOCK_FOUND(markdown_core_block_turn_of(tokenizer)->start, S_item_found);
}

static inline bool S_punct(int code) { return code > 0 && code < 0x80 && markdown_core_ispunct((char)code); }

static markdown_core_state S_yes(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return markdown_core_ok(tokenizer);
}

static markdown_core_state S_no(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return markdown_core_nok(tokenizer);
}

/* WHITESPACE: spaces, at most one line ending, spaces. */
static markdown_core_state S_space_more(markdown_core_tokenizer *tokenizer, int code) {
    return markdown_core_spaces(tokenizer, markdown_core_state_of(S_yes), 0, UINT32_MAX, code);
}

static markdown_core_state S_space_feed(markdown_core_tokenizer *tokenizer, int code) {
    if (code == '\n') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_space_more);
    }
    return S_space_more(tokenizer, code);
}

static markdown_core_state S_space_end(markdown_core_tokenizer *tokenizer, int code) {
    if (code == '\r') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_space_feed);
    }
    if (code == '\n') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_space_more);
    }
    return markdown_core_ok(tokenizer);
}

static markdown_core_state S_space_start(markdown_core_tokenizer *tokenizer, int code) {
    return markdown_core_spaces(tokenizer, markdown_core_state_of(S_space_end), 0, UINT32_MAX, code);
}

static const markdown_core_construct LINK_SPACE = {.start = S_space_start};

static inline markdown_core_state S_whitespace(markdown_core_tokenizer *tokenizer, markdown_core_state next, int code) {
    return markdown_core_attempt(tokenizer, &LINK_SPACE, next, next, code);
}

/* THE LINE'S END after a definition: spaces, then a line ending or the
 * text's end. */
static markdown_core_state S_end_feed(markdown_core_tokenizer *tokenizer, int code) {
    if (code == '\n') {
        markdown_core_consume(tokenizer);
    }
    return markdown_core_ok(tokenizer);
}

static markdown_core_state S_end_line(markdown_core_tokenizer *tokenizer, int code) {
    if (code == '\r') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_end_feed);
    }
    if (code == '\n') {
        markdown_core_consume(tokenizer);
        return markdown_core_ok(tokenizer);
    }
    return code == MARKDOWN_CORE_EOF ? markdown_core_ok(tokenizer) : markdown_core_nok(tokenizer);
}

static markdown_core_state S_end_start(markdown_core_tokenizer *tokenizer, int code) {
    return markdown_core_spaces(tokenizer, markdown_core_state_of(S_end_line), 0, UINT32_MAX, code);
}

static const markdown_core_construct LINK_END = {.start = S_end_start};

/* THE ATTRIBUTE TAIL: whitespace, a `{` whose list the attribute grammar
 * closes, and the line's end, the list then read into the definition. */
static markdown_core_state S_list_read(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    const markdown_core_need need = self->needs[LINK_ATTRIBUTES];
    reference_definition *definition = &S_found_of(tokenizer)->definition;
    const uint32_t *words = markdown_core_frame_local(tokenizer);
    markdown_core_attributes_free(&definition->value);
    definition->listed = ((const markdown_core_attribute_ops *)need.ops)
                             ->parse(need.instance, tokenizer->data, (bufsize_t)words[WORD_FROM],
                                     (bufsize_t)words[WORD_END], &definition->value);
    if (!definition->listed) {
        tokenizer->events.failed = true;
    }
    return definition->listed ? markdown_core_ok(tokenizer) : markdown_core_nok(tokenizer);
}

static markdown_core_state S_list_after(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_frame_local(tokenizer)[WORD_END] = tokenizer->position;
    return markdown_core_attempt(tokenizer, &LINK_END, markdown_core_state_of(S_list_read),
                                 markdown_core_state_of(S_no), code);
}

static markdown_core_state S_list_open(markdown_core_tokenizer *tokenizer, int code) {
    if (code != '{') {
        return markdown_core_nok(tokenizer);
    }
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    const markdown_core_need need = self->needs[LINK_ATTRIBUTES];
    reference_definition *definition = &S_found_of(tokenizer)->definition;
    const uint32_t at = tokenizer->position;
    /* An attribute list looked for reads on to where it ends, or to the
     * text's end. */
    definition->wide = true;
    if (definition->brace < 0) {
        definition->brace = (bufsize_t)at;
    }
    markdown_core_frame_local(tokenizer)[WORD_FROM] = at;
    return markdown_core_attempt(tokenizer, ((const markdown_core_attribute_ops *)need.ops)->list(need.instance, false),
                                 markdown_core_state_of(S_list_after), markdown_core_state_of(S_no), code);
}

static markdown_core_state S_list_start(markdown_core_tokenizer *tokenizer, int code) {
    return S_whitespace(tokenizer, markdown_core_state_of(S_list_open), code);
}

static const markdown_core_construct LINK_LIST = {.start = S_list_start};

/* The definition's end: an attribute list, when the dialect reads them,
 * or none. */
static markdown_core_state S_tail_plain(markdown_core_tokenizer *tokenizer, int code) {
    return markdown_core_attempt(tokenizer, &LINK_END, markdown_core_state_of(S_yes), markdown_core_state_of(S_no),
                                 code);
}

static markdown_core_state S_tail(markdown_core_tokenizer *tokenizer, int code) {
    if (((const markdown_core_element_instance *)markdown_core_self(tokenizer))->needs[LINK_ATTRIBUTES].ops) {
        return markdown_core_attempt(tokenizer, &LINK_LIST, markdown_core_state_of(S_yes),
                                     markdown_core_state_of(S_tail_plain), code);
    }
    return S_tail_plain(tokenizer, code);
}

/* THE TITLE: after whitespace, `"`, `'` or `(` to its closing byte, a
 * backslash escaping the punctuation after it; no `(` in one that `(`
 * opens. A NUL is a byte like any other. */
static markdown_core_state S_title_body(markdown_core_tokenizer *tokenizer, int code);

static markdown_core_state S_title_escape(markdown_core_tokenizer *tokenizer, int code) {
    if (S_punct(code)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_title_body);
    }
    return S_title_body(tokenizer, code);
}

static markdown_core_state S_title_body(markdown_core_tokenizer *tokenizer, int code) {
    const uint32_t close = markdown_core_frame_local(tokenizer)[WORD_CLOSE];
    if (code == MARKDOWN_CORE_EOF || (close == ')' && code == '(')) {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_consume(tokenizer);
    if ((uint32_t)code == close) {
        S_read *read = S_read_of(tokenizer);
        read->title = markdown_core_frame_local(tokenizer)[WORD_FROM];
        read->title_end = tokenizer->position;
        read->titled = true;
        return markdown_core_ok(tokenizer);
    }
    return code == '\\' ? markdown_core_state_of(S_title_escape) : markdown_core_state_of(S_title_body);
}

static markdown_core_state S_title_open(markdown_core_tokenizer *tokenizer, int code) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    if (tokenizer->position == words[WORD_FROM] || (code != '"' && code != '\'' && code != '(')) {
        return markdown_core_nok(tokenizer);
    }
    S_read_of(tokenizer)->opener = code;
    words[WORD_CLOSE] = code == '(' ? ')' : (uint32_t)code;
    words[WORD_FROM] = tokenizer->position;
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_title_body);
}

static markdown_core_state S_title_start(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_frame_local(tokenizer)[WORD_FROM] = tokenizer->position;
    return S_whitespace(tokenizer, markdown_core_state_of(S_title_open), code);
}

static const markdown_core_construct LINK_TITLE = {.start = S_title_start};

/* A DEFINITION'S TITLE: one the line's end follows. */
static markdown_core_state S_titled(markdown_core_tokenizer *tokenizer, int code) {
    const S_read *read = S_read_of(tokenizer);
    reference_definition *definition = &S_found_of(tokenizer)->definition;
    const markdown_core_chunk *text = &markdown_core_block_turn_of(tokenizer)->cut->text;
    definition->title = (markdown_core_chunk){text->data + read->title, (bufsize_t)(read->title_end - read->title), 0};
    return S_tail(tokenizer, code);
}

static markdown_core_state S_titled_start(markdown_core_tokenizer *tokenizer, int code) {
    return markdown_core_attempt(tokenizer, &LINK_TITLE, markdown_core_state_of(S_titled), markdown_core_state_of(S_no),
                                 code);
}

static const markdown_core_construct LINK_TITLED = {.start = S_titled_start};

/* No title: the line ends after the destination, the title the bytes
 * after it read stays none. */
static markdown_core_state S_untitled(markdown_core_tokenizer *tokenizer, int code) {
    S_found_of(tokenizer)->definition.title = (markdown_core_chunk)MARKDOWN_CORE_CHUNK_EMPTY;
    return S_tail(tokenizer, code);
}

/* The definition's title read or not: the opener seen says whether a
 * title was looked for. */
static markdown_core_state S_title_yes(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    S_found_of(tokenizer)->definition.opener = S_read_of(tokenizer)->opener;
    return markdown_core_ok(tokenizer);
}

static markdown_core_state S_title_no(markdown_core_tokenizer *tokenizer, int code) {
    S_found_of(tokenizer)->definition.opener = S_read_of(tokenizer)->opener;
    return S_untitled(tokenizer, code);
}

/* THE DESTINATION: `<` to `>`, no line ending and no `<` between, or a
 * run of bytes that are no space and no control, its parentheses balanced
 * to a depth of 32, a `)` that closes none ending it; a backslash escapes
 * the punctuation after it. A byte follows it. */
static markdown_core_state S_destined(markdown_core_tokenizer *tokenizer, uint32_t start, uint32_t end, int code) {
    if (code == MARKDOWN_CORE_EOF) {
        return markdown_core_nok(tokenizer);
    }
    S_read *read = S_read_of(tokenizer);
    read->dest = start;
    read->dest_end = end;
    return markdown_core_ok(tokenizer);
}

static markdown_core_state S_angle(markdown_core_tokenizer *tokenizer, int code);

static markdown_core_state S_angle_escape(markdown_core_tokenizer *tokenizer, int code) {
    if (S_punct(code)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_angle);
    }
    return S_angle(tokenizer, code);
}

static markdown_core_state S_angle_after(markdown_core_tokenizer *tokenizer, int code) {
    const uint32_t start = markdown_core_frame_local(tokenizer)[WORD_FROM];
    return S_destined(tokenizer, start, tokenizer->position - 1, code);
}

static markdown_core_state S_angle(markdown_core_tokenizer *tokenizer, int code) {
    if (code == MARKDOWN_CORE_EOF || code == '\n' || code == '\r' || code == '<') {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_consume(tokenizer);
    if (code == '>') {
        return markdown_core_state_of(S_angle_after);
    }
    return code == '\\' ? markdown_core_state_of(S_angle_escape) : markdown_core_state_of(S_angle);
}

static markdown_core_state S_raw(markdown_core_tokenizer *tokenizer, int code);

static markdown_core_state S_raw_escape(markdown_core_tokenizer *tokenizer, int code) {
    if (S_punct(code)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_raw);
    }
    return S_raw(tokenizer, code);
}

static markdown_core_state S_raw(markdown_core_tokenizer *tokenizer, int code) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    if (code == MARKDOWN_CORE_EOF) {
        return markdown_core_nok(tokenizer);
    }
    if (code == ')' && !words[WORD_DEPTH]) {
        return S_destined(tokenizer, words[WORD_FROM], tokenizer->position, code);
    }
    if (code >= 0 && (code <= 0x20 || code == 0x7F)) {
        return tokenizer->position == words[WORD_FROM]
                   ? markdown_core_nok(tokenizer)
                   : S_destined(tokenizer, words[WORD_FROM], tokenizer->position, code);
    }
    if (code == '(' && ++words[WORD_DEPTH] > 32) {
        return markdown_core_nok(tokenizer);
    }
    words[WORD_DEPTH] -= code == ')';
    markdown_core_consume(tokenizer);
    return code == '\\' ? markdown_core_state_of(S_raw_escape) : markdown_core_state_of(S_raw);
}

static markdown_core_state S_dest_start(markdown_core_tokenizer *tokenizer, int code) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    words[WORD_DEPTH] = 0;
    if (code == '<') {
        markdown_core_consume(tokenizer);
        words[WORD_FROM] = tokenizer->position;
        return markdown_core_state_of(S_angle);
    }
    words[WORD_FROM] = tokenizer->position;
    return S_raw(tokenizer, code);
}

static const markdown_core_construct LINK_DESTINATION = {.start = S_dest_start};

/* A DEFINITION'S DESTINATION, then its title or none. */
static markdown_core_state S_dested(markdown_core_tokenizer *tokenizer, int code) {
    S_read *read = S_read_of(tokenizer);
    reference_definition *definition = &S_found_of(tokenizer)->definition;
    const markdown_core_chunk *text = &markdown_core_block_turn_of(tokenizer)->cut->text;
    definition->url = (markdown_core_chunk){text->data + read->dest, (bufsize_t)(read->dest_end - read->dest), 0};
    definition->reached = 2;
    read->opener = 0;
    return markdown_core_attempt(tokenizer, &LINK_TITLED, markdown_core_state_of(S_title_yes),
                                 markdown_core_state_of(S_title_no), code);
}

static markdown_core_state S_dest_begin(markdown_core_tokenizer *tokenizer, int code) {
    return markdown_core_attempt(tokenizer, &LINK_DESTINATION, markdown_core_state_of(S_dested),
                                 markdown_core_state_of(S_no), code);
}

/* THE LABEL: `[`, bytes to the first `]` or `[` no backslash escapes, at
 * most 999 of them, then `:`. Its text trimmed is not empty. */
static markdown_core_state S_label(markdown_core_tokenizer *tokenizer, int code);

static markdown_core_state S_label_escape(markdown_core_tokenizer *tokenizer, int code) {
    if (S_punct(code)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_label);
    }
    return S_label(tokenizer, code);
}

static markdown_core_state S_colon_after(markdown_core_tokenizer *tokenizer, int code) {
    return S_whitespace(tokenizer, markdown_core_state_of(S_dest_begin), code);
}

static markdown_core_state S_colon(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_cut *cut = markdown_core_block_turn_of(tokenizer)->cut;
    reference_definition *definition = &S_found_of(tokenizer)->definition;
    const bufsize_t close = (bufsize_t)tokenizer->position - 1, open = cut->at + 1;
    definition->label_end = close - cut->at;
    if (code != ':') {
        return markdown_core_nok(tokenizer);
    }
    definition->reached = 1;
    definition->label = (markdown_core_chunk){cut->text.data + open, close - open, 0};
    markdown_core_chunk_trim(&definition->label);
    if (!definition->label.len) {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_colon_after);
}

static markdown_core_state S_label(markdown_core_tokenizer *tokenizer, int code) {
    const markdown_core_cut *cut = markdown_core_block_turn_of(tokenizer)->cut;
    if (code == MARKDOWN_CORE_EOF || code == '[' ||
        tokenizer->position - (uint32_t)cut->at - 1 > MAX_LINK_LABEL_LENGTH) {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_consume(tokenizer);
    if (code == ']') {
        return markdown_core_state_of(S_colon);
    }
    return code == '\\' ? markdown_core_state_of(S_label_escape) : markdown_core_state_of(S_label);
}

static markdown_core_state S_definition(markdown_core_tokenizer *tokenizer, int code) {
    if (code != '[') {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_label);
}

static const markdown_core_construct LINK_DEFINITION = {.start = S_definition};

/* THE FRONT CUT'S ITEM: the definition read, its length is the cut's; a
 * reading that measures makes none. */
static markdown_core_state S_item_read(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    markdown_core_cut *cut = markdown_core_block_turn_of(tokenizer)->cut;
    reference_definition *definition = &S_found_of(tokenizer)->definition;
    definition->wide |= !definition->title.data &&
                        (definition->opener == '"' || definition->opener == '\'' || definition->opener == '(');
    cut->wide = definition->wide;
    cut->length = (bufsize_t)tokenizer->position - cut->at;
    if (cut->measure) {
        markdown_core_attributes_free(&definition->value);
        return markdown_core_nok(tokenizer);
    }
    return markdown_core_block_found(tokenizer, LINK_ITEM_REFERENCE);
}

/* A definition given up keeps what it read on the paragraph. */
static markdown_core_state S_item_lost(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    markdown_core_cut *cut = turn->cut;
    reference_definition *definition = &S_found_of(tokenizer)->definition;
    markdown_core_attributes_free(&definition->value);
    cut->wide = definition->wide;
    cut->length = 0;
    if (!cut->measure) {
        S_report_unread(turn->parser, cut->block, &cut->rest, cut->at, definition);
        S_report_tail(self, turn->parser, cut->block, &cut->rest, cut->at, definition);
    }
    return markdown_core_nok(tokenizer);
}

static markdown_core_state S_item(markdown_core_tokenizer *tokenizer, int code) {
    reference_definition *definition = &S_found_of(tokenizer)->definition;
    *definition = (reference_definition){.brace = -1};
    return markdown_core_attempt(tokenizer, &LINK_DEFINITION, markdown_core_state_of(S_item_read),
                                 markdown_core_state_of(S_item_lost), code);
}

/* The definition at the front of `input`, read alone as a reading that
 * measures reads it: its length, 0 when there is none. */
bufsize_t markdown_core_reference_definition_length(const markdown_core_element_instance *self,
                                                    markdown_core_parser *parser, markdown_core_chunk *input) {
    block_start start = {.owner = self, .kind = MARKDOWN_CORE_NODE_NONE};
    markdown_core_cut cut = {.text = *input, .rest = *input, .measure = true};
    markdown_core_alone alone = {.turn = {.parser = parser, .start = &start, .cut = &cut}};
    const markdown_core_construct item = {.start = S_item, .context = self};
    (void)markdown_core_alone_read(parser, &alone, &item, input->data, (uint32_t)input->len);
    markdown_core_alone_end(parser, &alone);
    return cut.length;
}

static void S_item_make(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)token;
    (void)index;
    markdown_core_block_turn *turn = markdown_core_block_turned(compiler);
    const markdown_core_element_instance *self = markdown_core_compile_self(compiler);
    markdown_core_parser *parser = turn->parser;
    markdown_core_cut *cut = turn->cut;
    markdown_core_node *b = cut->block;
    const markdown_core_chunk *input = &cut->rest;
    const bufsize_t before = cut->at, length = cut->length;
    reference_definition *definition = &MARKDOWN_CORE_BLOCK_FOUND(turn->start, S_item_found)->definition;
    const markdown_core_need need = self->needs[LINK_ATTRIBUTES];
    const S_tails tails = {need.ops, need.instance};
    /* The destination and title are cleaned here, the way a direct link's
     * are, so a Reference and a direct link state the same values. */
    int lost = 0;
    markdown_core_chunk clean_url = markdown_core_clean_url(self, &definition->url, &lost);
    markdown_core_optional_chunk clean_title = markdown_core_clean_title(self, &definition->title, &lost);
    markdown_core_resource *resource = lost ? NULL : markdown_core_resource_new(parser->pool, clean_url, clean_title);
    markdown_core_node *reference =
        resource
            ? markdown_core_parser_make_node_with_ext(parser, MARKDOWN_CORE_NODE_REFERENCE, &MARKDOWN_CORE_ELEMENT_LINK)
            : NULL;
    if (!reference) {
        if (resource) {
            markdown_core_resource_free(&parser->pool->resources, resource);
        } else {
            markdown_core_chunk_free(&clean_url);
            markdown_core_optional_chunk_free(&clean_title);
        }
        markdown_core_attributes_free(&definition->value);
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    } else {
        reference->as.reference->resource = resource;
        reference->attributes = definition->value;
        reference->flags |= MARKDOWN_CORE_NODE__BLANK_TRANSPARENT;
        /* It spans its definition, through the end of its last line's
         * content: the line ending after it belongs to no block. */
        bufsize_t end = length;
        while (end > 0 && (input->data[end - 1] == '\n' || input->data[end - 1] == '\r')) {
            end--;
        }
        reference->place = (markdown_core_place){b->place.start, b->place.start};
        markdown_core_text_place(parser, &b->content_map, before, before + end, &reference->place);
        S_reference_fields(parser, reference, b, cut->text.data, definition, &tails);
        const bufsize_t start = (bufsize_t)reference->place.start;
        cut->made = reference;
        /* The document defines the label from where the definition starts. */
        if (!parser->error) {
            markdown_core_parser_declare(parser, self->spaces[LINK_REFERENCE], &definition->label, start,
                                         &reference->as.reference->label, reference);
        }
    }
}

/* The numeric character reference whose `#` is at `hash`, after an `&`:
 * its length past the `#` through its `;`, 0 when the bytes are no
 * reference, or -1 when the data ends while they may still be one. */
static bufsize_t numeric_reference(const unsigned char *data, bufsize_t hash, bufsize_t length) {
    bufsize_t i = hash + 1;
    const bool hex = i < length && (data[i] == 'x' || data[i] == 'X');
    i += hex;
    const bufsize_t digits = i, most = hex ? 6 : 7;
    while (i < length && i - digits < most &&
           (markdown_core_isdigit((char)data[i]) || (hex && (data[i] | 0x20) >= 'a' && (data[i] | 0x20) <= 'f'))) {
        i++;
    }
    if (i >= length) {
        return -1;
    }
    return i > digits && data[i] == ';' ? i + 1 - hash : 0;
}

/* HOW A LINK'S FIELDS GO ON PAST A CURSOR. A destination's path runs to
 * the `#` that starts its fragment, outside the parentheses it opens, as an
 * edit of the path alone must keep them whole, and the fragment to the destination's
 * end: a bare destination ends at a space, a control or a `)` that closes
 * no `(` of its own, as manual_scan_link_url_2 reads it, and one in angle
 * brackets at its `>`, a `<` or a line ending. A label runs to a bracket. A
 * backslash escapes the byte after it, and the `#` of a numeric character
 * reference starts no fragment. */
static bufsize_t field_end(const unsigned char *data, bufsize_t length, bool angled, bool fragment) {
    bufsize_t i = 0, amp = -1;
    size_t depth = 0;
    while (i < length) {
        const unsigned char c = data[i];
        /* References are decoded before escapes, so an escaped `&` still
         * begins one. */
        if (c == '\\' && i + 1 < length && markdown_core_ispunct(data[i + 1])) {
            i += data[i + 1] == '&' ? 1 : 2;
            continue;
        }
        if (c == '#' && !fragment && amp == i - 1) {
            const bufsize_t reference = numeric_reference(data, i, length);
            if (reference > 0) {
                i += reference;
                continue;
            }
        }
        amp = c == '&' ? i : amp;
        if ((!fragment && c == '#' && !depth) ||
            (angled ? c == '>' || c == '<' || markdown_core_is_line_end(c)
                    : destination_excludes(c) || (c == ')' && !depth) || (c == '(' && depth == 32))) {
            break;
        }
        depth += !angled && c == '(';
        depth -= !angled && c == ')';
        i++;
    }
    return i;
}
/* The fields of a destination read from `start` of the `len` bytes at
 * `data`, as far as its reading reads it, into `fields`, of a construct of
 * kind `owner`: its path, up to the first `#` that starts a fragment, and
 * the fragment after it, read from the path's start, through the `(` before
 * its `#` that a `)` after it closes; a destination in angle brackets holds
 * neither bracket. Their count. */
static size_t S_destination(const unsigned char *data, bufsize_t len, bufsize_t start, markdown_core_node_kind owner,
                            markdown_core_field *fields) {
    const bool angled = start < len && data[start] == '<';
    const bufsize_t path = angled ? start + 1 : start;
    const bufsize_t end = path + field_end(data + path, len - path, angled, true);
    const bufsize_t hash = path + field_end(data + path, end - path, angled, false);
    fields[0] = (markdown_core_field){MARKDOWN_CORE_SLOT_DEST, owner, path, hash};
    fields[1] = (markdown_core_field){MARKDOWN_CORE_SLOT_ANCHOR, owner, hash + 1, end};
    return hash < end ? 2 : 1;
}

/* The definition at the front of `input`, content offset `before` of the
 * paragraph `b`, given up with the destination read from `start`, kept on
 * the paragraph, the opener its `[`. */
static void S_report_destination_field(markdown_core_parser *parser, markdown_core_node *b,
                                       const markdown_core_chunk *input, bufsize_t before, bufsize_t start,
                                       bufsize_t len) {
    markdown_core_field fields[2];
    const size_t count = S_destination(input->data, len, start, MARKDOWN_CORE_KIND_REFERENCE, fields);
    const markdown_core_field_to to = {parser, b, &b->content_map, NULL, before, NULL};
    markdown_core_field_opener(&to, MARKDOWN_CORE_KIND_REFERENCE, true, 0, 1, fields, count);
}

/* The content of `input` without the spaces and line endings at its end. */
static bufsize_t S_content_length(const markdown_core_chunk *input) {
    bufsize_t len = input->len;
    while (len > 0 && markdown_core_is_whitespace(input->data[len - 1])) {
        len--;
    }
    return len;
}

/* A definition at the front of `input` that no definition reads yet, kept
 * on the paragraph: one whose destination the content's end has not begun,
 * `[label]: `, or whose destination in angle brackets it cuts short,
 * `[label]: <pa`, which no definition reads until its `>`; `definition`
 * says how far the recognizer read it. */
static void S_report_unread(markdown_core_parser *parser, markdown_core_node *b, const markdown_core_chunk *input,
                            bufsize_t before, const reference_definition *definition) {
    const unsigned char *data = input->data;
    const bufsize_t len = S_content_length(input);
    bufsize_t at = definition->label_end;
    if (!definition->reached || at == 1 || at + 1 >= len) {
        return;
    }
    /* No destination yet: a bare one starts after the spaces, at the end of
     * the input but for the line ending the block phase gave it. */
    bufsize_t stop = input->len, rest = at + 2;
    while (stop > rest && (data[stop - 1] == '\n' || data[stop - 1] == '\r')) {
        stop--;
    }
    while (rest < stop && (data[rest] == ' ' || data[rest] == '\t')) {
        rest++;
    }
    if (rest == stop) {
        S_report_destination_field(parser, b, input, before, stop, stop);
        return;
    }
    at = markdown_core_skip_spaces_and_line_end(data, at + 2, len);
    if (at < len && data[at] == '<') {
        S_report_destination_field(parser, b, input, before, at, len);
    }
}

/* The attribute block after a definition's destination or title that does
 * not close, kept on the paragraph `b` (services.h, `open`). */
static void S_report_tail(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_node *b, markdown_core_chunk *input, bufsize_t before,
                          const reference_definition *definition) {
    const markdown_core_need need = self->needs[LINK_ATTRIBUTES];
    const markdown_core_attribute_ops *attributes = need.ops;
    /* Only a definition whose destination was read opens a tail. */
    const bufsize_t brace =
        attributes && definition->reached == 2 && definition->brace >= 0 ? definition->brace - before : -1;
    if (brace >= 0) {
        const markdown_core_field_to to = {parser, b, &b->content_map, NULL, before, NULL};
        attributes->open(need.instance, &to, MARKDOWN_CORE_KIND_REFERENCE, input->data, S_content_length(input), brace);
    }
}

static markdown_core_bracket_match S_end_explicit(const markdown_core_element_instance *self,
                                                  markdown_core_tokenizer *tokenizer, markdown_core_opener *label,
                                                  markdown_core_label_made *made);
static markdown_core_bracket_match S_end_shortcut(const markdown_core_element_instance *self,
                                                  markdown_core_tokenizer *tokenizer, markdown_core_opener *label,
                                                  markdown_core_label_made *made);
static markdown_core_state S_after(markdown_core_tokenizer *tokenizer, int code);
static const markdown_core_construct LINK_AFTER = {.start = S_after};
static const markdown_core_close_rule LINK_CLOSES[] = {
    {.precedence = -20, .tail = &LINK_AFTER, .end = S_end_explicit},
    {.precedence = 10, .end = S_end_shortcut},
};

/* THE LINK'S TEXT (plan symbols-cleanroom.md 23.12): a `[` opens a label
 * (label.h), and the link's rules at its `]` read what follows it as the
 * reading stands there; a link or an image made is a GROUP token whose
 * word names what its rule read (S_found), kept beside the events. */
enum { LINK_GROUP, LINK_KINDS };

/* WHAT A RULE READ of a link or an image: where its group starts; an
 * inline tail's destination, `angled` when `<` and `>` hold it, and its
 * title inside its quotes (`titled`); a full reference's label inside its
 * brackets; the name of the definition a reference names, kept at `name`,
 * `name_size` bytes (none for a direct link); an image's dimensions and
 * its suffix's field, from `field` to `field_end`. */
typedef struct S_found {
    uint32_t start;
    uint32_t dest, dest_end, title_start, title_end, label_start, label_end;
    uint32_t name, name_size;
    uint32_t field, field_end;
    markdown_core_dimensions dimensions;
    bool image, resource, angled, titled, dimensioned;
} S_found;

/* WHAT A READING HOLDS OF THE LINK: what the explicit rule read at the
 * label whose token is `label`, which the shortcut rule after it reads. */
typedef struct S_asked {
    uint32_t label;
    markdown_core_link_match match;
    S_found found;
    uint32_t past;
} S_asked;

static markdown_core_node_kind S_kind_of(const markdown_core_opener *label) {
    return label->shape->kind == MARKDOWN_CORE_KIND_EMBEDDED ? MARKDOWN_CORE_KIND_EMBEDDED : MARKDOWN_CORE_KIND_LINK;
}

/* The link or image whose label is `label`, given up after its `(` with
 * the destination read from `start`. */
static void S_give_destination(markdown_core_tokenizer *tokenizer, const markdown_core_opener *label, uint32_t start) {
    const markdown_core_node_kind kind = S_kind_of(label);
    markdown_core_field fields[2];
    const size_t count = S_destination(tokenizer->data, (bufsize_t)tokenizer->size, (bufsize_t)start, kind, fields);
    markdown_core_text_give_up(tokenizer, kind, (bufsize_t)label->start, (bufsize_t)label->position, fields, count);
}

/* The same, given up after its `]` with the label of `[text][label` read
 * from the `[` at `start`, which no `]` closes. */
static void S_give_label(markdown_core_tokenizer *tokenizer, const markdown_core_opener *label, uint32_t start) {
    const unsigned char *data = tokenizer->data;
    const bufsize_t len = (bufsize_t)tokenizer->size;
    if ((bufsize_t)start >= len || data[start] != '[') {
        return;
    }
    const bufsize_t stop = (bufsize_t)start + 1 +
                           markdown_core_inline_reference_label_length(data + start + 1, len - (bufsize_t)start - 1);
    if (stop < len && data[stop] == ']') {
        return;
    }
    const markdown_core_node_kind kind = S_kind_of(label);
    const markdown_core_field field = {MARKDOWN_CORE_SLOT_LABEL, kind, (bufsize_t)start + 1, stop};
    markdown_core_text_give_up(tokenizer, kind, (bufsize_t)label->start, (bufsize_t)label->position, &field, 1);
}

/* WHAT FOLLOWS A LABEL'S `]`, code by code (micromark's resource and
 * full reference after a label end), checked before the link's explicit
 * rule is asked, its reading kept in the element's record: a resource,
 * `(`, whitespace, a destination, a title after whitespace or none,
 * whitespace, `)`; else a full reference's label, `[`, bytes to the first
 * `]` or `[` no backslash escapes, at most 999 of them, `]`. Only an image,
 * or a link where links may open, reads a resource. */
static markdown_core_state S_after_close(markdown_core_tokenizer *tokenizer, int code) {
    S_read *read = S_read_of(tokenizer);
    if (code == MARKDOWN_CORE_EOF || (!read->titled && (code == '"' || code == '\'' || code == '('))) {
        markdown_core_met_end(tokenizer);
    }
    if (code != ')') {
        read->given = true;
        return markdown_core_nok(tokenizer);
    }
    markdown_core_consume(tokenizer);
    read->resource = true;
    read->past = tokenizer->position;
    return markdown_core_ok(tokenizer);
}

static markdown_core_state S_after_titled(markdown_core_tokenizer *tokenizer, int code) {
    return S_whitespace(tokenizer, markdown_core_state_of(S_after_close), code);
}

static markdown_core_state S_after_untitled(markdown_core_tokenizer *tokenizer, int code) {
    S_read_of(tokenizer)->titled = false;
    return S_whitespace(tokenizer, markdown_core_state_of(S_after_close), code);
}

static markdown_core_state S_after_dested(markdown_core_tokenizer *tokenizer, int code) {
    return markdown_core_attempt(tokenizer, &LINK_TITLE, markdown_core_state_of(S_after_titled),
                                 markdown_core_state_of(S_after_untitled), code);
}

static markdown_core_state S_after_lost(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    markdown_core_met_end(tokenizer);
    S_read_of(tokenizer)->given = true;
    return markdown_core_nok(tokenizer);
}

static markdown_core_state S_after_dest(markdown_core_tokenizer *tokenizer, int code) {
    S_read_of(tokenizer)->from = tokenizer->position;
    return markdown_core_attempt(tokenizer, &LINK_DESTINATION, markdown_core_state_of(S_after_dested),
                                 markdown_core_state_of(S_after_lost), code);
}

static markdown_core_state S_after_open(markdown_core_tokenizer *tokenizer, int code) {
    return S_whitespace(tokenizer, markdown_core_state_of(S_after_dest), code);
}

static markdown_core_state S_after_label(markdown_core_tokenizer *tokenizer, int code);

static markdown_core_state S_after_escape(markdown_core_tokenizer *tokenizer, int code) {
    if (S_punct(code)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_after_label);
    }
    return S_after_label(tokenizer, code);
}

static markdown_core_state S_after_label(markdown_core_tokenizer *tokenizer, int code) {
    S_read *read = S_read_of(tokenizer);
    if (tokenizer->position - read->label_start > MAX_LINK_LABEL_LENGTH) {
        return markdown_core_nok(tokenizer);
    }
    if (code == MARKDOWN_CORE_EOF) {
        markdown_core_met_end(tokenizer);
        return markdown_core_nok(tokenizer);
    }
    if (code == '[') {
        return markdown_core_nok(tokenizer);
    }
    if (code == ']') {
        read->label_end = tokenizer->position;
        markdown_core_consume(tokenizer);
        read->labeled = true;
        read->past = tokenizer->position;
        return markdown_core_ok(tokenizer);
    }
    markdown_core_consume(tokenizer);
    return code == '\\' ? markdown_core_state_of(S_after_escape) : markdown_core_state_of(S_after_label);
}

static markdown_core_state S_after(markdown_core_tokenizer *tokenizer, int code) {
    const markdown_core_inline_state *inline_state = tokenizer->reader;
    const markdown_core_opener *label = markdown_core_opener_last(tokenizer);
    S_read *read = S_read_of(tokenizer);
    *read = (S_read){.label = label->token};
    if (code == MARKDOWN_CORE_EOF) {
        markdown_core_met_end(tokenizer);
        return markdown_core_nok(tokenizer);
    }
    if (code == '(' && (label->shape->kind == MARKDOWN_CORE_KIND_EMBEDDED || !inline_state->no_link_openers)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_after_open);
    }
    if (code == '[') {
        markdown_core_consume(tokenizer);
        read->label_start = tokenizer->position;
        return markdown_core_state_of(S_after_label);
    }
    return markdown_core_nok(tokenizer);
}

/* What follows the `]` of `label` at `after`, as its tail read it
 * (S_after): an inline tail, a reference by a label, or the label's own
 * body as a reference, into `found`, with where the tail ends in `*past`.
 * A label asked again (markdown_core_label_again) whose tail is not the
 * one last read is one a citation's tail waited on, which only a byte
 * that opens no resource and no label follows: its tail read nothing. */
static markdown_core_link_match S_recognize(const markdown_core_element_instance *self,
                                            markdown_core_tokenizer *tokenizer, const markdown_core_opener *label,
                                            uint32_t after, S_found *found, uint32_t *past) {
    markdown_core_inline_state *inline_state = tokenizer->reader;
    const unsigned char *data = tokenizer->data;
    const bool image = label->shape->kind == MARKDOWN_CORE_KIND_EMBEDDED;
    const bool allowed = image || !inline_state->no_link_openers;
    const S_read *kept = &((const S_state *)self->state)->read;
    const S_read read = kept->label == label->token ? *kept : (S_read){0};
    *found = (S_found){.start = label->start, .image = image};
    *past = after;
    if (read.given) {
        S_give_destination(tokenizer, label, read.from);
    }
    if (read.resource) {
        found->dest = read.dest;
        found->dest_end = read.dest_end;
        found->resource = true;
        found->angled = data[read.from] == '<';
        if (read.titled) {
            found->titled = true;
            found->title_start = read.title + 1;
            found->title_end = read.title_end - 1;
        }
        *past = read.past;
        return LINK_EXPLICIT;
    }
    markdown_core_chunk raw = markdown_core_chunk_literal("");
    bool labeled = read.labeled;
    if (labeled) {
        found->label_start = read.label_start;
        found->label_end = read.label_end;
        raw = (markdown_core_chunk){(unsigned char *)data + read.label_start,
                                    (bufsize_t)(read.label_end - read.label_start), 0};
        markdown_core_chunk_trim(&raw);
    } else {
        S_give_label(tokenizer, label, after);
    }
    const bool tail = labeled;
    if ((!labeled || raw.len == 0) && !label->after) {
        raw =
            (markdown_core_chunk){(unsigned char *)data + label->position, (bufsize_t)(after - label->position - 1), 0};
        labeled = true;
    }
    /* `[t][l]`, `[l][]` and `[l]` resolve identically and to the same
     * node: the one the definition names. */
    const markdown_core_mention *record = NULL;
    if (allowed && labeled) {
        bool failed = false;
        record = markdown_core_parser_find_definition(
            inline_state->owner_parser, self->spaces[LINK_REFERENCE], &raw,
            markdown_core_inline_state_root_offset(inline_state, (int)label->position), &failed);
        if (failed) {
            inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        }
    }
    if (!record) {
        return LINK_UNMATCHED;
    }
    const markdown_core_chunk name = markdown_core_mention_name(record);
    found->name_size = (uint32_t)name.len;
    found->name = name.len ? markdown_core_events_keep(&tokenizer->events, name.data, (size_t)name.len) : 0;
    if (tail) {
        *past = read.past;
    }
    return tail ? LINK_EXPLICIT : LINK_SHORTCUT;
}

/* The link or image `found` is made of `label`, its tail ending at
 * `past`: REJECTED where its holder takes none. */
static markdown_core_bracket_match S_make(const markdown_core_element_instance *self,
                                          markdown_core_tokenizer *tokenizer, const markdown_core_opener *label,
                                          S_found *found, uint32_t past, markdown_core_label_made *made) {
    const bool image = found->image;
    if (!markdown_core_attention_allows(tokenizer, MARKDOWN_CORE_NODE_NONE,
                                        image ? MARKDOWN_CORE_NODE_EMBEDDED : MARKDOWN_CORE_NODE_LINK)) {
        return BRACKET_REJECTED;
    }
    const uint32_t start = label->start;
    if (image) {
        /* Only the embedded element opens an image's label. */
        const markdown_core_need dimensions = self->needs[LINK_DIMENSIONS];
        const markdown_core_dimension_ops *ops = dimensions.ops;
        if (ops && ops->measure) {
            found->dimensioned = ops->measure(dimensions.instance, tokenizer, label, &found->dimensions, &found->field);
            /* The suffix runs to the closer. */
            found->field_end = markdown_core_event_token(&tokenizer->events, label->close)->start;
        }
    }
    const uint32_t kept = markdown_core_events_keep(&tokenizer->events, found, sizeof(*found));
    made->kind = (uint16_t)(self->token_base + LINK_GROUP);
    made->data = kept;
    made->from = start;
    made->past = past;
    made->holder = image ? MARKDOWN_CORE_NODE_EMBEDDED : MARKDOWN_CORE_NODE_LINK;
    made->links = image ? 0 : 1;
    return kept == UINT32_MAX ? BRACKET_REJECTED : BRACKET_MATCHED;
}

/* THE LINK'S RULES FOR A `]`, as the core's label end asks them: a link
 * whose destination, title or label follows the `]` is made first; a
 * shortcut, whose `]` is all of its tail, after the other rules. */
static markdown_core_bracket_match S_end_explicit(const markdown_core_element_instance *self,
                                                  markdown_core_tokenizer *tokenizer, markdown_core_opener *label,
                                                  markdown_core_label_made *made) {
    S_asked *asked = markdown_core_read_record(tokenizer, self->run_offset);
    const markdown_core_opener opener = *label;
    asked->label = opener.token;
    asked->match = S_recognize(self, tokenizer, &opener, made->past, &asked->found, &asked->past);
    if (asked->match != LINK_EXPLICIT) {
        return BRACKET_UNMATCHED;
    }
    const markdown_core_bracket_match match = S_make(self, tokenizer, &opener, &asked->found, asked->past, made);
    /* What follows a link whose destination, title or label follows its
     * `]` may update it (no-flush.md 10.13). */
    made->follows = asked->found.image ? MARKDOWN_CORE_NODE_EMBEDDED : MARKDOWN_CORE_NODE_LINK;
    return match;
}

static markdown_core_bracket_match S_end_shortcut(const markdown_core_element_instance *self,
                                                  markdown_core_tokenizer *tokenizer, markdown_core_opener *label,
                                                  markdown_core_label_made *made) {
    S_asked *asked = markdown_core_read_record(tokenizer, self->run_offset);
    const markdown_core_opener opener = *label;
    if (asked->label != opener.token) {
        return BRACKET_UNMATCHED;
    }
    if (asked->match == LINK_UNMATCHED && asked->found.label_end > asked->found.label_start) {
        /* A full reference, `[t][label]`, whose label names no definition. */
        const markdown_core_node_kind kind = S_kind_of(&opener);
        const markdown_core_field field = {MARKDOWN_CORE_SLOT_LABEL, kind, (bufsize_t)asked->found.label_start,
                                           (bufsize_t)asked->found.label_end};
        markdown_core_text_give_up(tokenizer, kind, (bufsize_t)opener.start, (bufsize_t)opener.position, &field, 1);
    }
    if (asked->match != LINK_SHORTCUT) {
        return BRACKET_UNMATCHED;
    }
    return S_make(self, tokenizer, &opener, &asked->found, made->past, made);
}

/* The labels no closer met, each a construct given up (report_open). */
static void S_left(const markdown_core_element_instance *self, markdown_core_tokenizer *tokenizer,
                   markdown_core_opener *b) {
    (void)self;
    const unsigned char *data = tokenizer->data;
    const bufsize_t len = (bufsize_t)tokenizer->size;
    const bufsize_t position = (bufsize_t)b->position;
    const bool own = b->shape == &MARKDOWN_CORE_LINK_BRACKET;
    const bufsize_t start = (bufsize_t)b->start;
    markdown_core_field fields[2];
    size_t count = 0;
    markdown_core_node_kind opens = own ? MARKDOWN_CORE_KIND_NONE : b->shape->kind;
    const bufsize_t stop = position + markdown_core_inline_reference_label_length(data + position, len - position);
    const bool label = own && stop == len;
    const bool caret = own && position < len && data[position] == '^';
    const bool line = start == 0 || data[start - 1] == '\n' || data[start - 1] == '\r';
    if (caret && label) {
        opens = MARKDOWN_CORE_KIND_CITE;
        fields[count++] =
            (markdown_core_field){MARKDOWN_CORE_SLOT_FOOTNOTE, MARKDOWN_CORE_KIND_CITATION, position + 1, stop};
    }
    if (line && caret) {
        bufsize_t note = position + 1;
        while (note < len && data[note] != ']' && !markdown_core_is_whitespace(data[note])) {
            note++;
        }
        fields[count++] =
            (markdown_core_field){MARKDOWN_CORE_SLOT_FOOTNOTE, MARKDOWN_CORE_KIND_FOOTNOTE, position + 1, note};
    } else if (line && label) {
        fields[count++] = (markdown_core_field){MARKDOWN_CORE_SLOT_LABEL, MARKDOWN_CORE_KIND_REFERENCE, position, stop};
    }
    markdown_core_text_give_up(tokenizer, opens, start, caret && label ? position + 1 : position, fields, count);
}

static markdown_core_state S_start(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return markdown_core_label_open(tokenizer, &MARKDOWN_CORE_LINK_BRACKET);
}

/* COMPILE: a link or an image over its body. */
static void S_open(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    const markdown_core_element_instance *self = markdown_core_compile_self(compiler);
    markdown_core_parser *parser = inline_state->owner_parser;
    const S_found *found = markdown_core_events_kept(compiler->events, token->data);
    const unsigned char *data = compiler->data;
    markdown_core_node *inl = markdown_core_inline_make_simple(inline_state, found->image ? MARKDOWN_CORE_NODE_EMBEDDED
                                                                                          : MARKDOWN_CORE_NODE_LINK);
    if (inl && found->name_size) {
        const unsigned char *name = markdown_core_events_kept(compiler->events, found->name);
        unsigned char *label = markdown_core_node_pool_bytes(parser->pool, (size_t)found->name_size + 1);
        if (label) {
            memcpy(label, name, found->name_size);
            label[found->name_size] = '\0';
            inl->as.link->label = (markdown_core_chunk){label, (bufsize_t)found->name_size, 0};
        } else {
            markdown_core_parser_release_node(parser, inl);
            inl = NULL;
        }
    } else if (inl) {
        int lost = 0;
        markdown_core_chunk url = {(unsigned char *)data + found->dest, (bufsize_t)(found->dest_end - found->dest), 0};
        markdown_core_chunk title =
            found->titled ? (markdown_core_chunk){(unsigned char *)data + found->title_start - 1,
                                                  (bufsize_t)(found->title_end - found->title_start + 2), 0}
                          : (markdown_core_chunk)MARKDOWN_CORE_CHUNK_EMPTY;
        markdown_core_chunk clean = markdown_core_clean_url(self, &url, &lost);
        markdown_core_optional_chunk titled = markdown_core_clean_title(self, &title, &lost);
        if (lost) {
            inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        }
        inl->as.link->resource = markdown_core_resource_new(parser->pool, clean, titled);
        if (!inl->as.link->resource) {
            markdown_core_chunk_free(&clean);
            markdown_core_optional_chunk_free(&titled);
            markdown_core_parser_release_node(parser, inl);
            inl = NULL;
        }
    }
    if (!inl) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        compiler->failed = true;
        return;
    }
    markdown_core_inline_state_place(inline_state, inl, (int)token->start, (int)token->end - 1);
    /* The fields of its tail (S_tail_fields). */
    const markdown_core_field_to to = markdown_core_inline_fields(inline_state, inl);
    markdown_core_field_spot spots[4];
    size_t count = 0;
    if (inl->as.link->resource) {
        const bufsize_t start = (bufsize_t)found->dest;
        const bufsize_t end = found->dest_end > found->dest ? (bufsize_t)found->dest_end : start;
        const bufsize_t hash = memchr(data + start, '#', (size_t)(end - start))
                                   ? start + field_end(data + start, end - start, found->angled, false)
                                   : end;
        spots[count++] = (markdown_core_field_spot){start, hash < end ? hash : end, MARKDOWN_CORE_SLOT_DEST, true,
                                                    MARKDOWN_CORE_FIELD};
        if (hash < end) {
            spots[count++] =
                (markdown_core_field_spot){hash + 1, end, MARKDOWN_CORE_SLOT_ANCHOR, true, MARKDOWN_CORE_FIELD};
        }
    }
    spots[count++] = (markdown_core_field_spot){(bufsize_t)found->title_start, (bufsize_t)found->title_end,
                                                MARKDOWN_CORE_SLOT_NONE, false, MARKDOWN_CORE_FIELD};
    if (!inl->as.link->resource) {
        spots[count++] = (markdown_core_field_spot){(bufsize_t)found->label_start, (bufsize_t)found->label_end,
                                                    MARKDOWN_CORE_SLOT_LABEL, false, MARKDOWN_CORE_FIELD};
    }
    markdown_core_field_many(&to, spots, count);
    if (found->dimensioned) {
        inl->as.link->dimensions.value = found->dimensions;
        inl->as.link->dimensions.has_value = true;
        markdown_core_field_add(&to, MARKDOWN_CORE_SLOT_NONE, (bufsize_t)found->field, (bufsize_t)found->field_end);
    }
    markdown_core_compile_open(compiler, inl);
}

static void S_close(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)token;
    (void)index;
    markdown_core_compile_pop(compiler);
}

static const markdown_core_text_construct LINK_CONSTRUCTS[] = {{"[", MARKDOWN_CORE_LABEL_CONSTRUCT(S_start)}};
static const markdown_core_compile_ops LINK_TOKENS[LINK_KINDS] = {
    [LINK_GROUP] = {.enter = S_open, .exit = S_close, .asked = true}};

static const markdown_core_inline_ops LINK_INLINES = {
    .constructs = LINK_CONSTRUCTS,
    .construct_count = sizeof(LINK_CONSTRUCTS) / sizeof(LINK_CONSTRUCTS[0]),
    .tokens = LINK_TOKENS,
    .token_count = LINK_KINDS,
    .left = S_left,
    .precedence = MARKDOWN_CORE_INLINE_FALLBACK,
    .closes = LINK_CLOSES,
    .close_count = sizeof(LINK_CLOSES) / sizeof(LINK_CLOSES[0]),
    .closer = ']',
};

/* A paragraph's text opens with its link reference definitions, which the
 * engine cuts off its front (blocks.c, THE FRONT CUT). */
/* A definition found and not made gives back its attributes. */
static void S_item_drop(const markdown_core_element_instance *self, block_start *start) {
    (void)self;
    markdown_core_attributes_free(&MARKDOWN_CORE_BLOCK_FOUND(start, S_item_found)->definition.value);
}

static const markdown_core_block_construct LINK_ITEM = {
    .bytes = "[", .construct = {.start = S_item}, .drop = S_item_drop};

static const markdown_core_compile_ops LINK_BLOCK_TOKENS[LINK_BLOCKS] = {
    [LINK_ITEM_REFERENCE] = {.enter = S_item_make},
};

static const markdown_core_open_ops LINK_OPENS = {
    .tokens = LINK_BLOCK_TOKENS,
    .token_count = LINK_BLOCKS,
    .item = &LINK_ITEM,
};

const markdown_core_facet MARKDOWN_CORE_LINK_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_LINK, .reads = MARKDOWN_CORE_READS_CHILDREN},
    {.kind = MARKDOWN_CORE_KIND_EMBEDDED, .reads = MARKDOWN_CORE_READS_CHILDREN},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

static bufsize_t S_label_length(const markdown_core_element_instance *self, const unsigned char *data, bufsize_t size) {
    (void)self;
    return markdown_core_inline_reference_label_length(data, size);
}

static const markdown_core_label_ops LINK_LABELS = {.length = S_label_length,
                                                    .definition = markdown_core_reference_definition_length};
static const markdown_core_service LINK_SERVICES[] = {{MARKDOWN_CORE_SERVICE_LABELS, &LINK_LABELS}, {0}};

const markdown_core_element MARKDOWN_CORE_ELEMENT_LINK = {
    .provides = LINK_SERVICES,
    .facets = MARKDOWN_CORE_LINK_FACETS,
    .needs = LINK_NEEDS,
    .spaces = LINK_SPACES,
    .state_size = sizeof(S_state),
    .run_state_size = sizeof(S_asked),
    .opens = &LINK_OPENS,
    .name = "link",
    .inlines = &LINK_INLINES,
};

/* A link's or an image's record (markdown_core_record_ops): its resource,
 * from the pool's resource slabs, and the label it was written with, in the
 * pool's storage. An extended autolink's destination views its text until
 * it settles. */
static void free_link(markdown_core_node_pool *pool, markdown_core_node *node) {
    markdown_core_resource_free(pool ? &pool->resources : NULL, node->as.link->resource);
    node->as.link->resource = NULL;
    if (node->as.link->label.len) {
        markdown_core_node_pool_bytes_free(pool, node->as.link->label.data);
    }
}

static bool viewed_link(markdown_core_node *node) {
    markdown_core_resource *resource = node->as.link->resource;
    return !resource || (markdown_core_chunk_own_viewed(&resource->url) &&
                         (!resource->title.has_value || markdown_core_chunk_own_viewed(&resource->title.value)));
}

const markdown_core_record_ops MARKDOWN_CORE_RECORD_LINK = {
    .size = sizeof(markdown_core_link),
    .free = free_link,
    .viewed = viewed_link,
};

/* A link reference definition's record: its resource and its label, as a
 * link's. */
static void free_reference(markdown_core_node_pool *pool, markdown_core_node *node) {
    markdown_core_resource_free(pool ? &pool->resources : NULL, node->as.reference->resource);
    node->as.reference->resource = NULL;
    if (node->as.reference->label.len) {
        markdown_core_node_pool_bytes_free(pool, node->as.reference->label.data);
    }
}

const markdown_core_record_ops MARKDOWN_CORE_RECORD_REFERENCE = {
    .size = sizeof(markdown_core_reference_value),
    .free = free_reference,
};
