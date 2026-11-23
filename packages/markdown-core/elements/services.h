#ifndef MARKDOWN_CORE_ELEMENT_SERVICES_H
#define MARKDOWN_CORE_ELEMENT_SERVICES_H
#include "element.h"
#include "delimiter.h"
#include "buffer.h"
#include "../core/attributes.h"
#include "inlines.h"

/* THE SERVICES ELEMENTS OFFER EACH OTHER (element.h, markdown_core_service):
 * the interfaces, one per kind of question, and the names a dialect files
 * them under. No element owns this header and it holds no grammar: an
 * element that provides a service implements its interface with its own
 * grammar, and one that asks a service reads its answer through the need
 * sealing resolved (markdown-core-element-api.h, markdown_core_need), never
 * the provider's functions. Every operation takes the provider's instance
 * first. The helpers below answer for a service the dialect does not
 * provide, as its interface states. */

/* A DECODER: how a slice of source another element reads as a value -- a
 * link's destination and title, a fence's info string, an autolink, an
 * attribute's quoted value -- is decoded.
 *
 * `read`: the unit whose bytes after its sigil begin `data`: appends its
 * value to `out` and returns the bytes it spans, or 0 when none begins
 * there. NULL for a decoder that reads no unit alone.
 * `decode`: `data` with every unit decoded, appended to `out`; false, and
 * nothing appended, when it holds none. NULL for a decoder that decodes in
 * place only.
 * `strip`: `buf` decoded in place, for a decoder whose value is never longer
 * than its source. NULL for any other. */
typedef struct markdown_core_decoder {
    bufsize_t (*read)(const markdown_core_element_instance *self, markdown_core_strbuf *out, const uint8_t *data,
                      bufsize_t size);
    bool (*decode)(const markdown_core_element_instance *self, markdown_core_strbuf *out, const uint8_t *data,
                   bufsize_t size);
    void (*strip)(const markdown_core_element_instance *self, markdown_core_strbuf *buf);
} markdown_core_decoder;

/* The backslash escapes of a slice (CommonMark 0.31.2, 2.4), and its entity
 * and numeric character references (2.5). */
#define MARKDOWN_CORE_SERVICE_ESCAPES "escapes"
#define MARKDOWN_CORE_SERVICE_REFERENCES "references"

/* What a decoder the dialect may not provide answers: none decodes nothing. */
static inline bufsize_t markdown_core_decoder_read(markdown_core_need need, markdown_core_strbuf *out,
                                                   const uint8_t *data, bufsize_t size) {
    const markdown_core_decoder *decoder = need.ops;
    return decoder && decoder->read ? decoder->read(need.instance, out, data, size) : 0;
}
static inline bool markdown_core_decoder_decode(markdown_core_need need, markdown_core_strbuf *out, const uint8_t *data,
                                                bufsize_t size) {
    const markdown_core_decoder *decoder = need.ops;
    return decoder && decoder->decode && decoder->decode(need.instance, out, data, size);
}
/* `data` decoded, or as it is when nothing in it decodes, appended to `out`. */
static inline void markdown_core_decoder_write(markdown_core_need need, markdown_core_strbuf *out, const uint8_t *data,
                                               bufsize_t size) {
    if (!markdown_core_decoder_decode(need, out, data, size)) {
        markdown_core_strbuf_put(out, data, size);
    }
}
static inline void markdown_core_decoder_strip(markdown_core_need need, markdown_core_strbuf *buf) {
    const markdown_core_decoder *decoder = need.ops;
    if (decoder && decoder->strip) {
        decoder->strip(need.instance, buf);
    }
}

/* A FLANKING RULE (delimiter.h, markdown_core_flank): what the neighbours of
 * a delimiter run make of it, MARKDOWN_CORE_FLANK_OPENS and _CLOSES. The
 * paired-run elements take emphasis's `*` rule as their own. */
typedef struct markdown_core_flank_rule {
    unsigned (*sides)(const markdown_core_element_instance *self, markdown_core_flank flank);
} markdown_core_flank_rule;

#define MARKDOWN_CORE_SERVICE_FLANK "flank"

/* No rule: the run neither opens nor closes. */
static inline unsigned markdown_core_flank_sides(markdown_core_need need, markdown_core_flank flank) {
    const markdown_core_flank_rule *rule = need.ops;
    return rule ? rule->sides(need.instance, flank) : 0u;
}

/* A WORD SPACE: a literal space that reads as a no-break space inside a
 * word -- an escaped space (CommonMark leaves `\ ` to extensions) -- which
 * the text element decodes once inline ownership is final. `note`: the
 * literal of the text node `node` is one. With no provider it stays a
 * space. */
