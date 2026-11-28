#include "alloc.h"
#include "escape.h"
#include "entities.h"
#include "link_scanners.h"
#define advance(inline_state) ((inline_state)->pos += 1)
#include "attributes.h"
#include "link.h"
#include "embed.h"
#include "footnote.h"
#include "inline_internal.h"
#include "block_internal.h"

/* The elements whose state this element reads, as `self->peers` holds them. */
enum { LINK_EMBED, LINK_ATTRIBUTES, LINK_FOOTNOTE, LINK_ESCAPE, LINK_ENTITIES };
static const markdown_core_element *const LINK_PEERS[] = {
    [LINK_EMBED] = &MARKDOWN_CORE_ELEMENT_EMBED,       [LINK_ATTRIBUTES] = &MARKDOWN_CORE_ELEMENT_ATTRIBUTES,
    [LINK_FOOTNOTE] = &MARKDOWN_CORE_ELEMENT_FOOTNOTE, [LINK_ESCAPE] = &MARKDOWN_CORE_ELEMENT_ESCAPE,
    [LINK_ENTITIES] = &MARKDOWN_CORE_ELEMENT_ENTITIES, NULL};

const markdown_core_bracket_shape MARKDOWN_CORE_LINK_BRACKET = {.width = 1, .mark = 0, .closer = ']'};

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

