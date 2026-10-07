#include "autolink_scanners.h"
#include "inline_internal.h"
#include "attributes.h"
#include "autolink.h"
#include "link.h"
#include "element.h"
#include <parser.h>
#include <string.h>
#include <utf8.h>
#include <stddef.h>

#if defined(_WIN32)
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

/* The elements whose state this element reads, as `self->peers` holds them. */
enum { AUTOLINK_LINK };
static const markdown_core_element *const AUTOLINK_PEERS[] = {[AUTOLINK_LINK] = &MARKDOWN_CORE_ELEMENT_LINK, NULL};

static markdown_core_node *make_str_with_entities(markdown_core_inline_state *inline_state, int start_column,
                                                  int end_column, markdown_core_chunk *content) {
    markdown_core_strbuf unescaped = MARKDOWN_CORE_BUF_INIT();

    if (houdini_unescape_html(&unescaped, content->data, content->len)) {
        if (unescaped.oom) {
            inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        }
        return make_str(inline_state, start_column, end_column, markdown_core_chunk_buf_detach(&unescaped));
    } else {
        return make_str(inline_state, start_column, end_column, *content);
    }
}

static markdown_core_chunk markdown_core_clean_autolink(markdown_core_inline_state *inline_state,
                                                        markdown_core_chunk *url, int is_email) {
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT();

    markdown_core_chunk_trim(url);

    if (url->len == 0) {
        markdown_core_chunk result = MARKDOWN_CORE_CHUNK_EMPTY;
        return result;
    }

    if (is_email) {
        markdown_core_strbuf_puts(&buf, "mailto:");
    }

    houdini_unescape_html_f(&buf, url->data, url->len);
    if (buf.oom) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
    }
    return markdown_core_chunk_buf_detach(&buf);
}

/* An autolink token joins the content with its text as its child. */
static markdown_core_member *S_append_link(markdown_core_inline_state *inline_state, markdown_core_node *link,
                                           markdown_core_node *text) {
    markdown_core_member *member = markdown_core_inline_state_append(inline_state, link);
    if (!member) {
        if (text) {
            markdown_core_parser_release_node(inline_state->owner_parser, text);
        }
        return NULL;
    }
    if (text && !markdown_core_parser_attach(inline_state->owner_parser, member, text, NULL)) {
        return NULL;
    }
    return member;
}

static MARKDOWN_CORE_INLINE markdown_core_member *make_autolink(markdown_core_inline_state *inline_state,
                                                                int start_column, int end_column,
                                                                markdown_core_chunk url, int is_email) {
    markdown_core_node *link = markdown_core_inline_make_simple(inline_state, MARKDOWN_CORE_NODE_LINK);
    markdown_core_node *text;
    if (!link) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        return NULL;
    }
    {
        // No title here: an autolink has no syntax for one, so the resource is
        // built with absence. It used to be set to an empty title, and
        // `elements.txt` records both spellings of one construct on one line
        // disagreeing about it three columns apart.
        markdown_core_chunk destination = markdown_core_clean_autolink(inline_state, &url, is_email);
        link->as.link->resource = markdown_core_resource_new(inline_state->owner_parser->pool, destination,
                                                             markdown_core_optional_chunk_absent());
        if (!link->as.link->resource) {
            inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
            markdown_core_chunk_free(&destination);
            markdown_core_parser_release_node(inline_state->owner_parser, link);
            return NULL;
        }
    }

    // Both offsets, like every other column in this file. This was the one site
    // that turned a raw inline state-buffer offset into a column without them, so an
    // autolink inside a block quote, or on any line but the first of its
    // paragraph, produced a Link that did not contain its own Text.
    markdown_core_inline_state_place(inline_state, link, start_column, end_column);
    text = make_str_with_entities(inline_state, start_column + 1, end_column - 1, &url);
    markdown_core_inline_attach_inline_attributes(inline_state, link, start_column);
    /* The pointy braces are the syntax; what they enclose is the text. */
    return S_append_link(inline_state, link, text);
}