typedef struct markdown_core_word_space {
    void (*note)(const markdown_core_element_instance *self, markdown_core_node *node);
} markdown_core_word_space;

#define MARKDOWN_CORE_SERVICE_WORD_SPACES "word spaces"

static inline void markdown_core_word_space_note(markdown_core_need need, markdown_core_node *node) {
    const markdown_core_word_space *spaces = need.ops;
    if (spaces) {
        spaces->note(need.instance, node);
    }
}

/* AN ATTRIBUTE BLOCK, `{#id .class key=value}`: the attribute element's
 * recognizer and decoder, which the elements whose constructs take a block
 * after them ask.
 *
 * A RECOGNIZER belongs to one immutable input extent, the `length` bytes at
 * `data`. It walks forward from each candidate brace it is asked about and
 * memoises, by position, what it passes (its `ends`, the provider's), so
 * overlapping candidates cannot scan that extent again. One zeroed but for
 * its extent recognizes only; `begin` lends it the parse's workspace, into
 * which `parse` decodes values, and `release` lets it go either way,
 * adding what it read to the parse's work. Allocation failure is sticky in
 * `oom` and never becomes a grammar fallback. `ran_out`: a quote's
 * lookahead met the extent's end with no closer, so what the walks answered
 * may change once the input goes on. */
struct markdown_core_attribute_suffix;
struct markdown_core_attribute_scratch;
struct markdown_core_member_memo;
struct markdown_core_field;
typedef struct markdown_core_attribute_parser {
    const unsigned char *data;
    bufsize_t length;
    struct markdown_core_attribute_suffix *ends;
    struct markdown_core_attribute_scratch *scratch;
    size_t work;
    int oom;
    int ran_out;
} markdown_core_attribute_parser;

/* `end`: one past the `}` of the block whose `{` is at `start`, or 0 when
 * none closes there; recognition only.
 * `tail`: where the first complete block that ends at `end` starts, outside
 * escaped punctuation, at or after `start`; -1 when none does.
 * `parse`: the block at `start` decoded into `value`, which the caller gives
 * empty, and `end` past it: success transfers the value's allocations, and
 * failure leaves both as they were.
 * `closes`: where a block at content offset `start` of a construct of kind
 * `owner` of the run closes, one past its `}`, or 0; the block's value is
 * the attribute element's to write, on the node the construct makes.
 * `report`: the blocks a block of kind `owner` may open on the line `line`
 * being read, at or after `from`, that the line's end at `end` leaves open,
 * to the parse's context sink; whether there is one.
 * `member`: the member a block that does not close is in at its end, the
 * block's `{` at `start` of the `size` bytes at `data`, into `field`;
 * false when it is no block whatever follows. `memo`, or NULL, is shared by
 * the braces asked about on the same bytes. */
typedef struct markdown_core_attribute_ops {
    void (*begin)(const markdown_core_element_instance *self, markdown_core_attribute_parser *p,
                  const unsigned char *data, bufsize_t size);
    void (*release)(const markdown_core_element_instance *self, markdown_core_attribute_parser *p);
    bufsize_t (*end)(const markdown_core_element_instance *self, markdown_core_attribute_parser *p, bufsize_t start);
    bufsize_t (*tail)(const markdown_core_element_instance *self, markdown_core_attribute_parser *p, bufsize_t start,
                      bufsize_t end);
    bool (*parse)(const markdown_core_element_instance *self, markdown_core_attribute_parser *p, bufsize_t start,
                  markdown_core_attributes *value, bufsize_t *end);
    bufsize_t (*closes)(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state,
                        bufsize_t start, markdown_core_node_kind owner);
    bool (*report)(const markdown_core_element_instance *self, markdown_core_parser *parser,
                   markdown_core_node_kind owner, const unsigned char *line, bufsize_t from, bufsize_t end);
    bool (*member)(const markdown_core_element_instance *self, const unsigned char *data, bufsize_t size,
                   bufsize_t start, struct markdown_core_field *field, struct markdown_core_member_memo *memo);
} markdown_core_attribute_ops;

#define MARKDOWN_CORE_SERVICE_ATTRIBUTES "attributes"

/* AN IMAGE'S DIMENSIONS, `![alt|320x240](src)`: the embedded element's
 * grammar of a label's dimension suffix.
 *
 * `parse`: the suffix at `suffix` of `label`, after a separator of
 * `separator` bytes, into `value`, adding the bytes it examined to `work`;
 * false when it is none. It searches nothing and allocates nothing.
 * `record`: the run's text from its cursor to `end`, a text slice, inside
 * the innermost bracket, whose last `|` an image's label keeps.
 * `apply`: an image the bracket `opener` made, its label ending at `end`,
 * takes the dimensions its label's suffix states, and the suffix leaves its
 * text. */
