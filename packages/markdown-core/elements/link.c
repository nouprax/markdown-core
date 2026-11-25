#include <string.h>

#include "alloc.h"
#include "services.h"
#include "link_scanners.h"
#define advance(inline_state) markdown_core_inline_seek((inline_state), markdown_core_inline_offset(inline_state) + 1)
#include "link.h"
#include "inline_internal.h"
#include "block_internal.h"

/* The services this element asks, as `self->needs` holds them. */
enum { LINK_DIMENSIONS, LINK_ATTRIBUTES, LINK_ESCAPES, LINK_REFERENCES };
static const char *const LINK_NEEDS[] = {[LINK_DIMENSIONS] = MARKDOWN_CORE_SERVICE_DIMENSIONS,
                                         [LINK_ATTRIBUTES] = MARKDOWN_CORE_SERVICE_ATTRIBUTES,
                                         [LINK_ESCAPES] = MARKDOWN_CORE_SERVICE_ESCAPES,
                                         [LINK_REFERENCES] = MARKDOWN_CORE_SERVICE_REFERENCES,
                                         NULL};

/* A link's bracket, opened by a `[` (bracket.h). */
static const markdown_core_bracket_shape MARKDOWN_CORE_LINK_BRACKET = {
    .width = 1, .mark = 0, .closer = ']', .kind = MARKDOWN_CORE_KIND_LINK};

const markdown_core_space MARKDOWN_CORE_REFERENCE_SPACE = {.name = "reference", .folds = true};
/* The spaces this element files labels in, as `self->spaces` numbers them. */
enum { LINK_REFERENCE };
static const markdown_core_space *const LINK_SPACES[] = {[LINK_REFERENCE] = &MARKDOWN_CORE_REFERENCE_SPACE, NULL};

/* WHAT A PARSE HOLDS OF THE LINK ELEMENT: the recognizer of the attribute
 * blocks of the text a front cut reads (element.h, markdown_core_cut), kept
 * across its items, so that the many blocks of one text are read linearly. */
typedef struct {
    markdown_core_attribute_parser held;
} S_state;

/* The held recognizer let go, which only the grammar that began it held. */
static void S_release(const markdown_core_element_instance *self) {
    markdown_core_attribute_parser *held = &((S_state *)self->state)->held;
    const markdown_core_need need = self->needs[LINK_ATTRIBUTES];
    if (need.ops) {
        ((const markdown_core_attribute_ops *)need.ops)->release(need.instance, held);
    }
    *held = (markdown_core_attribute_parser){0};
}

static void dispose_parser(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    (void)parser;
    S_release(self);
}

static bufsize_t markdown_core_inline_manual_scan_link_url(markdown_core_chunk *input, bufsize_t offset,
                                                           markdown_core_chunk *output);
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

int markdown_core_inline_link_label(markdown_core_inline_state *inline_state, markdown_core_chunk *raw_label) {
    bufsize_t startpos = markdown_core_inline_offset(inline_state);
    int length = 0;
    unsigned char c;

    // advance past [
    if (markdown_core_inline_peek_char(inline_state) == '[') {
        markdown_core_inline_seek(inline_state, markdown_core_inline_offset(inline_state) + 1);
    } else {
        return 0;
    }

    length = markdown_core_inline_reference_label_length(
        markdown_core_inline_window(inline_state, 0).data + markdown_core_inline_offset(inline_state),
        markdown_core_inline_size(inline_state) - markdown_core_inline_offset(inline_state));
    markdown_core_inline_seek(inline_state, markdown_core_inline_offset(inline_state) + (length));
    if (length > MAX_LINK_LABEL_LENGTH) {
        goto noMatch;
    }
    c = markdown_core_inline_peek_char(inline_state);

    if (c == ']') { // match found
        *raw_label = markdown_core_inline_take(inline_state, startpos + 1,
                                               markdown_core_inline_offset(inline_state) - (startpos + 1));
        markdown_core_chunk_trim(raw_label);
        markdown_core_inline_seek(inline_state, markdown_core_inline_offset(inline_state) + 1); // advance past ]
        return 1;
    }

noMatch:
    markdown_core_inline_seek(inline_state, startpos); // rewind
    return 0;
}

/* An unbracketed destination includes no ASCII control character (U+0000 to
 * U+001F, U+007F) and no space; tab and the line endings are controls. */
static bool destination_excludes(unsigned char c) { return c <= 0x20 || c == 0x7F; }

static bufsize_t manual_scan_link_url_2(markdown_core_chunk *input, bufsize_t offset, markdown_core_chunk *output) {
    bufsize_t i = offset;
    size_t nb_p = 0;

    while (i < input->len) {
        if (input->data[i] == '\\' && i + 1 < input->len && markdown_core_ispunct(input->data[i + 1])) {
            i += 2;
        } else if (input->data[i] == '(') {
            ++nb_p;
            ++i;
            if (nb_p > 32) {
                return -1;
            }
        } else if (input->data[i] == ')') {
            if (nb_p == 0) {
                break;
            }
            --nb_p;
            ++i;
        } else if (destination_excludes(input->data[i])) {
            if (i == offset) {
                return -1;
            }
            break;
        } else {
            ++i;
        }
    }

    if (i >= input->len) {
        return -1;
    }

    {
        markdown_core_chunk result = {input->data + offset, i - offset, 0};
        *output = result;
    }
    return i - offset;
}