static void dispose_parser(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    (void)parser;
    markdown_core_attribute_parser_free(&((S_state *)self->state)->held);
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

    markdown_core_entities_copy(link->peers[LINK_ENTITIES], &buf, url->data, url->len);

    markdown_core_escape_strip(link->peers[LINK_ESCAPE], &buf);
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
        markdown_core_entities_copy(link->peers[LINK_ENTITIES], &buf, title->data + 1, title->len - 2);
    } else {
        markdown_core_entities_copy(link->peers[LINK_ENTITIES], &buf, title->data, title->len);
    }

    markdown_core_escape_strip(link->peers[LINK_ESCAPE], &buf);
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

    /* A parenthesis left open makes no destination (CommonMark 6.3: only
     * escaped or balanced pairs belong to one). */
    if (i >= input->len || nb_p != 0) {
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
                           markdown_core_attributes *value, bool *wide) {
    bufsize_t before = inline_state->pos;
    if (!attributes) {
        /* A dialect without attribute blocks reads none. */
        markdown_core_inline_skip_spaces(inline_state);
        return markdown_core_inline_skip_line_end(inline_state);
    }
    spnl(inline_state);
    bufsize_t base = (bufsize_t)(inline_state->input.data - attributes->data);
    bufsize_t start = base + inline_state->pos;
    /* An attribute list looked for reads on to where it ends, or to the
     * text's end. */
    *wide |= inline_state->pos < inline_state->input.len && inline_state->input.data[inline_state->pos] == '{';
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
 * one. `wide` when its reading may have read past its end and the first
 * byte after it: a title or an attribute list looked for, which reads on
 * to where it closes, or to the text's end. */
typedef struct {
    markdown_core_chunk label, url, title;
    markdown_core_attributes value;
    bool wide;
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
    if (!reference_tail(&inline_state, attributes, &definition->value, &definition->wide)) {
        if (matchlen) { // try rewinding before title
            inline_state.pos = beforetitle;
            if (!reference_tail(&inline_state, attributes, &definition->value, &definition->wide)) {
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
    const markdown_core_element_instance *peer = self->peers[LINK_ATTRIBUTES];
    markdown_core_attribute_parser attributes = {
        .data = input->data, .length = input->len, .scratch = peer ? markdown_core_attributes_scratch(peer) : NULL};
    reference_definition definition;
    bufsize_t length = S_reference_definition(input, peer ? &attributes : NULL, &definition);
    if (length) {
        markdown_core_attributes_free(&definition.value);
    }
    if (attributes.oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    markdown_core_attribute_parser_free(&attributes);
    return length;
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
    const markdown_core_element_instance *peer = self->peers[LINK_ATTRIBUTES];
    markdown_core_attribute_parser *held = &((S_state *)self->state)->held;
    if (peer && (cut->at == 0 || held->data != cut->text.data || held->length != cut->text.len)) {
        markdown_core_attribute_parser_free(held);
        *held = (markdown_core_attribute_parser){
            .data = cut->text.data, .length = cut->text.len, .scratch = markdown_core_attributes_scratch(peer)};
    }
    reference_definition definition; /* S_reference_definition writes it whole. */
    bufsize_t length = S_reference_definition(input, peer ? held : NULL, &definition);
    if (peer && held->oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    cut->wide = definition.wide;
    if (cut->measure) {
        if (length) {
            markdown_core_attributes_free(&definition.value);
        }
        return length;
    }
    if (!length) {
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
    const bufsize_t start = (bufsize_t)reference->place.start;
    cut->made = reference;
    /* The document defines the label from where the definition starts. */
    if (!parser->error) {
        markdown_core_parser_declare(parser, self->spaces[LINK_REFERENCE], &definition.label, start,
                                     &reference->as.reference->label);
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
        markdown_core_attribute_parser *held = &((S_state *)self->state)->held;
        markdown_core_attribute_parser_free(held);
        *held = (markdown_core_attribute_parser){0};
    }
    return length;
}

markdown_core_link_match markdown_core_link_recognize(const markdown_core_element_instance *link,
                                                      markdown_core_inline_state *inline_state, bracket *opener,
                                                      markdown_core_link_candidate *candidate) {
    bufsize_t initial_pos = inline_state->pos, starturl = 0, endurl, starttitle, endtitle, endall, n = -1;
    markdown_core_chunk url_chunk, title_chunk, raw_label;
    markdown_core_chunk url = MARKDOWN_CORE_CHUNK_EMPTY;
    markdown_core_optional_chunk title = {MARKDOWN_CORE_CHUNK_EMPTY, false};
    const markdown_core_roster_record *record = NULL;
    int found_label;
    bool explicit_tail = false;
    // If we got here, we matched a potential link/image text.
    // Now we check to see if it's a link/image.
    bool is_image = opener->shape == &MARKDOWN_CORE_EMBED_BRACKET;

    bool link_allowed = is_image || !markdown_core_brackets(inline_state)->no_link_openers;

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
     * `Link` or `Embed` the definition names (M2). Nothing records which of the
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
    candidate->label = record ? markdown_core_roster_label(record) : markdown_core_chunk_literal("");
    candidate->explicit_tail = explicit_tail;
    return record ? (explicit_tail ? LINK_EXPLICIT : LINK_SHORTCUT) : LINK_UNMATCHED;
}

bool markdown_core_link_commit(const markdown_core_element_instance *link, markdown_core_parser *parser,
                               markdown_core_inline_state *inline_state, bracket *opener,
                               markdown_core_link_candidate *candidate, bufsize_t initial_pos) {
    bool is_image = opener->shape == &MARKDOWN_CORE_EMBED_BRACKET;
    bool explicit_tail = candidate->explicit_tail;
    bool reference = candidate->label.len != 0;
    markdown_core_chunk url = candidate->url;
    markdown_core_optional_chunk title = candidate->title;
    markdown_core_node *inl;
    markdown_core_inline_settle_bracket(inline_state, opener);
    if (!markdown_core_node_can_contain_type(markdown_core_scaffold_parent(opener->inl_text),
                                             is_image ? MARKDOWN_CORE_NODE_EMBED : MARKDOWN_CORE_NODE_LINK)) {
        markdown_core_chunk_free(&url);
        markdown_core_optional_chunk_free(&title);
        return false;
    }
    inl = markdown_core_inline_make_simple(inline_state, is_image ? MARKDOWN_CORE_NODE_EMBED : MARKDOWN_CORE_NODE_LINK);
    if (inl && reference) {
        /* A RESOLVED REFERENCE NAMES ITS DEFINITION BY LABEL: the Link or
         * Embed holds its own copy of the normalized label, and its
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
    markdown_core_inline_state_place(inline_state, inl, opener->position - 1, inline_state->pos - 1);
    inl->place.start = opener->inl_text->place.start;
    /* What follows a link whose destination, title or label follows its `]`
     * may update it (no-flush.md 10.13). */
    if (explicit_tail) {
        markdown_core_inline_made(inline_state, inl);
    }
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
    markdown_core_inline_take_bracket_content(parser, inline_state, opener, inl);

    /* Only the embed element opens an image bracket. */
    if (is_image) {
        markdown_core_inline_apply_image_dimensions(link->peers[LINK_EMBED], inline_state, opener, inl,
                                                    initial_pos - 1);
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
    const bufsize_t after = inline_state->pos;
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
    if (tail->match != LINK_SHORTCUT) {
        return BRACKET_UNMATCHED;
    }
    return markdown_core_link_commit(self, parser, inline_state, opener, &tail->candidate, inline_state->pos)
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
        markdown_core_node *text = make_str(inline_state, inline_state->pos - 1, inline_state->pos - 1,
                                            markdown_core_chunk_dup(&inline_state->input, inline_state->pos - 1, 1));
        if (text) {
            markdown_core_inline_push_bracket(self, inline_state, &MARKDOWN_CORE_LINK_BRACKET, text);
        }
        return text;
    }
    return NULL;
}

static const markdown_core_inline_ops LINK_INLINES = {
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

const markdown_core_element MARKDOWN_CORE_ELEMENT_LINK = {
    .peers = LINK_PEERS,
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