static markdown_core_member *match_angle(markdown_core_inline_state *inline_state) {
    bufsize_t from = inline_state->pos + 1;
    bufsize_t length = scan_autolink_uri(inline_state->input.data, inline_state->input.len, from);
    bool email = false;
    if (!length) {
        length = scan_autolink_email(inline_state->input.data, inline_state->input.len, from);
        email = true;
    }
    if (!length) {
        return NULL;
    }
    markdown_core_chunk content = markdown_core_chunk_dup(&inline_state->input, from, length - 1);
    inline_state->pos = from + length;
    return make_autolink(inline_state, from - 1, inline_state->pos - 1, content, email);
}

/* The width of the host character at `link`, or 0 when the character there
 * is not one. A host character is neither whitespace nor punctuation.
 * Whitespace is a space, a tab or a line ending, as everywhere outside
 * delimiter flanking, so a non-breaking space is a host character. Both
 * callers have at least one byte at `link`. */
static inline int hostchar_width(const uint8_t *link, size_t link_len) {
    int32_t ch;
    int width = markdown_core_utf8proc_decode(link, (bufsize_t)link_len, &ch);
    if (markdown_core_is_whitespace(link[0]) ||
        (markdown_core_utf8proc_classes(ch) & MARKDOWN_CORE_UNICODE_PUNCTUATION)) {
        return 0;
    }
    return width;
}

static int sd_autolink_issafe(const uint8_t *link, size_t link_len) {
    static const size_t valid_uris_count = 3;
    static const char *const valid_uris[] = {"http://", "https://", "ftp://"};

    size_t i;

    for (i = 0; i < valid_uris_count; ++i) {
        size_t len = strlen(valid_uris[i]);

        if (link_len > len && strncasecmp((char *)link, valid_uris[i], len) == 0 &&
            hostchar_width(link + len, link_len - len)) {
            return 1;
        }
    }

    return 0;
}

static size_t autolink_delim(uint8_t *data, size_t link_end) {
    size_t i;
    size_t closing = 0;
    size_t opening = 0;

    for (i = 0; i < link_end; ++i) {
        const uint8_t c = data[i];
        if (c == '<') {
            link_end = i;
            break;
        } else if (c == '(') {
            opening++;
        } else if (c == ')') {
            closing++;
        }
    }

    while (link_end > 0) {
        switch (data[link_end - 1]) {
        case ')':
            /* Allow any number of matching brackets (as recognised in copen/cclose)
             * at the end of the URL.  If there is a greater number of closing
             * brackets than opening ones, we remove one character from the end of
             * the link.
             *
             * Examples (input text => output linked portion):
             *
             *        http://www.pokemon.com/Pikachu_(Electric)
             *                => http://www.pokemon.com/Pikachu_(Electric)
             *
             *        http://www.pokemon.com/Pikachu_((Electric)
             *                => http://www.pokemon.com/Pikachu_((Electric)
             *
             *        http://www.pokemon.com/Pikachu_(Electric))
             *                => http://www.pokemon.com/Pikachu_(Electric)
             *
             *        http://www.pokemon.com/Pikachu_((Electric))
             *                => http://www.pokemon.com/Pikachu_((Electric))
             */
            if (closing <= opening) {
                return link_end;
            }
            closing--;
            link_end--;
            break;
        case '?':
        case '!':
        case '.':
        case ',':
        case ':':
        case '*':
        case '_':
        case '~':
        case '\'':
        case '"':
            link_end--;
            break;
        case ';': {
            size_t new_end = link_end - 2;

            while (new_end > 0 && markdown_core_isalpha(data[new_end])) {
                new_end--;
            }

            if (new_end < link_end - 2 && data[new_end] == '&') {
                link_end = new_end;
            } else {
                link_end--;
            }
            break;
        }

        default:
            return link_end;
        }
    }

    return link_end;
}

/* THE AUTOLINK SCANNER'S STATE. Its run record: where a shared domain suffix
 * was rejected, so that the starts before its underscore reject it without
 * rescanning. Its parse record (autolink.h): the domain bytes scanned, for
 * the scanner's complexity gate. */