static bufsize_t markdown_core_inline_manual_scan_link_url(markdown_core_chunk *input, bufsize_t offset,
                                                           markdown_core_chunk *output) {
    bufsize_t i = offset;

    if (i < input->len && input->data[i] == '<') {
        ++i;
        while (i < input->len) {
            if (input->data[i] == '>') {
                ++i;
                break;
            } else if (input->data[i] == '\\' && i + 1 < input->len && markdown_core_ispunct(input->data[i + 1])) {
                i += 2;
            } else if (markdown_core_is_line_end(input->data[i]) || input->data[i] == '<') {
                return -1;
            } else {
                ++i;
            }
        }
    } else {
        return manual_scan_link_url_2(input, offset, output);
    }

    if (i >= input->len) {
        return -1;
    }

    {
        markdown_core_chunk result = {input->data + offset + 1, i - 2 - offset, 0};
        *output = result;
    }
    return i - offset;
}

static void spnl(markdown_core_inline_state *inline_state) {
    const markdown_core_chunk window = markdown_core_inline_window(inline_state, 0);
    markdown_core_inline_seek(inline_state, markdown_core_skip_spaces_and_line_end(
                                                window.data, markdown_core_inline_offset(inline_state), window.len));
}

/* What a definition's attribute tail is read with: the attribute grammar
 * (services.h, markdown_core_attribute_ops), its provider and the
 * recognizer over the definition's text. */
typedef struct {
    const markdown_core_attribute_ops *ops;
    const markdown_core_element_instance *self;
    markdown_core_attribute_parser *p;
} S_tails;

static bool reference_tail(markdown_core_inline_state *inline_state, const S_tails *tails,
                           markdown_core_attributes *value, bool *wide, bool *listed) {
    bufsize_t before = markdown_core_inline_offset(inline_state);
    *listed = false;
    if (!tails) {
        /* A dialect without attribute blocks reads none. */
        markdown_core_inline_skip_spaces(inline_state);
        return markdown_core_inline_skip_line_end(inline_state);
    }
    spnl(inline_state);
    bufsize_t base = (bufsize_t)(markdown_core_inline_window(inline_state, 0).data - tails->p->data);
    bufsize_t start = base + markdown_core_inline_offset(inline_state);
    /* An attribute list looked for reads on to where it ends, or to the
     * text's end. */
    *wide |= markdown_core_inline_offset(inline_state) < markdown_core_inline_size(inline_state) &&
             markdown_core_inline_char_at(inline_state, markdown_core_inline_offset(inline_state)) == '{';
    bufsize_t end = tails->ops->end(tails->self, tails->p, start);
    if (end) {
        markdown_core_inline_seek(inline_state, end - base);
        markdown_core_inline_skip_spaces(inline_state);
        if (markdown_core_inline_skip_line_end(inline_state)) {
            *listed = tails->ops->parse(tails->self, tails->p, start, value, &end);
            return *listed;
        }
    }
    markdown_core_inline_seek(inline_state, before);
    markdown_core_inline_skip_spaces(inline_state);
    return markdown_core_inline_skip_line_end(inline_state);
}

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
} reference_definition;

static bufsize_t S_reference_definition(markdown_core_chunk *input, const S_tails *tails,
                                        reference_definition *definition) {
    markdown_core_inline_state inline_state;
    const markdown_core_chunk absent_title = MARKDOWN_CORE_CHUNK_EMPTY;
    bufsize_t matchlen = 0;
    bufsize_t beforetitle;
    *definition = (reference_definition){0};

    markdown_core_inline_state_from_buf(NULL, &inline_state, input);

    // parse label and colon:
    if (!markdown_core_inline_link_label(&inline_state, &definition->label)) {
        return 0;
    }
    definition->label_end = inline_state.pos - 1;
    if (markdown_core_inline_peek_char(&inline_state) != ':') {
        return 0;
    }
    definition->reached = 1;
    if (definition->label.len == 0) {
        return 0;
    }
    inline_state.pos++;

    // parse link url:
    spnl(&inline_state);
    if ((matchlen =
             markdown_core_inline_manual_scan_link_url(&inline_state.input, inline_state.pos, &definition->url)) > -1) {
        inline_state.pos += matchlen;
        definition->reached = 2;
    } else {
        return 0;
    }

    // parse optional link_title
    beforetitle = inline_state.pos;
    spnl(&inline_state);
    const unsigned char opener = inline_state.pos != beforetitle && inline_state.pos < inline_state.input.len
                                     ? inline_state.input.data[inline_state.pos]
                                     : 0;
    matchlen = inline_state.pos == beforetitle
                   ? 0
                   : scan_link_title(inline_state.input.data, inline_state.input.len, inline_state.pos);
    if (matchlen) {
        definition->title = markdown_core_chunk_dup(&inline_state.input, inline_state.pos, matchlen);
        inline_state.pos += matchlen;
    } else {
        inline_state.pos = beforetitle;
        // No title was written, so record that rather than an empty one.
        definition->title = absent_title;
    }

    // parse final spaces and newline:
    if (!reference_tail(&inline_state, tails, &definition->value, &definition->wide, &definition->listed)) {
        if (matchlen) { // try rewinding before title
            inline_state.pos = beforetitle;
            if (!reference_tail(&inline_state, tails, &definition->value, &definition->wide, &definition->listed)) {
                return 0;
            }
            // The title candidate is un-read here: its bytes stay paragraph
            // text, and the definition has no title. `title` still held the
            // scanned chunk, which then went into the definition -- so a
            // reference to this label resolved with a title the definition does
            // not have, and the same bytes were stated twice, once as prose and
            // once as a title.
            definition->title = absent_title;
        } else {
            return 0;
        }
    }
    definition->wide |= !definition->title.data && (opener == '"' || opener == '\'' || opener == '(');
    return inline_state.pos;
}

