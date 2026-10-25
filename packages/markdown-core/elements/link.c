#include "alloc.h"
#include "link_scanners.h"
#include "citation.h"
#include "footnote.h"
#include "span.h"
#define advance(inline_state) ((inline_state)->pos += 1)
#include "attributes.h"
#include "citation.h"
#include "link.h"
#include "embedded.h"
#include "inline_internal.h"
#include "block_internal.h"

/* The elements whose state this element reads, as `self->peers` holds them. */
enum { LINK_EMBEDDED, LINK_FOOTNOTE, LINK_CITATION };
static const markdown_core_element *const LINK_PEERS[] = {[LINK_EMBEDDED] = &MARKDOWN_CORE_ELEMENT_EMBEDDED,
                                                          [LINK_FOOTNOTE] = &MARKDOWN_CORE_ELEMENT_FOOTNOTE,
                                                          [LINK_CITATION] = &MARKDOWN_CORE_ELEMENT_CITATION,
                                                          NULL};

static bufsize_t markdown_core_inline_manual_scan_link_url(markdown_core_chunk *input, bufsize_t offset,
                                                           markdown_core_chunk *output);
/* Reads the link reference definition at the front of `input`, its bytes
 * `before` past the front of `b`'s content, into a Reference node settled
 * into `parent`, the block that holds `b`, which starts on input line
 * `*line`, an earlier one or a later one, and leaves `*line` the line it ends
 * on. Its length, 0 when there is none. */
static bufsize_t S_read_reference(markdown_core_parser *parser, markdown_core_node *b, markdown_core_node *parent,
                                  markdown_core_chunk *input, markdown_core_attribute_parser *attributes,
                                  bufsize_t before, int *line);