typedef struct {
    bufsize_t rejected_until;
} autolink_run;

static size_t check_domain(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state,
                           uint8_t *data, size_t size, int allow_short) {
    size_t i, np = 0, uscore1 = 0, uscore2 = 0, last_underscore = 0;
    bufsize_t start = (bufsize_t)(data - inline_state->input.data);
    autolink_run *run = markdown_core_run_state(inline_state, self);
    markdown_core_autolink_work *counts = self->state;
    counts->domains++;
    if (start < run->rejected_until) {
        return 0;
    }

    /* The purpose of this code is to reject urls that contain an underscore
     * in one of the last two segments. Examples:
     *
     *   www.xxx.yyy.zzz     autolinked
     *   www.xxx.yyy._zzz    not autolinked
     *   www.xxx._yyy.zzz    not autolinked
     *   www._xxx.yyy.zzz    autolinked
     *
     * The reason is that domain names are allowed to include underscores,
     * but host names are not. See: https://stackoverflow.com/a/2183140
     *
     * The walk starts after the first character, which the caller has
     * matched, and steps a whole host character at a time. Stepping a byte
     * reached the second byte of a multi-byte character, which is no
     * character at all, and ended the domain there: an underscore after
     * any non-ASCII letter escaped the rule the same underscore after an
     * ASCII letter obeys. The first character's width is decoded within
     * `size`, which both callers make at least one byte: a lead byte's own
     * width could claim bytes past the end of the range. */
    int32_t first;
    for (i = (size_t)markdown_core_utf8proc_decode(data, (bufsize_t)size, &first); i < size - 1;) {
        counts->domains++;
        if (data[i] == '\\' && i < size - 2) {
            i++;
        }
        if (data[i] == '_') {
            uscore2++;
            last_underscore = i;
        } else if (data[i] == '.') {
            uscore1 = uscore2;
            uscore2 = 0;
            np++;
        } else if (data[i] != '-') {
            int width = hostchar_width(data + i, size - i);
            if (!width) {
                break;
            }
            i += (size_t)width;
            continue;
        }
        i++;
    }

    if (uscore1 > 0 || uscore2 > 0) {
        /* Every later candidate before this underscore still has it in its
         * last two segments. Keep that rejection frontier, not the whole
         * host end: a candidate after the underscore may be valid. A shared
         * suffix is scanned once, with no segment-count change in grammar. */
        run->rejected_until = start + (bufsize_t)last_underscore;
        return 0;
    }

    if (allow_short) {
        /* We don't need a valid domain in the strict sense (with
         * least one dot; so just make sure it's composed of valid
         * domain characters and return the length of the the valid
         * sequence. */
        return i;
    } else {
        /* a valid domain needs to have at least a dot.
         * that's as far as we get */
        return np ? i : 0;
    }
}

static void set_sourcepos_from_range(markdown_core_parser *parser, markdown_core_node *node,
                                     const markdown_core_content_map *source, size_t start, size_t len) {
    node->where.place = (markdown_core_place){0, 0};
    if (!len) {
        return;
    }
    int line;
    bufsize_t from, to;
    markdown_core_parser_content_place(parser, source, (bufsize_t)start, &line, &from);
    markdown_core_parser_content_end_place(parser, source, (bufsize_t)(start + len - 1), &line, &to);
    node->where.place = (markdown_core_place){(uint32_t)from, (uint32_t)to};
    if (node->kind == MARKDOWN_CORE_NODE_TEXT) {
        markdown_core_parser_adopt_content_marks(parser, source, &node->content_map, (bufsize_t)start, (bufsize_t)len);
    }
}

/* URL and www candidates use the same extent rule. Bracket bodies supply
 * their delimiter; escaped punctuation stays in the opaque token. Each byte
 * is visited once, including tokens that end at a footnote's closing ]. */
