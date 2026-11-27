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
 * grammar, which the elements whose constructs take a block after them ask.
 *
 * A RECOGNIZER is what the block's construct reads with (its context): the
 * memo of the reading it reads, by position, of what its walks passed (its
 * `ends`, `count` of them from `base`), which the next reading clears, so
 * overlapping candidates cannot scan the bytes again. Where its last walk
 * ended is `end`, 0 when the block did not close. One over `length` bytes
 * at `data` reads alone (markdown_core_attributes_end), with a tokenizer of
 * its own (`own`), its readings of the same bytes sharing its memo. Its
 * `scratch` is the workspace values are decoded into, which totals the
 * work. Allocation failure is sticky in `oom`. `ran_out`: a walk of the
 * memo met the end of the bytes, so what the walks answered may change
 * once the input goes on. */
struct markdown_core_attribute_suffix;
struct markdown_core_attribute_scratch;
struct markdown_core_attribute_own;
struct markdown_core_field;
typedef struct markdown_core_attribute_parser {
    const unsigned char *data;
    bufsize_t length;
    struct markdown_core_attribute_suffix *ends;
    bufsize_t base, count, room;
    /* The reading the memo is of. */
    const void *reading;
    const unsigned char *bytes;
    uint32_t size, serial;
    bufsize_t end;
    struct markdown_core_attribute_scratch *scratch;
    struct markdown_core_attribute_own *own;
    size_t work;
    int oom;
    int ran_out;
} markdown_core_attribute_parser;

/* `list`: the block's construct, handed the `{`: ATTRIBUTES_LINE when
 * `line`, which a line's end fails, else ATTRIBUTES_TEXT, whose members
 * may be one line ending apart. It makes no tokens.
 * `parse`: the block that closes at `end`, its `{` at `start` of `data`,
 * decoded into `value`, which the caller gives empty: success transfers
 * the value's allocations, and failure, which is allocation's, leaves it as
 * it was.
 * `fields`: the members the last `parse` read, as the fields of the node
 * `to` names, the parsed bytes' offsets placed as it places them.
 * `give`: the block at `start` of an inline reading that did not close, of
 * a construct of kind `owner`, given up with the members it read; asked
 * once for that block.
 * `report`: the blocks a block of kind `owner` opens and does not close on
 * its line `line`, at or after `from`, before `end`, kept on `to`'s node;
 * whether there is one.
 * `open`: a block that does not close, the block's `{` at `start` of the
 * `size` bytes at `data`, kept on `to`'s node with the members it read
 * (attributes.h, markdown_core_attributes_open), a line's; false when it
 * is no block whatever follows. */
typedef struct markdown_core_attribute_ops {
    const markdown_core_construct *(*list)(const markdown_core_element_instance *self, bool line);
    bool (*parse)(const markdown_core_element_instance *self, const unsigned char *data, bufsize_t start, bufsize_t end,
                  markdown_core_attributes *value);
    void (*fields)(const markdown_core_element_instance *self, const markdown_core_field_to *to);
    void (*give)(const markdown_core_element_instance *self, markdown_core_tokenizer *tokenizer, bufsize_t start,
                 markdown_core_node_kind owner);
    bool (*report)(const markdown_core_element_instance *self, const markdown_core_field_to *to,
                   markdown_core_node_kind owner, const unsigned char *line, bufsize_t from, bufsize_t end);
    bool (*open)(const markdown_core_element_instance *self, const markdown_core_field_to *to,
                 markdown_core_node_kind owner, const unsigned char *data, bufsize_t size, bufsize_t start);
} markdown_core_attribute_ops;

#define MARKDOWN_CORE_SERVICE_ATTRIBUTES "attributes"

/* AN IMAGE'S DIMENSIONS, `![alt|320x240](src)`: the embedded element's
 * grammar of a label's dimension suffix.
 *
 * `parse`: the suffix at `suffix` of `label`, after a separator of
 * `separator` bytes, into `value`, adding the bytes it examined to `work`;
 * false when it is none. It searches nothing and allocates nothing. */
typedef struct markdown_core_dimension_ops {
    bool (*parse)(const markdown_core_element_instance *self, markdown_core_chunk label, bufsize_t suffix,
                  bufsize_t separator, markdown_core_dimensions *value, size_t *work);
    /* `measure`: the image the label `label` (label.h) makes, whose body
     * ends at its end's enter event (`close`): when its label's suffix
     * states dimensions, they in `*value` and where the suffix's field
     * starts in `*field`, and the suffix leaves the body's last text. */
    bool (*measure)(const markdown_core_element_instance *self, markdown_core_tokenizer *tokenizer,
                    const struct markdown_core_opener *label, markdown_core_dimensions *value, uint32_t *field);
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