bool markdown_core_block_resolve_reference_link_definitions(markdown_core_parser *parser, markdown_core_node *b,
                                                            markdown_core_node *parent) {
    bufsize_t pos;
    const markdown_core_chunk text = markdown_core_node_text(b);
    markdown_core_chunk chunk = text;
    markdown_core_attribute_parser attributes = {
        .data = chunk.data, .length = chunk.len, .scratch = &parser->attribute_scratch};
    /* The definitions follow each other down the block's lines. */
    int line = parser->at.line_number;
    while (chunk.len && chunk.data[0] == '[' && !parser->error) {
        pos = S_read_reference(parser, b, parent, &chunk, &attributes, (bufsize_t)(chunk.data - text.data), &line);
        if (!pos) {
            break;
        }
        chunk.data += pos;
        chunk.len -= pos;
    }
    if (attributes.oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    markdown_core_attribute_parser_free(&attributes);
    /* The definitions read are cut from the front of the block's text, and
     * its map follows the cut. */
    bufsize_t read = text.len - chunk.len;
    if (read) {
        markdown_core_block_rebase_content_marks(parser, b, read, chunk.len);
        b->cut_front += read;
    }
    if (!b->cut_front) {
        return !markdown_core_is_blank_to_line_end(text.data, 0, text.len);
    }
    int ignored;
    bufsize_t source;
    /* The block now begins where its FIRST SURVIVING line was written, and
     * that is asked of the map rather than derived: this function can be
     * reached twice on one paragraph -- once at the setext-underline check and
     * again as the block closes -- and the first call can consume everything
     * recorded so far, leaving the line that carries what is left still
     * unread. Taking the answer from the surviving run rather than from the
     * size of the cut is what makes both arrivals give the same result. */
    if (markdown_core_parser_content_place(parser, &b->content_map, 0, &ignored, &source)) {
        b->place.start = (uint32_t)source;
    }
    return !markdown_core_is_blank_to_line_end(chunk.data, 0, chunk.len);
}

/* A destination is its source less the angle brackets, with escapes and
 * references decoded. Nothing is trimmed: an unbracketed destination cannot
 * hold a space, tab or line ending, and CommonMark keeps the spaces a
 * bracketed one encloses (`<  u >` is `  u `). */
markdown_core_chunk markdown_core_clean_url(markdown_core_chunk *url, int *lost) {
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT();

    if (url->len == 0) {
        return markdown_core_chunk_literal("");
    }

    houdini_unescape_html_f(&buf, url->data, url->len);

    markdown_core_strbuf_unescape(&buf);
    if (buf.oom && lost) {
        *lost = 1;
    }
    return markdown_core_chunk_buf_detach(&buf);
}

markdown_core_optional_chunk markdown_core_clean_title(markdown_core_chunk *title, int *lost) {
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT();
    unsigned char first, last;

    if (title->len == 0) {
        return markdown_core_optional_chunk_absent();
    }

    first = title->data[0];
    last = title->data[title->len - 1];

    // remove surrounding quotes if any:
    if ((first == '\'' && last == '\'') || (first == '(' && last == ')') || (first == '"' && last == '"')) {
        houdini_unescape_html_f(&buf, title->data + 1, title->len - 2);
    } else {
        houdini_unescape_html_f(&buf, title->data, title->len);
    }

    markdown_core_strbuf_unescape(&buf);
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
    bufsize_t startpos = inline_state->pos;
    int length = 0;
    unsigned char c;

    // advance past [
    if (markdown_core_inline_peek_char(inline_state) == '[') {
        inline_state->pos++;
    } else {
        return 0;
    }

    length = markdown_core_inline_reference_label_length(inline_state->input.data + inline_state->pos,
                                                         inline_state->input.len - inline_state->pos);
    inline_state->pos += length;
    if (length > MAX_LINK_LABEL_LENGTH) {
        goto noMatch;
    }
    c = markdown_core_inline_peek_char(inline_state);

    if (c == ']') { // match found
        *raw_label = markdown_core_chunk_dup(&inline_state->input, startpos + 1, inline_state->pos - (startpos + 1));
        markdown_core_chunk_trim(raw_label);
        inline_state->pos++; // advance past ]
        return 1;
    }

noMatch:
    inline_state->pos = startpos; // rewind
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
    inline_state->pos =
        markdown_core_skip_spaces_and_line_end(inline_state->input.data, inline_state->pos, inline_state->input.len);
}

static bool reference_tail(markdown_core_inline_state *inline_state, markdown_core_attribute_parser *attributes,
                           markdown_core_attributes *value) {
    bufsize_t before = inline_state->pos;
    spnl(inline_state);
    bufsize_t base = (bufsize_t)(inline_state->input.data - attributes->data);
    bufsize_t start = base + inline_state->pos;
    bufsize_t end = markdown_core_attributes_end(attributes, start);
    if (end) {
        inline_state->pos = end - base;
        markdown_core_inline_skip_spaces(inline_state);
        if (markdown_core_inline_skip_line_end(inline_state)) {
            return markdown_core_attributes_parse(attributes, start, value, &end) != 0;
        }
    }
    inline_state->pos = before;
    markdown_core_inline_skip_spaces(inline_state);
    return markdown_core_inline_skip_line_end(inline_state);
}

/* A link reference definition at the front of `input`: its length, 0 when
 * there is none, and its parts, the attributes `value` holds when there is
 * one. */
typedef struct {
    markdown_core_chunk label, url, title;
    markdown_core_attributes value;
} reference_definition;

static bufsize_t S_reference_definition(markdown_core_chunk *input, markdown_core_attribute_parser *attributes,
                                        reference_definition *definition) {
    markdown_core_inline_state inline_state;
    const markdown_core_chunk absent_title = MARKDOWN_CORE_CHUNK_EMPTY;
    bufsize_t matchlen = 0;
    bufsize_t beforetitle;
    *definition = (reference_definition){0};

    markdown_core_inline_state_from_buf(NULL, &inline_state, input);

    // parse label:
    if (!markdown_core_inline_link_label(&inline_state, &definition->label) || definition->label.len == 0) {
        return 0;
    }
    // colon:
    if (markdown_core_inline_peek_char(&inline_state) == ':') {
        inline_state.pos++;
    } else {
        return 0;
    }

    // parse link url:
    spnl(&inline_state);
    if ((matchlen =
             markdown_core_inline_manual_scan_link_url(&inline_state.input, inline_state.pos, &definition->url)) > -1) {
        inline_state.pos += matchlen;
    } else {
        return 0;
    }

    // parse optional link_title
    beforetitle = inline_state.pos;
    spnl(&inline_state);
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
    if (!reference_tail(&inline_state, attributes, &definition->value)) {
        if (matchlen) { // try rewinding before title
            inline_state.pos = beforetitle;
            if (!reference_tail(&inline_state, attributes, &definition->value)) {
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
    return inline_state.pos;
}

bufsize_t markdown_core_reference_definition_length(markdown_core_chunk *input,
                                                    markdown_core_attribute_parser *attributes) {
    reference_definition definition;
    bufsize_t length = S_reference_definition(input, attributes, &definition);
    if (length) {
        markdown_core_attributes_free(&definition.value);
    }
    return length;
}

static bool S_report_reference(markdown_core_parser *parser, markdown_core_node *b, const markdown_core_chunk *input,
                               bufsize_t before, const markdown_core_chunk *url);
static void S_report_unread(markdown_core_parser *parser, markdown_core_node *b, const markdown_core_chunk *input,
                            bufsize_t before);
static void S_report_tail(markdown_core_parser *parser, markdown_core_node *b, markdown_core_chunk *input,
                          bufsize_t before);

static bufsize_t S_read_reference(markdown_core_parser *parser, markdown_core_node *b, markdown_core_node *parent,
                                  markdown_core_chunk *input, markdown_core_attribute_parser *attributes,
                                  bufsize_t before, int *line) {
    reference_definition definition;
    bufsize_t length = S_reference_definition(input, attributes, &definition);
    if (!length) {
        if (parser->sink) {
            S_report_unread(parser, b, input, before);
            S_report_tail(parser, b, input, before);
        }
        return 0;
    }
    /* A definition whose destination the content's end cuts short may yet
     * be text: the run reads it too. */
    if (parser->sink) {
        S_report_tail(parser, b, input, before);
        if (S_report_reference(parser, b, input, before, &definition.url)) {
            markdown_core_attributes_free(&definition.value);
            return 0;
        }
    }
    /* The destination and title are cleaned here, the way a direct link's
     * are, so a Reference and a direct link state the same values. */
    int lost = 0;
    markdown_core_chunk clean_url = markdown_core_clean_url(&definition.url, &lost);
    markdown_core_optional_chunk clean_title = markdown_core_clean_title(&definition.title, &lost);
    markdown_core_chunk label;
    if (!markdown_core_registry_normal_label(&parser->registry, parser->pool, &definition.label, &label)) {
        lost = 1;
    }
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
        if (label.len) {
            markdown_core_node_pool_bytes_free(parser->pool, label.data);
        }
        markdown_core_attributes_free(&definition.value);
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return length;
    }
    reference->as.reference->resource = resource;
    reference->as.reference->label = label;
    reference->attributes = definition.value;
    reference->flags |= MARKDOWN_CORE_NODE__BLANK_TRANSPARENT;
    /* It spans its definition, through the end of its last line's content:
     * the line ending after it belongs to no block. */
    bufsize_t end = length;
    while (end > 0 && (input->data[end - 1] == '\n' || input->data[end - 1] == '\r')) {
        end--;
    }
    int ignored;
    bufsize_t start = b->place.start, stop = b->place.start;
    markdown_core_parser_content_place(parser, &b->content_map, before, &ignored, &start);
    markdown_core_parser_content_end_place(parser, &b->content_map, before + end - 1, &ignored, &stop);
    reference->place = (markdown_core_place){(uint32_t)start, (uint32_t)stop};
    markdown_core_parser_line_places(parser, reference, parent, line);
    /* A definition is read whole: it settles as it is made, before the block
     * it was read from, which settles later if anything of it is left. */
    markdown_core_block_settle_before(parser, reference, parent);
    /* The document defines the label from where the definition starts. */
    if (!parser->error && !markdown_core_registry_declare(&parser->registry, MARKDOWN_CORE_REGISTRY_REFERENCE,
                                                          label.data, label.len, start)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
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
static bufsize_t path_end(const unsigned char *data, bufsize_t length) { return field_end(data, length, false, false); }
/* A fragment's end is read from its path's start, through the `(` before
 * its `#` that a `)` after it closes. */
static bufsize_t fragment_end(const unsigned char *data, bufsize_t length) {
    return field_end(data, length, false, true);
}
static bufsize_t angled_path_end(const unsigned char *data, bufsize_t length) {
    return field_end(data, length, true, false);
}
static bufsize_t angled_fragment_end(const unsigned char *data, bufsize_t length) {
    return field_end(data, length, true, true);
}
/* A destination not begun before the cursor, read from its first byte: one
 * in angle brackets takes them, to its `>`, fragment and all. */
static bufsize_t unbegun_end(const unsigned char *data, bufsize_t length) {
    if (!length || data[0] != '<') {
        return path_end(data, length);
    }
    const bufsize_t end = 1 + field_end(data + 1, length - 1, true, true);
    return end < length && data[end] == '>' ? end + 1 : end;
}
static bufsize_t label_end(const unsigned char *data, bufsize_t length) {
    return markdown_core_inline_reference_label_length(data, length);
}

/* Where a footnote definition's label goes on past a cursor: to a `]`, a
 * space, a tab or the line's end. */
static bufsize_t note_end(const unsigned char *data, bufsize_t length) {
    bufsize_t end = 0;
    while (end < length && data[end] != ']' && !markdown_core_is_whitespace(data[end]) && data[end]) {
        end++;
    }
    return end;
}

/* Whether the `#` at `hash` of a destination from `path`, cut short at
 * `end`, may be a numeric character reference's that the text after the
 * cursor finishes: whether it starts a fragment only that text tells. */
static bool S_reference_cut(const unsigned char *data, bufsize_t path, bufsize_t hash, bufsize_t end) {
    /* References are decoded before escapes: an escaped `&` begins one. */
    return hash < end && hash > path && data[hash - 1] == '&' && numeric_reference(data, hash, end) < 0;
}

/* The destination the content's end cuts short, read from `start`, to the
 * run's context sink, as the field of a link or an image whose `[` the
 * opener holds: the fragment after its first `#`, with the path before it,
 * or else the path. A destination that ended before the content's end is
 * not open. */
static void S_report_destination(markdown_core_inline_state *inline_state, const bracket *opener, bufsize_t start,
                                 markdown_core_slot_kind *reported) {
    const unsigned char *data = inline_state->input.data;
    const bufsize_t len = inline_state->input.len;
    const bool angled = start < len && data[start] == '<';
    const bufsize_t path = angled ? start + 1 : start;
    const bufsize_t end = path + field_end(data + path, len - path, angled, true);
    if (end < len || (!angled && end == start && start < len)) {
        return;
    }
    const bufsize_t hash = path + field_end(data + path, end - path, angled, false);
    if (S_reference_cut(data, path, hash, end)) {
        return;
    }
    const bool image = opener->kind == BRACKET_IMAGE;
    const markdown_core_node_kind kind = image ? MARKDOWN_CORE_KIND_EMBEDDED : MARKDOWN_CORE_KIND_LINK;
    const bufsize_t at = opener->position - (image ? 2 : 1);
    if (hash < end) {
        const markdown_core_field fragment = {MARKDOWN_CORE_SLOT_ANCHOR,
                                              kind,
                                              hash + 1,
                                              angled ? angled_fragment_end : fragment_end,
                                              {{path, hash}},
                                              1,
                                              false,
                                              hash + 1 - path};
        markdown_core_inline_open(inline_state, kind, at, opener->position, &fragment, 1);
        *reported = MARKDOWN_CORE_SLOT_ANCHOR;
    } else {
        const markdown_core_field_end read = angled ? angled_path_end : path == len ? unbegun_end : path_end;
        const markdown_core_field destination = {MARKDOWN_CORE_SLOT_DEST, kind, path, read, {{0}}, 0, false, 0};
        markdown_core_inline_open(inline_state, kind, at, opener->position, &destination, 1);
        *reported = MARKDOWN_CORE_SLOT_DEST;
    }
}

/* The destination of the definition at the front of `input`, content
 * offset `before` of the paragraph `b`, from `path` to the content's end at
 * `len`, which cuts it short, to the parse's context sink: its fragment
 * after a `#`, with the path before it, or else its path. */
static void S_report_destination_field(markdown_core_parser *parser, markdown_core_node *b,
                                       const markdown_core_chunk *input, bufsize_t before, bufsize_t path,
                                       bufsize_t len, bool angled) {
    const bufsize_t hash = path + field_end(input->data + path, len - path, angled, false);
    if (S_reference_cut(input->data, path, hash, len)) {
        return;
    }
    int ignored;
    uint32_t place[4];
    const bufsize_t at[4] = {0, path, hash, hash + 1};
    for (size_t i = 0; i < 4; i++) {
        bufsize_t source = 0;
        markdown_core_parser_content_place(parser, &b->content_map, before + at[i], &ignored, &source);
        place[i] = (uint32_t)source;
    }
    const int32_t opener = markdown_core_context_opener_add(parser->sink, MARKDOWN_CORE_KIND_REFERENCE,
                                                            (markdown_core_place){place[0], place[0] + 1}, true);
    if (opener < 0) {
        return;
    }
    markdown_core_context_field field = {.kind = MARKDOWN_CORE_SLOT_DEST,
                                         .owner = MARKDOWN_CORE_KIND_REFERENCE,
                                         .opener = opener,
                                         .start = place[1],
                                         .end = angled        ? angled_path_end
                                                : path == len ? unbegun_end
                                                              : path_end};
    if (hash < len) {
        field.kind = MARKDOWN_CORE_SLOT_ANCHOR;
        field.start = place[3];
        field.end = angled ? angled_fragment_end : fragment_end;
        field.prefix[0] = (markdown_core_place){place[1], place[2]};
        field.prefix_count = 1;
        field.lead = place[3] - place[1];
    }
    markdown_core_context_field_add(parser->sink, &field);
}

/* The content of `input` without the spaces and line endings at its end. */
static bufsize_t S_content_length(const markdown_core_chunk *input) {
    bufsize_t len = input->len;
    while (len > 0 && markdown_core_is_whitespace(input->data[len - 1])) {
        len--;
    }
    return len;
}

/* The bare destination `url` of the definition read at the front of
 * `input`, when the content's end cuts it short, to the parse's context
 * sink; whether it does. A destination in angle brackets has closed. */
static bool S_report_reference(markdown_core_parser *parser, markdown_core_node *b, const markdown_core_chunk *input,
                               bufsize_t before, const markdown_core_chunk *url) {
    const bufsize_t len = S_content_length(input);
    if (!url->len || url->data < input->data || url->data + url->len != input->data + len) {
        return false;
    }
    S_report_destination_field(parser, b, input, before, (bufsize_t)(url->data - input->data), len, false);
    return true;
}

/* A definition at the front of `input` that no definition reads yet, to
 * the parse's context sink: one whose destination the content's end has
 * not begun, `[label]: `, or whose destination in angle brackets it cuts
 * short, `[label]: <pa`, which no definition reads until its `>`. */
static void S_report_unread(markdown_core_parser *parser, markdown_core_node *b, const markdown_core_chunk *input,
                            bufsize_t before) {
    const unsigned char *data = input->data;
    const bufsize_t len = S_content_length(input);
    if (len < 2 || data[0] != '[') {
        return;
    }
    bufsize_t at = 1 + markdown_core_inline_reference_label_length(data + 1, len - 1);
    if (at == 1 || at > MAX_LINK_LABEL_LENGTH + 1 || at + 1 >= len || data[at] != ']' || data[at + 1] != ':') {
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
        S_report_destination_field(parser, b, input, before, stop, stop, false);
        return;
    }
    at = markdown_core_skip_spaces_and_line_end(data, at + 2, len);
    if (at >= len || data[at] != '<' || at + 1 + field_end(data + at + 1, len - at - 1, true, true) < len) {
        return;
    }
    S_report_destination_field(parser, b, input, before, at + 1, len, true);
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

/* The member of the attribute block after a definition's destination or
 * title that the content's end leaves open, to the parse's context sink. */
static void S_report_tail(markdown_core_parser *parser, markdown_core_node *b, markdown_core_chunk *input,
                          bufsize_t before) {
    const bufsize_t brace = S_tail_brace(input);
    markdown_core_field member;
    if (brace < 0 ||
        !markdown_core_attributes_open_member(input->data, S_content_length(input), brace, &member, NULL)) {
        return;
    }
    int ignored;
    bufsize_t at[5] = {brace, member.start, member.prefix[0][0], member.prefix[0][1], 0};
    uint32_t place[5];
    for (size_t i = 0; i < 4; i++) {
        bufsize_t source = 0;
        markdown_core_parser_content_place(parser, &b->content_map, before + at[i], &ignored, &source);
        place[i] = (uint32_t)source;
    }
    const int32_t opener = markdown_core_context_opener_add(parser->sink, MARKDOWN_CORE_KIND_NONE,
                                                            (markdown_core_place){place[0], place[0] + 1}, true);
    if (opener < 0) {
        return;
    }
    markdown_core_context_field field = {.kind = member.slot,
                                         .owner = MARKDOWN_CORE_KIND_REFERENCE,
                                         .opener = opener,
                                         .start = place[1],
                                         .end = member.end,
                                         .lead = (uint32_t)member.lead};
    if (member.prefix_count) {
        field.prefix[0] = (markdown_core_place){place[2], place[3]};
        field.prefix_count = 1;
    }
    markdown_core_context_field_add(parser->sink, &field);
}

/* The label of `[text][label` the content's end cuts short, read from the
 * `[` at `start`, to the run's context sink. */
static void S_report_label(markdown_core_inline_state *inline_state, const bracket *opener, bufsize_t start) {
    const unsigned char *data = inline_state->input.data;
    const bufsize_t len = inline_state->input.len;
    if (start >= len || data[start] != '[' ||
        start + 1 + markdown_core_inline_reference_label_length(data + start + 1, len - start - 1) < len) {
        return;
    }
    const bool image = opener->kind == BRACKET_IMAGE;
    const markdown_core_node_kind kind = image ? MARKDOWN_CORE_KIND_EMBEDDED : MARKDOWN_CORE_KIND_LINK;
    const markdown_core_field label = {MARKDOWN_CORE_SLOT_LABEL, kind, start + 1, label_end, {{0}}, 0, false, 0};
    markdown_core_inline_open(inline_state, kind, opener->position - (image ? 2 : 1), opener->position, &label, 1);
}

markdown_core_link_match markdown_core_link_recognize(const markdown_core_element_instance *link,
                                                      markdown_core_inline_state *inline_state, bracket *opener,
                                                      markdown_core_link_candidate *candidate) {
    bufsize_t initial_pos = inline_state->pos, starturl = 0, endurl, starttitle, endtitle, endall, n = -1;
    markdown_core_chunk url_chunk, title_chunk, raw_label;
    markdown_core_chunk url = MARKDOWN_CORE_CHUNK_EMPTY;
    markdown_core_optional_chunk title = {MARKDOWN_CORE_CHUNK_EMPTY, false};
    const markdown_core_registry_record *record = NULL;
    int found_label;
    bool explicit_tail = false;
    // If we got here, we matched a potential link/image text.
    // Now we check to see if it's a link/image.
    bool is_image = opener->kind == BRACKET_IMAGE;

    bool link_allowed = is_image || !markdown_core_brackets(link, inline_state)->no_link_openers;

    bufsize_t after_link_text_pos = inline_state->pos;

    // First, look for an inline link. Its destination, title and closing
    // parenthesis may each follow spaces, tabs and up to one line ending.
    const unsigned char *data = inline_state->input.data;
    const bufsize_t len = inline_state->input.len;
    /* A destination, title or parenthesis the end cuts short may be one
     * once the input goes on. */
    if (link_allowed && markdown_core_inline_peek_char(inline_state) == '(') {
        starturl = markdown_core_skip_spaces_and_line_end(data, inline_state->pos + 1, len);
        n = markdown_core_inline_manual_scan_link_url(&inline_state->input, starturl, &url_chunk);
        if (n < 0) {
            markdown_core_inline_saw_end(inline_state);
            if (inline_state->context) {
                markdown_core_slot_kind reported = MARKDOWN_CORE_SLOT_NONE;
                S_report_destination(inline_state, opener, starturl, &reported);
            }
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
            (endall >= len || (endtitle == starttitle && starttitle < len && strchr("\"'(", data[starttitle])))) {
            markdown_core_inline_saw_end(inline_state);
        }

        if (markdown_core_inline_peek_at(inline_state, endall) == ')') {
            explicit_tail = true;
            inline_state->pos = endall + 1;

            title_chunk = markdown_core_chunk_dup(&inline_state->input, starttitle, endtitle - starttitle);
            {
                int lost = 0;
                url = markdown_core_clean_url(&url_chunk, &lost);
                title = markdown_core_clean_title(&title_chunk, &lost);
                if (lost) {
                    inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
                }
            }
            markdown_core_chunk_free(&url_chunk);
            markdown_core_chunk_free(&title_chunk);
            candidate->url = url;
            candidate->title = title;
            candidate->explicit_tail = true;
            return LINK_EXPLICIT;

        } else {
            // it could still be a shortcut reference link
            inline_state->pos = after_link_text_pos;
        }
    }

    // Next, look for a following [link label] that a definition declares.
    // skip spaces
    raw_label = markdown_core_chunk_literal("");
    found_label = markdown_core_inline_link_label(inline_state, &raw_label);
    if (!found_label && inline_state->context) {
        S_report_label(inline_state, opener, inline_state->pos);
    }
    explicit_tail = found_label;
    if (!found_label) {
        // If we have a shortcut reference link, back up
        // to before the spacse we skipped.
        inline_state->pos = initial_pos;
    }

    if ((!found_label || raw_label.len == 0) && !opener->bracket_after) {
        markdown_core_chunk_free(&raw_label);
        raw_label = markdown_core_chunk_dup(&inline_state->input, opener->position, initial_pos - opener->position - 1);
        found_label = true;
    }

    /* `[t][l]`, `[l][]` and `[l]` resolve identically and to the same node: the
     * `Link` or `Embedded` the definition names (M2). Nothing records which of the
     * three spellings the author wrote, and nothing downstream can recover it
     * -- the module states one node for every successful form. */
    if (link_allowed && found_label) {
        bool failed = false;
        record = markdown_core_parser_find_definition(inline_state->owner_parser, MARKDOWN_CORE_REGISTRY_REFERENCE,
                                                      &raw_label, &failed);
        if (failed) {
            inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        }
    }
    markdown_core_chunk_free(&raw_label);
    candidate->label = record ? markdown_core_registry_label(record) : markdown_core_chunk_literal("");
    candidate->explicit_tail = explicit_tail;
    return record ? (explicit_tail ? LINK_EXPLICIT : LINK_SHORTCUT) : LINK_UNMATCHED;
}

bool markdown_core_link_commit(const markdown_core_element_instance *link, markdown_core_parser *parser,
                               markdown_core_inline_state *inline_state, bracket *opener,
                               markdown_core_link_candidate *candidate, bufsize_t initial_pos) {
    bool is_image = opener->kind == BRACKET_IMAGE;
    bool explicit_tail = candidate->explicit_tail;
    bool reference = candidate->label.len != 0;
    markdown_core_chunk url = candidate->url;
    markdown_core_optional_chunk title = candidate->title;
    markdown_core_node *inl;
    markdown_core_inline_finish_citation_tokens(link->peers[LINK_CITATION], inline_state, &opener->citations);
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
    inl->place.start = opener->inl_text->place.start;
    if (explicit_tail) {
        markdown_core_inline_attach_inline_attributes(inline_state, inl, opener->position - 1);
        inl->place.start = opener->inl_text->place.start;
    }
    markdown_core_inline_state_place(inline_state, inl, opener->position - 1, inline_state->pos - 1);
    inl->place.start = opener->inl_text->place.start;
    // And the destination and title are scanned by markdown_core_inline_manual_scan_link_url and
    // scan_link_title, which move inline_state->pos without ever passing through
    // handle_newline -- so a line ending inside `(...)` is invisible to the
    // inline state, and every later node in the paragraph inherits the error.
    // The extent is projected from the two offsets, which is the repair inline
    // code and raw HTML already
    // use; it walks only [initial_pos, inline_state->pos), which is what the bracket
    // handler consumed for itself. Counting from the OPENING bracket instead
    // would count the label's own newlines a second time -- measured,
    // `[a\nb](/u) tail` then reports line 3 of a two-line document.
    markdown_core_scaffold_insert_before(opener->inl_text, inl);
    markdown_core_inline_take_bracket_content(link, parser, opener, inl);

    /* Only the embedded element opens an image bracket. */
    if (is_image) {
        markdown_core_inline_apply_image_dimensions(link->peers[LINK_EMBEDDED], inline_state, opener, inl,
                                                    initial_pos - 1);
    }

    // Free the bracket [:
    markdown_core_parser_release_node(parser, opener->inl_text);

    markdown_core_inline_process_delimiters(parser, inline_state, opener->position, opener->delim_end);
    markdown_core_inline_pop_bracket(link, inline_state);

    // Now, if we have a link, we also want to deactivate links until
    // we get a new opener. (This code can be removed if we decide to allow links
    // inside links.)
    if (!is_image) {
        markdown_core_brackets(link, inline_state)->no_link_openers = true;
    }

    return true;
}

/* Claim the parsed body of one balanced bracket pair. Span, links/images,
 * and document-owned inline notes share this transfer; their callers decide
 * where the resulting owner lives and when its delimiter boundary closes. */
void markdown_core_inline_take_bracket_content(const markdown_core_element_instance *link, markdown_core_parser *parser,
                                               bracket *opener, markdown_core_node *owner) {
    markdown_core_bracket_work *counts = link->state;
    markdown_core_node *child = markdown_core_scaffold_next(opener->inl_text);
    while (child != opener->close_text) {
        markdown_core_node *next = markdown_core_scaffold_next(child);
        markdown_core_scaffold_unlink(child);
        markdown_core_scaffold_append(owner, child);
        counts->work++;
        child = next;
    }
}

void markdown_core_inline_pop_bracket(const markdown_core_element_instance *link,
                                      markdown_core_inline_state *inline_state) {
    markdown_core_bracket_scope *brackets = markdown_core_brackets(link, inline_state);
    bracket *b = brackets->last;
    if (b == NULL) {
        return;
    }
    brackets->last = b->previous;
    if (b->close_text) {
        if (b->pending_previous) {
            b->pending_previous->pending_next = b->pending_next;
        } else {
            brackets->pending = b->pending_next;
        }
        if (b->pending_next) {
            b->pending_next->pending_previous = b->pending_previous;
        }
        markdown_core_inline_remove_delimiter(inline_state, b->delim_end);
    }
    markdown_core_inline_free_citation_tokens(inline_state, &b->citations);
    markdown_core_free(b);
}

void markdown_core_inline_push_bracket(const markdown_core_element_instance *link,
                                       markdown_core_inline_state *inline_state, bracket_kind kind,
                                       markdown_core_node *inl_text) {
    bracket *b = (bracket *)markdown_core_alloc(1, sizeof(bracket));
    if (!b) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        return;
    }
    markdown_core_bracket_scope *brackets = markdown_core_brackets(link, inline_state);
    if (brackets->last != NULL) {
        brackets->last->bracket_after = true;
        if (kind != BRACKET_FOOTNOTE) {
            b->in_bracket_image0 = brackets->last->in_bracket_image0;
            b->in_bracket_image1 = brackets->last->in_bracket_image1;
        }
    }
    b->kind = kind;
    if (link->peers[LINK_CITATION]) {
        markdown_core_citation_open_bracket(link->peers[LINK_CITATION], inline_state, b);
    }
    b->outer_no_link_openers = brackets->no_link_openers;
    b->active = true;
    b->inl_text = inl_text;
    b->previous = brackets->last;
    b->position = inline_state->pos;
    b->image_pipe = -1;
    b->bracket_after = false;
    if (kind == BRACKET_IMAGE) {
        b->in_bracket_image1 = true;
    } else if (kind == BRACKET_LINK) {
        b->in_bracket_image0 = true;
    }
    brackets->last = b;
    if (kind != BRACKET_IMAGE) {
        brackets->no_link_openers = false;
    }
}

void markdown_core_inline_replace_bracket_opener(markdown_core_inline_state *inline_state, bracket *opener,
                                                 markdown_core_node *replacement) {
    if (opener->kind == BRACKET_IMAGE) {
        opener->inl_text->as.literal->len = 1;
        markdown_core_inline_state_place(inline_state, opener->inl_text, opener->position - 2, opener->position - 2);
        /* The replacement follows the `!`, which stays as text. */
        markdown_core_node *after = markdown_core_scaffold_next(opener->inl_text);
        if (after) {
            markdown_core_scaffold_insert_before(after, replacement);
        } else {
            markdown_core_scaffold_append(markdown_core_scaffold_parent(opener->inl_text), replacement);
        }
    } else {
        markdown_core_scaffold_insert_before(opener->inl_text, replacement);
        markdown_core_parser_release_node(inline_state->owner_parser, opener->inl_text);
    }
}

markdown_core_node *markdown_core_inline_handle_close_bracket(const markdown_core_element_instance *link,
                                                              markdown_core_parser *parser,
                                                              markdown_core_inline_state *inline_state) {
    const markdown_core_element_instance *citation = link->peers[LINK_CITATION];
    const markdown_core_element_instance *footnote = link->peers[LINK_FOOTNOTE];
    ((markdown_core_bracket_work *)link->state)->work++;
    advance(inline_state);
    bufsize_t initial_pos = inline_state->pos;
    bracket *opener = markdown_core_brackets(link, inline_state)->last;
    if (!opener) {
        return make_str(inline_state, initial_pos - 1, initial_pos - 1,
                        markdown_core_chunk_dup(&inline_state->input, initial_pos - 1, 1));
    }
    /* Only the footnote element opens a footnote bracket. */
    if (opener->kind == BRACKET_FOOTNOTE) {
        return markdown_core_inline_close_inline_footnote(footnote, parser, inline_state, opener);
    }

    /* One bracket owns its parsed children. Alternatives only claim that
     * existing range, in syntax precedence order; none reparses its body. */
    markdown_core_link_candidate candidate = {0};
    markdown_core_link_match match = markdown_core_link_recognize(link, inline_state, opener, &candidate);
    if (match == LINK_EXPLICIT) {
        if (markdown_core_link_commit(link, parser, inline_state, opener, &candidate, initial_pos)) {
            return NULL;
        }
        goto no_match;
    }
    inline_state->pos = initial_pos;
    markdown_core_bracket_match span = markdown_core_span_close(link, citation, parser, inline_state, opener);
    if (span == BRACKET_MATCHED) {
        return NULL;
    }
    if (span == BRACKET_REJECTED) {
        goto no_match;
    }
    markdown_core_node *close = NULL;
    if (citation && markdown_core_citation_defer_tail(citation, inline_state, opener, &close)) {
        return close;
    }
    if (citation && markdown_core_inline_close_bibliography(citation, parser, inline_state, opener)) {
        return NULL;
    }
    if (match == LINK_SHORTCUT) {
        if (markdown_core_link_commit(link, parser, inline_state, opener, &candidate, initial_pos)) {
            return NULL;
        }
        goto no_match;
    }
    if (footnote && markdown_core_footnote_close_reference(footnote, parser, inline_state, opener)) {
        return NULL;
    }

no_match:
    markdown_core_inline_finish_citation_tokens(citation, inline_state, &opener->citations);
    markdown_core_inline_pop_bracket(link, inline_state);
    inline_state->pos = initial_pos;
    return make_str(inline_state, initial_pos - 1, initial_pos - 1,
                    markdown_core_chunk_dup(&inline_state->input, initial_pos - 1, 1));
}

static markdown_core_node *match_bracket(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                         markdown_core_node *parent, unsigned char character,
                                         markdown_core_inline_state *inline_state) {
    if (character == ']') {
        return markdown_core_inline_handle_close_bracket(self, parser, inline_state);
    }
    if (character == '[') {
        advance(inline_state);
        markdown_core_node *text = make_str(inline_state, inline_state->pos - 1, inline_state->pos - 1,
                                            markdown_core_chunk_dup(&inline_state->input, inline_state->pos - 1, 1));
        if (text) {
            markdown_core_inline_push_bracket(self, inline_state, BRACKET_LINK, text);
        }
        return text;
    }
    return NULL;
}
static void dispose_inline(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state) {
    markdown_core_bracket_scope *brackets = markdown_core_brackets(self, inline_state);
    while (brackets->last) {
        markdown_core_inline_pop_bracket(self, inline_state);
    }
    while (brackets->pending) {
        bracket *next = brackets->pending->pending_next;
        markdown_core_inline_free_citation_tokens(inline_state, &brackets->pending->citations);
        markdown_core_free(brackets->pending);
        brackets->pending = next;
    }
}

static void init_inline(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state) {
    markdown_core_brackets(self, inline_state)->no_link_openers = true;
}

/* An open bracket, or one whose citation tail waits for its key's owner. */
static bool holds_open(const markdown_core_element_instance *self, const markdown_core_inline_state *inline_state) {
    const markdown_core_bracket_scope *brackets = markdown_core_run_state(inline_state, self);
    return brackets->last || brackets->pending;
}

/* The brackets no closer met, each an opener: a `[` whose construct is
 * still open, an image's `![` and an inline note's `^[`. A `[^` reads a
 * footnote call's label, and at the start of a line of its content a
 * definition's; any other `[` there reads a reference definition's label. A
 * definition is one only where the document finishes it. */
static void report_open(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state) {
    for (const bracket *b = markdown_core_brackets(self, inline_state)->last; b; b = b->previous) {
        const bool wide = b->kind != BRACKET_LINK;
        const bufsize_t start = b->position - (wide ? 2 : 1);
        markdown_core_field fields[2];
        size_t count = 0;
        markdown_core_node_kind opens = b->kind == BRACKET_IMAGE      ? MARKDOWN_CORE_KIND_EMBEDDED
                                        : b->kind == BRACKET_FOOTNOTE ? MARKDOWN_CORE_KIND_FOOTNOTE
                                                                      : MARKDOWN_CORE_KIND_NONE;
        const bool label = b->kind == BRACKET_LINK && b->position + markdown_core_inline_reference_label_length(
                                                                        inline_state->input.data + b->position,
                                                                        inline_state->input.len - b->position) ==
                                                          inline_state->input.len;
        const bool caret =
            label && b->position < inline_state->input.len && inline_state->input.data[b->position] == '^';
        if (caret) {
            opens = MARKDOWN_CORE_KIND_CITE;
            fields[count++] = (markdown_core_field){MARKDOWN_CORE_SLOT_FOOTNOTE,
                                                    MARKDOWN_CORE_KIND_CITATION,
                                                    b->position + 1,
                                                    label_end,
                                                    {{0}},
                                                    0,
                                                    false,
                                                    0};
        }
        const unsigned char *data = inline_state->input.data;
        const bool line = start == 0 || data[start - 1] == '\n' || data[start - 1] == '\r';
        /* A footnote definition's label holds any byte but `]` and white
         * space, brackets too, which no other label does. */
        if (!label && line && b->kind == BRACKET_LINK && b->position < inline_state->input.len &&
            data[b->position] == '^' &&
            b->position + 1 + note_end(data + b->position + 1, inline_state->input.len - b->position - 1) ==
                inline_state->input.len) {
            fields[count++] = (markdown_core_field){
                MARKDOWN_CORE_SLOT_FOOTNOTE, MARKDOWN_CORE_KIND_FOOTNOTE, b->position + 1, note_end, {{0}}, 0, true, 0};
        }
        if (label && line) {
            fields[count++] = caret ? (markdown_core_field){MARKDOWN_CORE_SLOT_FOOTNOTE,
                                                            MARKDOWN_CORE_KIND_FOOTNOTE,
                                                            b->position + 1,
                                                            label_end,
                                                            {{0}},
                                                            0,
                                                            true,
                                                            0}
                                    : (markdown_core_field){MARKDOWN_CORE_SLOT_LABEL,
                                                            MARKDOWN_CORE_KIND_REFERENCE,
                                                            b->position,
                                                            label_end,
                                                            {{0}},
                                                            0,
                                                            true,
                                                            0};
        }
        markdown_core_inline_open(inline_state, opens, start, caret ? b->position + 1 : b->position, fields, count);
    }
}

const markdown_core_element MARKDOWN_CORE_ELEMENT_LINK = {
    .peers = LINK_PEERS,
    .init_inline = init_inline,
    .holds_open = holds_open,
    .report_open = report_open,
    .state_size = sizeof(markdown_core_bracket_work),
    .run_state_size = sizeof(markdown_core_bracket_scope),

    .inline_precedence = MARKDOWN_CORE_INLINE_FALLBACK,
    .dispose_inline = dispose_inline,

    .name = "link",
    .match_inline = match_bracket,
    .terminates_text = "[]",
    .dispatch = "[]",
};

int markdown_core_inline_state_in_bracket(const markdown_core_element_instance *link,
                                          markdown_core_inline_state *inline_state, int image) {
    bracket *b = markdown_core_open_bracket(link, inline_state);
    if (!b) {
        return 0;
    }
    if (image != 0) {
        return b->in_bracket_image1;
    } else {
        return b->in_bracket_image0;
    }
}
unsigned char markdown_core_inline_state_closing_bracket(const markdown_core_element_instance *link,
                                                         markdown_core_inline_state *inline_state) {
    return markdown_core_open_bracket(link, inline_state) ? ']' : 0;
}
int markdown_core_inline_state_context_start(const markdown_core_element_instance *link,
                                             markdown_core_inline_state *inline_state) {
    bracket *opener = markdown_core_open_bracket(link, inline_state);
    return opener && opener->kind == BRACKET_FOOTNOTE ? opener->position : 0;
}