static size_t autolink_extent(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state,
                              uint8_t *data, size_t size, size_t offset) {
    unsigned char closer = markdown_core_inline_state_closing_bracket(self->peers[AUTOLINK_LINK], inline_state);
    while (offset < size && !markdown_core_is_whitespace(data[offset]) && data[offset] != '<') {
        if (data[offset] == closer) {
            break;
        }
        if (data[offset] == '\\' && offset + 1 < size && markdown_core_ispunct(data[offset + 1]) &&
            data[offset + 1] != '<') {
            offset++;
        }
        offset++;
    }
    return offset;
}

static markdown_core_member *www_match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                       markdown_core_member *parent, markdown_core_inline_state *inline_state) {
    markdown_core_chunk *chunk = markdown_core_inline_state_get_chunk(inline_state);
    size_t max_rewind = markdown_core_inline_state_get_offset(inline_state);
    uint8_t *data = chunk->data + max_rewind;
    size_t size = chunk->len - max_rewind;

    size_t link_end;

    if (max_rewind > (size_t)markdown_core_inline_state_context_start(self->peers[AUTOLINK_LINK], inline_state) &&
        strchr("*_~(", data[-1]) == NULL && !markdown_core_is_whitespace(data[-1])) {
        return 0;
    }

    if (size < 4 || memcmp(data, "www.", strlen("www.")) != 0) {
        return 0;
    }

    link_end = check_domain(self, inline_state, data, size, 0);

    if (link_end == 0) {
        return NULL;
    }

    link_end = autolink_extent(self, inline_state, data, size, link_end);

    link_end = autolink_delim(data, link_end);

    if (link_end == 0) {
        return NULL;
    }

    markdown_core_inline_state_set_offset(inline_state, (int)(max_rewind + link_end));

    markdown_core_node *node = markdown_core_parser_make_node(parser, MARKDOWN_CORE_NODE_LINK);
    if (!node) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }

    markdown_core_strbuf buf;
    markdown_core_strbuf_init(&buf, 10);
    markdown_core_strbuf_puts(&buf, "http://");
    markdown_core_strbuf_put(&buf, data, (bufsize_t)link_end);
    {
        markdown_core_chunk url = markdown_core_chunk_buf_detach(&buf);
        node->as.link->resource =
            url.data ? markdown_core_resource_new(parser->pool, url, markdown_core_optional_chunk_absent()) : NULL;
        if (!node->as.link->resource) {
            markdown_core_chunk_free(&url);
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
    }

    markdown_core_node *text = markdown_core_parser_make_node(parser, MARKDOWN_CORE_NODE_TEXT);
    if (!text) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        markdown_core_parser_release_node(parser, node);
        return NULL;
    }
    *text->as.literal = markdown_core_chunk_dup(chunk, (bufsize_t)max_rewind, (bufsize_t)link_end);
    markdown_core_inline_state_place(inline_state, node, (int)max_rewind, (int)(max_rewind + link_end - 1));
    markdown_core_inline_state_place(inline_state, text, (int)max_rewind, (int)(max_rewind + link_end - 1));

    return S_append_link(inline_state, node, text);
}