bufsize_t markdown_core_reference_definition_length(const markdown_core_element_instance *self,
                                                    markdown_core_parser *parser, markdown_core_chunk *input) {
    const markdown_core_need need = self->needs[LINK_ATTRIBUTES];
    markdown_core_attribute_parser attributes = {0};
    const S_tails tails = {need.ops, need.instance, &attributes};
    if (tails.ops) {
        tails.ops->begin(need.instance, &attributes, input->data, input->len);
    }
    reference_definition definition;
    bufsize_t length = S_reference_definition(input, tails.ops ? &tails : NULL, &definition);
    if (length) {
        markdown_core_attributes_free(&definition.value);
    }
    if (attributes.oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    if (tails.ops) {
        tails.ops->release(need.instance, &attributes);
    }
    return length;
}

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
        tails->ops->fields(tails->self, tails->p, &to);
    }
}

/* THE FRONT CUT'S ITEM (element.h, markdown_core_cut): the link reference
 * definition at the front of `cut->rest`, `cut->at` bytes into the block's
 * text, made a Reference node, which the engine settles before the block;
 * with `measure`, only its length. Its length, 0 when there is none. */
static bufsize_t S_cut_item(const markdown_core_element_instance *self, markdown_core_parser *parser,
                            markdown_core_reader *reader, markdown_core_cut *cut) {
    (void)reader;
    markdown_core_node *b = cut->block;
    markdown_core_chunk *input = &cut->rest;
    const bufsize_t before = cut->at;
    /* A cut's first item begins a text: the recognizer of the last is let
     * go, and the next reads this one. */
    const markdown_core_need need = self->needs[LINK_ATTRIBUTES];
    markdown_core_attribute_parser *held = &((S_state *)self->state)->held;
    const S_tails tails = {need.ops, need.instance, held};
    if (tails.ops && (cut->at == 0 || held->data != cut->text.data || held->length != cut->text.len)) {
        tails.ops->release(need.instance, held);
        tails.ops->begin(need.instance, held, cut->text.data, cut->text.len);
    }
    reference_definition definition; /* S_reference_definition writes it whole. */
    bufsize_t length = S_reference_definition(input, tails.ops ? &tails : NULL, &definition);
    if (tails.ops && held->oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    cut->wide = definition.wide;
    if (cut->measure) {
        if (length) {
            markdown_core_attributes_free(&definition.value);
        }
        return length;
    }
    /* A definition given up keeps what it read on the paragraph. */
    if (!length) {
        S_report_unread(parser, b, input, before, &definition);
        S_report_tail(self, parser, b, input, before, &definition);
        return 0;
    }
    /* The destination and title are cleaned here, the way a direct link's
     * are, so a Reference and a direct link state the same values. */
    int lost = 0;
    markdown_core_chunk clean_url = markdown_core_clean_url(self, &definition.url, &lost);
    markdown_core_optional_chunk clean_title = markdown_core_clean_title(self, &definition.title, &lost);
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
        markdown_core_attributes_free(&definition.value);
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return length;
    }
    reference->as.reference->resource = resource;
    reference->attributes = definition.value;
    reference->flags |= MARKDOWN_CORE_NODE__BLANK_TRANSPARENT;
    /* It spans its definition, through the end of its last line's content:
     * the line ending after it belongs to no block. */
    bufsize_t end = length;
    while (end > 0 && (input->data[end - 1] == '\n' || input->data[end - 1] == '\r')) {
        end--;
    }
    reference->place = (markdown_core_place){b->place.start, b->place.start};
    markdown_core_text_place(parser, &b->content_map, before, before + end, &reference->place);
    S_reference_fields(parser, reference, b, cut->text.data, &definition, &tails);
    const bufsize_t start = (bufsize_t)reference->place.start;
    cut->made = reference;
    /* The document defines the label from where the definition starts. */
    if (!parser->error) {
        markdown_core_parser_declare(parser, self->spaces[LINK_REFERENCE], &definition.label, start,
                                     &reference->as.reference->label, reference, false);
    }
    return length;
}