struct bracket;
typedef struct markdown_core_dimension_ops {
    bool (*parse)(const markdown_core_element_instance *self, markdown_core_chunk label, bufsize_t suffix,
                  bufsize_t separator, markdown_core_dimensions *value, size_t *work);
    void (*record)(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state, bufsize_t end);
    void (*apply)(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state,
                  const struct bracket *opener, markdown_core_node *image, bufsize_t end);
} markdown_core_dimension_ops;

#define MARKDOWN_CORE_SERVICE_DIMENSIONS "dimensions"

/* A REFERENCE LABEL, `[label]`: the link element's grammar of a label and
 * of the link reference definition that declares one.
 *
 * `length`: the bytes of the label whose `[` begins `data`, through its
 * `]`, or 0 when none begins there.
 * `definition`: the bytes of the link reference definition at the front of
 * `input`, 0 when there is none. */
typedef struct markdown_core_label_ops {
    bufsize_t (*length)(const markdown_core_element_instance *self, const unsigned char *data, bufsize_t size);
    bufsize_t (*definition)(const markdown_core_element_instance *self, markdown_core_parser *parser,
                            markdown_core_chunk *input);
} markdown_core_label_ops;

#define MARKDOWN_CORE_SERVICE_LABELS "labels"

/* A LINE THE INPUT'S END CUTS SHORT that another element's block opens on:
 * `report` gives the parse's context sink the fields the line from `first`
 * to `end` leaves open, which only the provider's grammar knows -- a
 * directive's name after a definition body's `:`, a topic's after a
 * heading's `#` signs. */
typedef struct markdown_core_report_ops {
    void (*report)(const markdown_core_element_instance *self, markdown_core_parser *parser, bufsize_t first,
                   bufsize_t end);
} markdown_core_report_ops;

#define MARKDOWN_CORE_SERVICE_DIRECTIVE_LINES "directive lines"
#define MARKDOWN_CORE_SERVICE_TOPIC_LINES "topic lines"

static inline void markdown_core_line_report(markdown_core_need need, markdown_core_parser *parser, bufsize_t first,
                                             bufsize_t end) {
    const markdown_core_report_ops *lines = need.ops;
    if (lines) {
        lines->report(need.instance, parser, first, end);
    }
}

/* AN OPAQUE SPAN another element's scan steps over whole, its bytes no
 * syntax of the asker's: a code span, an HTML tag. `skip`: where the span
 * whose first byte is at `at` of the run ends, one past it, or 0 when none
 * does; then `*open`, else `at`, is where an opener that no closer of the
 * run closes ends, which the input's end may yet close. `memo`, zeroed
 * before a scan's first ask, keeps what the asks of one scan share, and
 * `release`, NULL when there is nothing to let go, lets it go. */
typedef union markdown_core_skip_memo {
    void *pointer;
    uint64_t words[3];
} markdown_core_skip_memo;

typedef struct markdown_core_skip_ops {
    bufsize_t (*skip)(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state,
                      bufsize_t at, markdown_core_skip_memo *memo, bufsize_t *open);
    void (*release)(const markdown_core_element_instance *self, markdown_core_skip_memo *memo);
} markdown_core_skip_ops;

#define MARKDOWN_CORE_SERVICE_CODE_SPANS "code spans"
#define MARKDOWN_CORE_SERVICE_HTML_TAGS "html tags"

/* None skips nothing. */
static inline bufsize_t markdown_core_skip(markdown_core_need need, markdown_core_inline_state *inline_state,
                                           bufsize_t at, markdown_core_skip_memo *memo, bufsize_t *open) {
    const markdown_core_skip_ops *ops = need.ops;
    *open = at;
    return ops ? ops->skip(need.instance, inline_state, at, memo, open) : 0;
}
static inline void markdown_core_skip_release(markdown_core_need need, markdown_core_skip_memo *memo) {
    const markdown_core_skip_ops *ops = need.ops;
    if (ops && ops->release) {
        ops->release(need.instance, memo);
    }
    *memo = (markdown_core_skip_memo){0};
}

/* A CONSTRUCT ANOTHER ELEMENT'S GRAMMAR READS and this element makes its
 * node of: `make` gives the node of the `size` bytes after the run's byte
 * at `at`, past which it moves the run's cursor -- an HTML comment the raw
 * HTML grammar read, `size` bytes after its `<`. */
typedef struct markdown_core_make_ops {
    markdown_core_node *(*make)(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state,
                                bufsize_t at, bufsize_t size);
} markdown_core_make_ops;

#define MARKDOWN_CORE_SERVICE_HTML_COMMENTS "html comments"

#endif