static markdown_core_member *url_match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                       markdown_core_member *parent, markdown_core_inline_state *inline_state) {
    size_t link_end, domain_len;
    int rewind = 0;

    markdown_core_chunk *chunk = markdown_core_inline_state_get_chunk(inline_state);
    int max_rewind = markdown_core_inline_state_get_offset(inline_state);
    uint8_t *data = chunk->data + max_rewind;
    size_t size = chunk->len - max_rewind;

    if (size < 4 || data[1] != '/' || data[2] != '/') {
        return 0;
    }

    while (rewind < max_rewind && markdown_core_isalpha(data[-rewind - 1])) {
        rewind++;
    }

    if (!sd_autolink_issafe(data - rewind, size + rewind)) {
        return 0;
    }

    link_end = strlen("://");

    domain_len = check_domain(self, inline_state, data + link_end, size - link_end, 1);

    if (domain_len == 0) {
        return 0;
    }

    link_end += domain_len;
    link_end = autolink_extent(self, inline_state, data, size, link_end);

    link_end = autolink_delim(data, link_end);

    if (link_end == 0) {
        return NULL;
    }

    markdown_core_inline_state_set_offset(inline_state, (int)(max_rewind + link_end));
    markdown_core_node_unput(parser, parent, rewind);

    markdown_core_node *node = markdown_core_parser_make_node(parser, MARKDOWN_CORE_NODE_LINK);
    if (!node) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }

    markdown_core_chunk url = markdown_core_chunk_dup(chunk, max_rewind - rewind, (bufsize_t)(link_end + rewind));
    node->as.link->resource = markdown_core_resource_new(parser->pool, url, markdown_core_optional_chunk_absent());
    if (!node->as.link->resource) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }

    markdown_core_node *text = markdown_core_parser_make_node(parser, MARKDOWN_CORE_NODE_TEXT);
    if (!text) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        markdown_core_parser_release_node(parser, node);
        return NULL;
    }
    *text->as.literal = url;
    markdown_core_inline_state_place(inline_state, node, max_rewind - rewind, (int)(max_rewind + link_end - 1));
    markdown_core_inline_state_place(inline_state, text, max_rewind - rewind, (int)(max_rewind + link_end - 1));

    return S_append_link(inline_state, node, text);
}

/* A3 BEFORE A10. A bare address is an autolink wherever it appears, and the
 * text-directive scanner also claims a colon before a name, so at
 * `mailto:x@y.z`, `xmpp:x@y.z`, or any `at:x@y.z` whichever scanner runs
 * first owns the colon. The dialect's recognition order gives it to the
 * autolink, as cmark-gfm does and remark-directive does not.
 *
 * Only the colon is claimed, as one opaque byte of text. The address bytes
 * stay with the base scanner, which is exactly what cmark-gfm's postprocess
 * pass sees: nothing in an address is special to the base language except
 * `_`, an intraword `_` is literal, and a `_` that does form emphasis splits
 * the address on both sides alike -- `[foo:_x_@y.z](u)` keeps its emphasis.
 * The runs are consolidated before that step, which then links the address
 * byte for byte as cmark-gfm links it, `mailto:` spelling included, and skips
 * it inside a link. */
static markdown_core_member *address_match(markdown_core_parser *parser, markdown_core_inline_state *inline_state) {
    markdown_core_chunk *chunk = markdown_core_inline_state_get_chunk(inline_state);
    size_t offset = (size_t)markdown_core_inline_state_get_offset(inline_state);
    uint8_t *data = chunk->data + offset;
    size_t size = chunk->len - offset;
    size_t at, end, np = 0;
    markdown_core_node *node;

    (void)parser;
    /* The local part, then '@'. */
    for (at = 1; at < size && (markdown_core_isalnum(data[at]) || strchr(".+-_", data[at]) != NULL); at++) {
    }
    if (at == 1 || at >= size || data[at] != '@') {
        return NULL;
    }
    /* The domain, as `link_text_addresses` scans it. */
    for (end = at + 1; end < size; end++) {
        uint8_t c = data[end];
        if (markdown_core_isalnum(c)) {
            continue;
        }
        if (c == '.' && end + 1 < size && markdown_core_isalnum(data[end + 1])) {
            np++;
            continue;
        }
        if (c != '-' && c != '_') {
            break;
        }
    }
    if (end - at < 2 || np == 0 || (!markdown_core_isalpha(data[end - 1]) && data[end - 1] != '.')) {
        return NULL;
    }

    node = markdown_core_inline_state_make_delimiter_text(inline_state, (int)offset, (int)offset);
    if (!node) {
        return NULL;
    }
    markdown_core_inline_state_set_offset(inline_state, (int)(offset + 1));
    return markdown_core_inline_state_append(inline_state, node);
}