/* (S_cut_item) A cut its item ends, with none or the last of the text, lets
 * its recognizer go, which adds what it read to the parse's work. */
static bufsize_t S_cut_reference(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                 markdown_core_reader *reader, markdown_core_cut *cut) {
    const bufsize_t rest = cut->rest.len;
    const bufsize_t length = S_cut_item(self, parser, reader, cut);
    if (!length || length == rest) {
        S_release(self);
    }
    return length;
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

/* The link or image whose `[` the opener holds, given up after its `(`
 * with the destination read from `start`, kept on the run's node
 * (markdown_core_inline_open). */
static void S_report_destination(markdown_core_inline_state *inline_state, const bracket *opener, bufsize_t start) {
    const bool image = opener->shape->kind == MARKDOWN_CORE_KIND_EMBEDDED;
    const markdown_core_node_kind kind = image ? MARKDOWN_CORE_KIND_EMBEDDED : MARKDOWN_CORE_KIND_LINK;
    markdown_core_field fields[2];
    const size_t count = S_destination(markdown_core_inline_window(inline_state, 0).data,
                                       markdown_core_inline_size(inline_state), start, kind, fields);
    markdown_core_inline_open(inline_state, kind, opener->position - opener->shape->width, opener->position, fields,
                              count);
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

/* The `{` of the attribute block a definition at the front of `input`
 * opens after its destination or title, or -1: the block is the
 * definition's once it closes its line. */
static bufsize_t S_tail_brace(markdown_core_chunk *input) {
    markdown_core_inline_state inline_state;
    markdown_core_chunk label, url;
    markdown_core_inline_state_from_buf(NULL, &inline_state, input);
    if (!markdown_core_inline_link_label(&inline_state, &label) || !label.len ||
        markdown_core_inline_peek_char(&inline_state) != ':') {
        return -1;
    }
    inline_state.pos++;
    spnl(&inline_state);
    const bufsize_t n = markdown_core_inline_manual_scan_link_url(&inline_state.input, inline_state.pos, &url);
    if (n < 0) {
        return -1;
    }
    const bufsize_t before = inline_state.pos += n;
    spnl(&inline_state);
    if (inline_state.pos > before) {
        inline_state.pos += scan_link_title(inline_state.input.data, inline_state.input.len, inline_state.pos);
        spnl(&inline_state);
        if (markdown_core_inline_peek_char(&inline_state) == '{') {
            return inline_state.pos;
        }
        inline_state.pos = before;
        spnl(&inline_state);
    }
    return markdown_core_inline_peek_char(&inline_state) == '{' ? inline_state.pos : -1;
}

/* The attribute block after a definition's destination or title that does
 * not close, kept on the paragraph `b` (services.h, `open`). */
static void S_report_tail(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_node *b, markdown_core_chunk *input, bufsize_t before,
                          const reference_definition *definition) {
    const markdown_core_need need = self->needs[LINK_ATTRIBUTES];
    const markdown_core_attribute_ops *attributes = need.ops;
    /* Only a definition whose destination was read opens a tail. */
    const bufsize_t brace = attributes && definition->reached == 2 ? S_tail_brace(input) : -1;
    if (brace >= 0) {
        const markdown_core_field_to to = {parser, b, &b->content_map, NULL, before, NULL};
        attributes->open(need.instance, &to, MARKDOWN_CORE_KIND_REFERENCE, input->data, S_content_length(input), brace);
    }
}

/* The link or image whose `[` the opener holds, given up after its `]`
 * with the label of `[text][label` read from the `[` at `start`, which no
 * `]` closes, kept on the run's node (markdown_core_inline_open). */
static void S_report_label(markdown_core_inline_state *inline_state, const bracket *opener, bufsize_t start) {
    const unsigned char *data = markdown_core_inline_window(inline_state, 0).data;
    const bufsize_t len = markdown_core_inline_size(inline_state);
    if (start >= len || data[start] != '[') {
        return;
    }
    const bufsize_t stop = start + 1 + markdown_core_inline_reference_label_length(data + start + 1, len - start - 1);
    if (stop < len && data[stop] == ']') {
        return;
    }
    const bool image = opener->shape->kind == MARKDOWN_CORE_KIND_EMBEDDED;
    const markdown_core_node_kind kind = image ? MARKDOWN_CORE_KIND_EMBEDDED : MARKDOWN_CORE_KIND_LINK;
    const markdown_core_field label = {MARKDOWN_CORE_SLOT_LABEL, kind, start + 1, stop};
    markdown_core_inline_open(inline_state, kind, opener->position - opener->shape->width, opener->position, &label, 1);
}

markdown_core_link_match markdown_core_link_recognize(const markdown_core_element_instance *link,
                                                      markdown_core_inline_state *inline_state, bracket *opener,
                                                      markdown_core_link_candidate *candidate) {
    bufsize_t initial_pos = markdown_core_inline_offset(inline_state), starturl = 0, endurl, starttitle, endtitle,
              endall, n = -1;
    markdown_core_chunk url_chunk = MARKDOWN_CORE_CHUNK_EMPTY, title_chunk, raw_label;
    markdown_core_chunk url = MARKDOWN_CORE_CHUNK_EMPTY;
    markdown_core_optional_chunk title = {MARKDOWN_CORE_CHUNK_EMPTY, false};
    const markdown_core_roster_record *record = NULL;
    int found_label;
    bool explicit_tail = false;
    // If we got here, we matched a potential link/image text.
    // Now we check to see if it's a link/image.
    bool is_image = opener->shape->kind == MARKDOWN_CORE_KIND_EMBEDDED;

    bool link_allowed = is_image || !markdown_core_brackets(inline_state)->no_link_openers;

    bufsize_t after_link_text_pos = markdown_core_inline_offset(inline_state);

    // First, look for an inline link. Its destination, title and closing
    // parenthesis may each follow spaces, tabs and up to one line ending.
    const unsigned char *data = markdown_core_inline_window(inline_state, 0).data;
    const bufsize_t len = markdown_core_inline_size(inline_state);
    /* A destination, title or parenthesis the end cuts short may be one
     * once the input goes on. */
    if (link_allowed && markdown_core_inline_peek_char(inline_state) == '(') {
        starturl = markdown_core_skip_spaces_and_line_end(data, markdown_core_inline_offset(inline_state) + 1, len);
        markdown_core_chunk window = markdown_core_inline_window(inline_state, 0);
        n = markdown_core_inline_manual_scan_link_url(&window, starturl, &url_chunk);
        if (n < 0) {
            markdown_core_inline_saw_end(inline_state);
            S_report_destination(inline_state, opener, starturl);
        }
    }
    if (n > -1) {

        // try to parse an explicit link:
        endurl = starturl + n;
        starttitle = markdown_core_skip_spaces_and_line_end(data, endurl, len);

        // A title must be separated from the destination; without one, the
        // separator already read is the one before the parenthesis.
        endtitle = starttitle == endurl ? starttitle : starttitle + scan_link_title(data, len, starttitle);
        endall = endtitle == starttitle ? starttitle : markdown_core_skip_spaces_and_line_end(data, endtitle, len);
        if (markdown_core_inline_tracks_end(inline_state) &&
            (endall >= len ||
             (endtitle == starttitle && starttitle < len && data[starttitle] && strchr("\"'(", data[starttitle])))) {
            markdown_core_inline_saw_end(inline_state);
        }

        if (markdown_core_inline_peek_at(inline_state, endall) == ')') {
            explicit_tail = true;
            markdown_core_inline_seek(inline_state, endall + 1);

            const unsigned char *url_chunk_start = url_chunk.data;
            const bufsize_t url_length = url_chunk.len;
            title_chunk = markdown_core_inline_take(inline_state, starttitle, endtitle - starttitle);
            {
                int lost = 0;
                url = markdown_core_clean_url(link, &url_chunk, &lost);
                title = markdown_core_clean_title(link, &title_chunk, &lost);
                if (lost) {
                    inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
                }
            }
            markdown_core_chunk_free(&url_chunk);
            markdown_core_chunk_free(&title_chunk);
            candidate->url = url;
            candidate->title = title;
            candidate->explicit_tail = true;
            candidate->dest = (bufsize_t)(url_chunk_start - data);
            candidate->dest_end = candidate->dest + url_length;
            candidate->angled = starturl < len && data[starturl] == '<';
            if (endtitle > starttitle) {
                candidate->title_start = starttitle + 1;
                candidate->title_end = endtitle - 1;
            }
            return LINK_EXPLICIT;

        } else {
            // it could still be a shortcut reference link
            S_report_destination(inline_state, opener, starturl);
            markdown_core_inline_seek(inline_state, after_link_text_pos);
        }
    }

    // Next, look for a following [link label] that a definition declares.
    // skip spaces
    raw_label = markdown_core_chunk_literal("");
    const bufsize_t label_open = markdown_core_inline_offset(inline_state);
    found_label = markdown_core_inline_link_label(inline_state, &raw_label);
    if (found_label) {
        candidate->label_start = label_open + 1;
        candidate->label_end = markdown_core_inline_offset(inline_state) - 1;
    }
    if (!found_label) {
        S_report_label(inline_state, opener, markdown_core_inline_offset(inline_state));
    }
    explicit_tail = found_label;
    if (!found_label) {
        // If we have a shortcut reference link, back up
        // to before the spacse we skipped.
        markdown_core_inline_seek(inline_state, initial_pos);
    }

    if ((!found_label || raw_label.len == 0) && !opener->bracket_after) {
        markdown_core_chunk_free(&raw_label);
        raw_label = markdown_core_inline_take(inline_state, opener->position, initial_pos - opener->position - 1);
        found_label = true;
    }

    /* `[t][l]`, `[l][]` and `[l]` resolve identically and to the same node: the
     * `Link` or `Embedded` the definition names (M2). Nothing records which of the
     * three spellings the author wrote, and nothing downstream can recover it
     * -- the module states one node for every successful form. */
    if (link_allowed && found_label) {
        bool failed = false;
        record = markdown_core_parser_find_definition(
            inline_state->owner_parser, link->spaces[LINK_REFERENCE], &raw_label,
            markdown_core_inline_state_root_offset(inline_state, opener->position), &failed);
        if (failed) {
            inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        }
    }
    markdown_core_chunk_free(&raw_label);
    candidate->label = record ? markdown_core_roster_label_of(record) : markdown_core_chunk_literal("");
    candidate->explicit_tail = explicit_tail;
    return record ? (explicit_tail ? LINK_EXPLICIT : LINK_SHORTCUT) : LINK_UNMATCHED;
}

/* THE FIELDS OF A LINK'S TAIL, as recognize read them (node.h,
 * markdown_core_fields): an inline tail's destination, its path and, after
 * its first `#` that starts a fragment, as field_end reads it, its anchor,
 * and its title inside its quotes; a full reference's label. */
static void S_tail_fields(markdown_core_inline_state *inline_state, markdown_core_node *inl,
                          const markdown_core_link_candidate *candidate) {
    const markdown_core_field_to to = markdown_core_inline_fields(inline_state, inl);
    markdown_core_field_spot spots[4];
    size_t count = 0;
    if (inl->as.link->resource) {
        const unsigned char *data = markdown_core_inline_window(inline_state, 0).data;
        const bufsize_t start = candidate->dest, end = candidate->dest_end > start ? candidate->dest_end : start;
        /* A destination with no `#` is all path, and one with no path,
         * `()` or `(#t)`, holds an empty one, as one with an empty fragment
         * holds an empty anchor. */
        const bufsize_t hash = memchr(data + start, '#', (size_t)(end - start))
                                   ? start + field_end(data + start, end - start, candidate->angled, false)
                                   : end;
        spots[count++] = (markdown_core_field_spot){start, hash < end ? hash : end, MARKDOWN_CORE_SLOT_DEST, true,
                                                    MARKDOWN_CORE_FIELD};
        if (hash < end) {
            spots[count++] =
                (markdown_core_field_spot){hash + 1, end, MARKDOWN_CORE_SLOT_ANCHOR, true, MARKDOWN_CORE_FIELD};
        }
    }
    spots[count++] = (markdown_core_field_spot){candidate->title_start, candidate->title_end, MARKDOWN_CORE_SLOT_NONE,
                                                false, MARKDOWN_CORE_FIELD};
    if (!inl->as.link->resource) {
        spots[count++] = (markdown_core_field_spot){candidate->label_start, candidate->label_end,
                                                    MARKDOWN_CORE_SLOT_LABEL, false, MARKDOWN_CORE_FIELD};
    }
    markdown_core_field_many(&to, spots, count);
}

bool markdown_core_link_commit(const markdown_core_element_instance *link, markdown_core_parser *parser,
                               markdown_core_inline_state *inline_state, bracket *opener,
                               markdown_core_link_candidate *candidate, bufsize_t initial_pos) {
    bool is_image = opener->shape->kind == MARKDOWN_CORE_KIND_EMBEDDED;
    bool explicit_tail = candidate->explicit_tail;
    bool reference = candidate->label.len != 0;
    markdown_core_chunk url = candidate->url;
    markdown_core_optional_chunk title = candidate->title;
    markdown_core_node *inl;
    markdown_core_inline_settle_bracket(inline_state, opener);
    if (!markdown_core_node_can_contain_type(markdown_core_scaffold_parent(opener->inl_text),
                                             is_image ? MARKDOWN_CORE_NODE_EMBEDDED : MARKDOWN_CORE_NODE_LINK)) {
        markdown_core_chunk_free(&url);
        markdown_core_optional_chunk_free(&title);
        return false;
    }
    inl = markdown_core_inline_make_simple(inline_state,
                                           is_image ? MARKDOWN_CORE_NODE_EMBEDDED : MARKDOWN_CORE_NODE_LINK);
    if (inl && reference) {
        /* A RESOLVED REFERENCE NAMES ITS DEFINITION BY LABEL: the Link or
         * Embedded holds its own copy of the normalized label, and its
         * destination and title are those of the node the document finds by
         * that label. The occurrence keeps its own scope and attributes. */
        const bufsize_t length = candidate->label.len;
        unsigned char *label = markdown_core_node_pool_bytes(parser->pool, (size_t)length + 1);
        if (label) {
            memcpy(label, candidate->label.data, (size_t)length);
            label[length] = '\0';
            inl->as.link->label = (markdown_core_chunk){label, length, 0};
        } else {
            markdown_core_parser_release_node(parser, inl);
            inl = NULL;
        }
    } else if (inl) {
        inl->as.link->resource = markdown_core_resource_new(parser->pool, url, title);
        if (!inl->as.link->resource) {
            markdown_core_parser_release_node(parser, inl);
            inl = NULL;
        }
    }
    if (!inl) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        if (!reference) {
            markdown_core_chunk_free(&url);
            markdown_core_optional_chunk_free(&title);
        }
        return false;
    }
    /* REQUIREMENT 11b: the brackets, and whatever follows the closing one --
     * `(...)` with the destination and title, or `[label]` -- are the link's
     * markers. They were claimed CONTENT as they were read, because an
     * unmatched `[` is its own literal; these claims are later and win. The
     * children keep the claims they made for themselves. */
    // A link starts at its own '[' and ends at its closing ')' or ']', and the
    // two need not be on the same line. Taking BOTH from inline_state->line made a link
    // start where it ENDED: `[a\nb](/u)` reported Link 2:1..2:6 around a child
    // Text at 1:2 -- a node that begins after its own first child.
    markdown_core_inline_state_place(inline_state, inl, opener->position - 1,
                                     markdown_core_inline_offset(inline_state) - 1);
    inl->place.start = opener->inl_text->place.start;
    S_tail_fields(inline_state, inl, candidate);
    /* What follows a link whose destination, title or label follows its `]`
     * may update it (no-flush.md 10.13). */
    if (explicit_tail) {
        markdown_core_inline_made(inline_state, inl);
    }
    // And the destination and title are scanned by markdown_core_inline_manual_scan_link_url and
    // scan_link_title, which move markdown_core_inline_offset(inline_state) without ever passing through
    // handle_newline -- so a line ending inside `(...)` is invisible to the
    // inline state, and every later node in the paragraph inherits the error.
    // The extent is projected from the two offsets, which is the repair inline
    // code and raw HTML already
    // use; it walks only [initial_pos, markdown_core_inline_offset(inline_state)), which is what the bracket
    // handler consumed for itself. Counting from the OPENING bracket instead
    // would count the label's own newlines a second time -- measured,
    // `[a\nb](/u) tail` then reports line 3 of a two-line document.
    markdown_core_scaffold_insert_before(opener->inl_text, inl);
    markdown_core_inline_take_bracket_content(parser, inline_state, opener, inl);

    /* Only the embedded element opens an image bracket. */
    if (is_image) {
        const markdown_core_need dimensions = link->needs[LINK_DIMENSIONS];
        if (dimensions.ops) {
            ((const markdown_core_dimension_ops *)dimensions.ops)
                ->apply(dimensions.instance, inline_state, opener, inl, initial_pos - 1);
        }
    }

    // Free the bracket [:
    markdown_core_parser_release_node(parser, opener->inl_text);

    markdown_core_inline_process_delimiters(parser, inline_state, opener->position, opener->delim_end);
    markdown_core_inline_pop_bracket(inline_state);

    // Now, if we have a link, we also want to deactivate links until
    // we get a new opener. (This code can be removed if we decide to allow links
    // inside links.)
    if (!is_image) {
        markdown_core_brackets(inline_state)->no_link_openers = true;
    }

    return true;
}

/* WHAT A BRACKET HOLDS OF A LINK (the link element's bracket record,
 * bracket.h): what its `]` read after it, which the explicit rule reads
 * and the shortcut rule commits. */
typedef struct {
    markdown_core_link_match match;
    markdown_core_link_candidate candidate;
} S_tail;

static S_tail *S_bracket(const markdown_core_element_instance *self, const bracket *b) {
    return markdown_core_bracket_record(b, self);
}

/* THE LINK'S RULES FOR A `]` (element.h, markdown_core_close_rule). One
 * bracket owns its parsed children; alternatives only claim that existing
 * range, in syntax precedence order, and none reparses its body. A link
 * whose destination, title or label follows the `]` is made first; a
 * shortcut, whose `]` is all of its tail, after the other rules. */
static markdown_core_bracket_match S_close_explicit(const markdown_core_element_instance *self,
                                                    markdown_core_parser *parser,
                                                    markdown_core_inline_state *inline_state, bracket *opener,
                                                    markdown_core_node **made) {
    (void)made;
    S_tail *tail = S_bracket(self, opener);
    const bufsize_t after = markdown_core_inline_offset(inline_state);
    tail->candidate = (markdown_core_link_candidate){0};
    tail->match = markdown_core_link_recognize(self, inline_state, opener, &tail->candidate);
    if (tail->match != LINK_EXPLICIT) {
        return BRACKET_UNMATCHED;
    }
    return markdown_core_link_commit(self, parser, inline_state, opener, &tail->candidate, after) ? BRACKET_MATCHED
                                                                                                  : BRACKET_REJECTED;
}

static markdown_core_bracket_match S_close_shortcut(const markdown_core_element_instance *self,
                                                    markdown_core_parser *parser,
                                                    markdown_core_inline_state *inline_state, bracket *opener,
                                                    markdown_core_node **made) {
    (void)made;
    S_tail *tail = S_bracket(self, opener);
    if (tail->match == LINK_UNMATCHED && tail->candidate.label_end > tail->candidate.label_start) {
        /* A full reference, `[t][label]`, whose label names no definition,
         * which a cursor in it may name (markdown_core_inline_open). */
        const markdown_core_node_kind kind =
            opener->shape->kind == MARKDOWN_CORE_KIND_EMBEDDED ? MARKDOWN_CORE_KIND_EMBEDDED : MARKDOWN_CORE_KIND_LINK;
        const markdown_core_field label = {MARKDOWN_CORE_SLOT_LABEL, kind, tail->candidate.label_start,
                                           tail->candidate.label_end};
        markdown_core_inline_open(inline_state, kind, opener->position - opener->shape->width, opener->position, &label,
                                  1);
    }
    if (tail->match != LINK_SHORTCUT) {
        return BRACKET_UNMATCHED;
    }
    return markdown_core_link_commit(self, parser, inline_state, opener, &tail->candidate,
                                     markdown_core_inline_offset(inline_state))
               ? BRACKET_MATCHED
               : BRACKET_REJECTED;
}

static const markdown_core_close_rule LINK_CLOSES[] = {
    {.precedence = -20, .close = S_close_explicit},
    {.precedence = 10, .close = S_close_shortcut},
};

static markdown_core_node *match_bracket(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                         markdown_core_node *parent, unsigned char character,
                                         markdown_core_inline_state *inline_state) {
    (void)parser;
    if (character == '[') {
        advance(inline_state);
        markdown_core_node *text = make_str(
            inline_state, markdown_core_inline_offset(inline_state) - 1, markdown_core_inline_offset(inline_state) - 1,
            markdown_core_inline_take(inline_state, markdown_core_inline_offset(inline_state) - 1, 1));
        if (text) {
            markdown_core_inline_push_bracket(self, inline_state, &MARKDOWN_CORE_LINK_BRACKET, text);
        }
        return text;
    }
    return NULL;
}
/* The brackets no closer met, each a construct given up, kept on the run's
 * node (markdown_core_inline_open): a `[` whose construct is still open, an
 * image's `![` and an inline note's `^[`. A `[` reads a label to where its
 * reading stops, when nothing it may not hold stops it: a `[^`'s a footnote
 * call's, and at the start of a line of its content a definition's, as any
 * other `[` there reads a reference definition's. */
static void report_open(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state) {
    (void)self;
    const unsigned char *data = markdown_core_inline_window(inline_state, 0).data;
    const bufsize_t len = markdown_core_inline_size(inline_state);
    for (const bracket *b = markdown_core_open_bracket(inline_state); b; b = bracket_previous(b)) {
        const bool own = b->shape == &MARKDOWN_CORE_LINK_BRACKET;
        const bufsize_t start = b->position - b->shape->width;
        markdown_core_field fields[2];
        size_t count = 0;
        markdown_core_node_kind opens = own ? MARKDOWN_CORE_KIND_NONE : b->shape->kind;
        const bufsize_t stop =
            b->position + markdown_core_inline_reference_label_length(data + b->position, len - b->position);
        const bool label = own && stop == len;
        const bool caret = own && b->position < len && data[b->position] == '^';
        const bool line = start == 0 || data[start - 1] == '\n' || data[start - 1] == '\r';
        if (caret && label) {
            opens = MARKDOWN_CORE_KIND_CITE;
            fields[count++] =
                (markdown_core_field){MARKDOWN_CORE_SLOT_FOOTNOTE, MARKDOWN_CORE_KIND_CITATION, b->position + 1, stop};
        }
        if (line && caret) {
            /* A footnote definition's label holds any byte but `]` and white
             * space, brackets too, which no other label does. */
            bufsize_t note = b->position + 1;
            while (note < len && data[note] != ']' && !markdown_core_is_whitespace(data[note])) {
                note++;
            }
            fields[count++] =
                (markdown_core_field){MARKDOWN_CORE_SLOT_FOOTNOTE, MARKDOWN_CORE_KIND_FOOTNOTE, b->position + 1, note};
        } else if (line && label) {
            fields[count++] =
                (markdown_core_field){MARKDOWN_CORE_SLOT_LABEL, MARKDOWN_CORE_KIND_REFERENCE, b->position, stop};
        }
        markdown_core_inline_open(inline_state, opens, start, caret && label ? b->position + 1 : b->position, fields,
                                  count);
    }
}

static const markdown_core_inline_ops LINK_INLINES = {
    .report = report_open,
    .precedence = MARKDOWN_CORE_INLINE_FALLBACK,
    .match = match_bracket,
    .closes = LINK_CLOSES,
    .close_count = sizeof(LINK_CLOSES) / sizeof(LINK_CLOSES[0]),
    .closer = ']',
    .terminates = "[",
    .dispatch = "[",
};

/* A paragraph's text opens with its link reference definitions, which the
 * engine cuts off its front (blocks.c, THE FRONT CUT). */
static const markdown_core_open_ops LINK_OPENS = {
    .cut = S_cut_reference,
    .cut_needs = {.bytes = "["},
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
    .dispose_parser = dispose_parser,
    .bracket_size = sizeof(S_tail),
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