static markdown_core_member *match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                   markdown_core_member *parent, unsigned char c,
                                   markdown_core_inline_state *inline_state) {
    if (c == '<') {
        return match_angle(inline_state);
    }

    int in_bracket = markdown_core_inline_state_in_bracket(self->peers[AUTOLINK_LINK], inline_state, false) ||
                     markdown_core_inline_state_in_bracket(self->peers[AUTOLINK_LINK], inline_state, true);

    if (c == ':') {
        /* No link forms inside a bracket, but the colon is still fenced off
         * there: `link_text_addresses` skips the text of a link, so
         * `[mailto:x@y.z](u)` keeps its plain text as cmark-gfm does, and a
         * bracket that never closes still gets its link. */
        markdown_core_member *node = in_bracket ? NULL : url_match(self, parser, parent, inline_state);
        return node || parser->error ? node : address_match(parser, inline_state);
    }

    if (c == 'w' && !in_bracket) {
        return www_match(self, parser, parent, inline_state);
    }

    return NULL;

    // note that we could end up re-consuming something already a
    // part of an inline, because we don't track when the last
    // inline was finished in inlines.c.
}

static bool validate_protocol(const char protocol[], uint8_t *data, size_t rewind, size_t max_rewind) {
    size_t len = strlen(protocol);

    if (rewind > max_rewind || len > (max_rewind - rewind)) {
        return false;
    }

    size_t prefix_len = rewind + len;

    // Check that the protocol matches
    if (memcmp(data - prefix_len, protocol, len) != 0) {
        return false;
    }

    if (prefix_len == max_rewind) {
        return true;
    }

    char prev_char = data[-(ptrdiff_t)(prefix_len + 1)];

    // Make sure the character before the protocol is non-alphanumeric
    return !markdown_core_isalnum(prev_char);
}

/* Construct only a nonempty fragment of an already recognized split. */
static markdown_core_node *email_text_fragment(markdown_core_parser *parser,
                                               const markdown_core_content_map *source_map,
                                               const markdown_core_chunk *source, size_t start, size_t length) {
    assert(length);
    markdown_core_node *text = markdown_core_parser_make_node(parser, MARKDOWN_CORE_NODE_TEXT);
    if (!text) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    markdown_core_chunk literal = markdown_core_chunk_dup(source, (bufsize_t)start, (bufsize_t)length);
    if (!markdown_core_chunk_to_cstr(&literal)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        markdown_core_parser_release_node(parser, text);
        return NULL;
    }
    *text->as.literal = literal;
    set_sourcepos_from_range(parser, text, source_map, start, length);
    return text;
}

/* Split `text` around every email address it holds: a Link (with its own Text)
 * and any prefix Text are attached BEFORE `text`, and `text` keeps the tail.
 * A Text that is nothing but addresses is freed once its splits are in place;
 * the return value says so, because the caller's event names a node that is
 * then gone. Sets parser->error on failure and leaves the tree consistent. */
static markdown_core_complete_result link_text_addresses(markdown_core_parser *parser, markdown_core_member *member) {
    markdown_core_node *text = member->node;
    size_t start = 0;
    size_t offset = 0;
    markdown_core_content_map source_map = text->content_map;
    /* The original Text owns the immutable source until every split is
     * committed. A failed search neither detaches nor copies its buffer. */
    markdown_core_chunk source = *text->as.literal;
    uint8_t *data = source.data;
    size_t remaining = source.len;

    while (true) {
        size_t link_end;
        uint8_t *at;
        bool auto_mailto = true;
        bool is_xmpp = false;
        size_t rewind;
        size_t max_rewind;
        size_t np = 0;

        if (offset >= remaining) {
            break;
        }

        at = (uint8_t *)memchr(data + start + offset, '@', remaining - offset);
        if (!at) {
            break;
        }

        max_rewind = at - (data + start + offset);

    found_at:
        for (rewind = 0; rewind < max_rewind; ++rewind) {
            uint8_t c = data[start + offset + max_rewind - rewind - 1];

            if (markdown_core_isalnum(c)) {
                continue;
            }

            if (strchr(".+-_", c) != NULL) {
                continue;
            }

            if (strchr(":", c) != NULL) {
                if (validate_protocol("mailto:", data + start + offset + max_rewind, rewind, max_rewind)) {
                    auto_mailto = false;
                    continue;
                }

                if (validate_protocol("xmpp:", data + start + offset + max_rewind, rewind, max_rewind)) {
                    auto_mailto = false;
                    is_xmpp = true;
                    continue;
                }
            }

            break;
        }

        if (rewind == 0) {
            offset += max_rewind + 1;
            continue;
        }

        assert(data[start + offset + max_rewind] == '@');
        for (link_end = 1; link_end < remaining - offset - max_rewind; ++link_end) {
            uint8_t c = data[start + offset + max_rewind + link_end];

            if (markdown_core_isalnum(c)) {
                continue;
            }

            if (c == '@') {
                // Found another '@', so go back and try again with an updated offset and
                // max_rewind.
                offset += max_rewind + 1;
                max_rewind = link_end - 1;
                goto found_at;
            } else if (c == '.' && link_end < remaining - offset - max_rewind - 1 &&
                       markdown_core_isalnum(data[start + offset + max_rewind + link_end + 1])) {
                np++;
            } else if (c == '/' && is_xmpp) {
                continue;
            } else if (c != '-' && c != '_') {
                break;
            }
        }

        if (link_end < 2 || np == 0 ||
            (!markdown_core_isalpha(data[start + offset + max_rewind + link_end - 1]) &&
             data[start + offset + max_rewind + link_end - 1] != '.')) {
            offset += max_rewind + link_end;
            continue;
        }

        link_end = autolink_delim(data + start + offset + max_rewind, link_end);

        if (link_end == 0) {
            offset += max_rewind + 1;
            continue;
        }

        /* Recognition alone cannot authorize a rewrite in an extension-owned
         * parent. Decide before allocating or splitting the original text. */
        size_t prefix_len = offset + max_rewind - rewind;
        if (!member->owner || !markdown_core_node_can_contain_type(member->owner->node, MARKDOWN_CORE_NODE_LINK) ||
            (prefix_len && !markdown_core_node_can_contain_type(member->owner->node, MARKDOWN_CORE_NODE_TEXT))) {
            break;
        }
        markdown_core_node *link_node = markdown_core_parser_make_node(parser, MARKDOWN_CORE_NODE_LINK);
        if (!link_node) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            break;
        }
        size_t prefix_start = start;
        size_t link_start = start + offset + max_rewind - rewind;
        size_t link_len = link_end + rewind;
        size_t post_start = start + offset + max_rewind + link_end;
        markdown_core_strbuf buf;
        markdown_core_strbuf_init(&buf, 10);
        if (auto_mailto) {
            markdown_core_strbuf_puts(&buf, "mailto:");
        }
        markdown_core_strbuf_put(&buf, data + start + offset + max_rewind - rewind, (bufsize_t)(link_end + rewind));
        {
            markdown_core_chunk url = markdown_core_chunk_buf_detach(&buf);
            link_node->as.link->resource =
                url.data ? markdown_core_resource_new(parser->pool, url, markdown_core_optional_chunk_absent()) : NULL;
            if (!link_node->as.link->resource) {
                markdown_core_chunk_free(&url);
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
                markdown_core_parser_release_node(parser, link_node);
                break;
            }
        }
        set_sourcepos_from_range(parser, link_node, &source_map, link_start, link_len);

        markdown_core_node *link_text = email_text_fragment(parser, &source_map, &source, link_start, link_len);
        if (!link_text) {
            markdown_core_parser_release_node(parser, link_node);
            break;
        }
        if (prefix_len) {
            markdown_core_node *prefix = email_text_fragment(parser, &source_map, &source, prefix_start, prefix_len);
            if (!prefix) {
                markdown_core_parser_release_node(parser, link_text);
                markdown_core_parser_release_node(parser, link_node);
                break;
            }
            if (!markdown_core_parser_attach(parser, member->owner, prefix, member)) {
                markdown_core_parser_release_node(parser, link_text);
                markdown_core_parser_release_node(parser, link_node);
                break;
            }
        }
        markdown_core_member *link = markdown_core_parser_attach(parser, member->owner, link_node, member);
        if (!link) {
            markdown_core_parser_release_node(parser, link_text);
            break;
        }
        if (!markdown_core_parser_attach(parser, link, link_text, NULL)) {
            break;
        }
        /* The pass has left the place it is inserted at: the link completes
         * here. */
        markdown_core_parser_complete_node(parser, link);
        start = post_start;
        remaining = source.len - start;
        offset = 0;
    }

    if (parser->error) {
        return MARKDOWN_CORE_COMPLETE_FAILED;
    }
    if (!start) {
        return MARKDOWN_CORE_COMPLETE_CONTINUE;
    }
    if (!remaining) {
        markdown_core_parser_release_member(parser, member);
        return MARKDOWN_CORE_COMPLETE_CONSUMED;
    }
    markdown_core_chunk tail = markdown_core_chunk_dup(&source, (bufsize_t)start, (bufsize_t)remaining);
    if (!markdown_core_chunk_to_cstr(&tail)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return MARKDOWN_CORE_COMPLETE_FAILED;
    }
    set_sourcepos_from_range(parser, text, &source_map, start, remaining);
    *text->as.literal = tail;
    markdown_core_chunk_free(&source);
    return MARKDOWN_CORE_COMPLETE_CONTINUE;
}

/* The email scan is a completion STEP: it is asked, from inside an inline
 * root's completion, at a Text's EXIT (the kind it acts on) and at a Link's
 * ENTER and EXIT (the kind whose extent it tracks). The Link events set and
 * clear the per-root state word -- a Text inside a Link is never scanned, an
 * address there is already a link's text -- and a Text's EXIT outside a Link
 * is scanned. The pass has already consolidated that Text with the siblings
 * that followed it when this is asked, so the scan sees the whole run, and
 * the EXIT's lookahead already names the following survivor, so the splits
 * inserted before the Text are never visited, the links among them complete
 * as they are inserted, and the Text itself may be freed. */
static markdown_core_complete_result complete_step(const markdown_core_element_instance *self,
                                                   markdown_core_parser *parser, markdown_core_member *member,
                                                   markdown_core_event_type event, int is_root, void **state) {
    (void)self;
    (void)is_root;
    if (member->node->kind == MARKDOWN_CORE_NODE_LINK) {
        *state = event == MARKDOWN_CORE_EVENT_ENTER ? member : NULL;
        return MARKDOWN_CORE_COMPLETE_CONTINUE;
    }
    assert(event == MARKDOWN_CORE_EVENT_EXIT);
    if (*state) {
        return MARKDOWN_CORE_COMPLETE_CONTINUE;
    }
    return link_text_addresses(parser, member);
}

static const markdown_core_node_type AUTOLINK_TEXT_KINDS[] = {MARKDOWN_CORE_NODE_TEXT, MARKDOWN_CORE_NODE_NONE};
static const markdown_core_node_type AUTOLINK_SCOPE_KINDS[] = {MARKDOWN_CORE_NODE_LINK, MARKDOWN_CORE_NODE_NONE};

const markdown_core_element MARKDOWN_CORE_ELEMENT_AUTOLINK = {
    .peers = AUTOLINK_PEERS,
    .name = "autolink",
    .state_size = sizeof(markdown_core_autolink_work),
    .run_state_size = sizeof(autolink_run),
    .match_inline = match,
    .complete_step = complete_step,
    /* The step rewrites a Text -- the kind it acts on and the kind it is asked
     * at are the same -- and reads a Link's ENTER and EXIT to know when a
     * Text is inside one. */
    .complete_acts_on_kinds = AUTOLINK_TEXT_KINDS,
    .complete_exit_kinds = AUTOLINK_TEXT_KINDS,
    .complete_scope_kinds = AUTOLINK_SCOPE_KINDS,
    .terminates_text = "<:w",
    .dispatch = "<:w",
};
