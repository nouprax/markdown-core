#include "inline_internal.h"
#include "autolink.h"
#include "services.h"
#include "label.h"
#include "label.h"
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

/* The services this element asks, as `self->needs` holds them. */
enum { AUTOLINK_REFERENCES };
static const char *const AUTOLINK_NEEDS[] = {[AUTOLINK_REFERENCES] = MARKDOWN_CORE_SERVICE_REFERENCES, NULL};

static markdown_core_node *make_str_with_entities(const markdown_core_element_instance *self,
                                                  markdown_core_inline_state *inline_state, int start_column,
                                                  int end_column, markdown_core_chunk *content) {
    markdown_core_strbuf unescaped = MARKDOWN_CORE_BUF_INIT();

    if (markdown_core_decoder_decode(self->needs[AUTOLINK_REFERENCES], &unescaped, content->data, content->len)) {
        if (unescaped.oom) {
            inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        }
        return make_str(inline_state, start_column, end_column, markdown_core_chunk_buf_detach(&unescaped));
    } else {
        return make_str(inline_state, start_column, end_column, *content);
    }
}

static markdown_core_chunk markdown_core_clean_autolink(const markdown_core_element_instance *self,
                                                        markdown_core_inline_state *inline_state,
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

    markdown_core_decoder_write(self->needs[AUTOLINK_REFERENCES], &buf, url->data, url->len);
    if (buf.oom) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
    }
    return markdown_core_chunk_buf_detach(&buf);
}

static MARKDOWN_CORE_INLINE markdown_core_node *make_autolink(const markdown_core_element_instance *self,
                                                              markdown_core_inline_state *inline_state,
                                                              int start_column, int end_column, markdown_core_chunk url,
                                                              int is_email) {
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
        markdown_core_chunk destination = markdown_core_clean_autolink(self, inline_state, &url, is_email);
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
    const markdown_core_field_to to = markdown_core_inline_fields(inline_state, link);
    markdown_core_field_put(&to, MARKDOWN_CORE_SLOT_DEST, MARKDOWN_CORE_FIELD_BARE, start_column + 1, end_column,
                            false);
    text = make_str_with_entities(self, inline_state, start_column + 1, end_column - 1, &url);
    if (text) {
        markdown_core_scaffold_append(link, text);
    }
    /* The pointy braces are the syntax; what they enclose is the text. */
    return link;
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

static size_t autolink_delim(const uint8_t *data, size_t link_end) {
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

/* What an autolink scan reads: the run's bytes from `base`, its record and
 * the element's counts, the byte that closes the innermost label (0 for
 * none), and whether it read the end. */
typedef struct {
    const uint8_t *base;
    autolink_run *run;
    markdown_core_autolink_work *counts;
    unsigned char closer;
    bool end;
} S_scan;

static size_t S_domain(S_scan *scan, const uint8_t *data, size_t size, int allow_short) {
    size_t i, np = 0, uscore1 = 0, uscore2 = 0, last_underscore = 0;
    bufsize_t start = (bufsize_t)(data - scan->base);
    autolink_run *run = scan->run;
    markdown_core_autolink_work *counts = scan->counts;
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

    /* The domain may go on with the input. */
    if (i >= size - 1) {
        scan->end = true;
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
    node->place = (markdown_core_place){0, 0};
    if (!len) {
        return;
    }
    markdown_core_text_place(parser, source, (bufsize_t)start, (bufsize_t)(start + len), &node->place);
    if (node->kind == MARKDOWN_CORE_NODE_TEXT) {
        markdown_core_text_share(parser, source, &node->content_map, (bufsize_t)start, (bufsize_t)len);
    }
}

/* URL and www candidates use the same extent rule. Bracket bodies supply
 * their delimiter; escaped punctuation stays in the opaque token. Each byte
 * is visited once, including tokens that end at a footnote's closing ]. */
static size_t S_extent(S_scan *scan, const uint8_t *data, size_t size, size_t offset) {
    const unsigned char closer = scan->closer;
    while (offset < size && !markdown_core_is_whitespace(data[offset]) && data[offset] != '<') {
        if (closer && data[offset] == closer) {
            break;
        }
        if (data[offset] == '\\' && offset + 1 < size && markdown_core_ispunct(data[offset + 1]) &&
            data[offset + 1] != '<') {
            offset++;
        }
        offset++;
    }
    if (offset >= size) {
        scan->end = true;
    }
    return offset;
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
/* `text` is a child of `parent`, whose children the walk has not settled
 * yet: the splits are linked before it in their scaffold, each made whole. */
static markdown_core_finish_result postprocess_text(markdown_core_parser *parser, markdown_core_node *text,
                                                    markdown_core_node *parent) {
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

            if (c && strchr(".+-_", c) != NULL) {
                continue;
            }

            if (c == ':') {
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
        if (!parent || !markdown_core_node_can_contain_type(parent, MARKDOWN_CORE_NODE_LINK) ||
            (prefix_len && !markdown_core_node_can_contain_type(parent, MARKDOWN_CORE_NODE_TEXT))) {
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
        /* All of the address is the link's destination. */
        if (!markdown_core_node_add_field(parser->pool, link_node, MARKDOWN_CORE_SLOT_DEST, MARKDOWN_CORE_FIELD_BARE,
                                          link_node->place.start, link_node->place.end, false)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }

        markdown_core_node *link_text = email_text_fragment(parser, &source_map, &source, link_start, link_len);
        if (!link_text) {
            markdown_core_parser_release_node(parser, link_node);
            break;
        }
        if (!markdown_core_node_push_child(parser->pool, link_node, link_text)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            markdown_core_parser_release_node(parser, link_text);
            markdown_core_parser_release_node(parser, link_node);
            break;
        }
        if (prefix_len) {
            markdown_core_node *prefix = email_text_fragment(parser, &source_map, &source, prefix_start, prefix_len);
            if (!prefix) {
                markdown_core_parser_release_node(parser, link_node);
                break;
            }
            markdown_core_scaffold_insert_before(text, prefix);
        }
        markdown_core_scaffold_insert_before(text, link_node);
        start = post_start;
        remaining = source.len - start;
        offset = 0;
    }

    if (parser->error) {
        return MARKDOWN_CORE_FINISH_FAILED;
    }
    if (!start) {
        return MARKDOWN_CORE_FINISH_CONTINUE;
    }
    if (!remaining) {
        markdown_core_parser_release_node(parser, text);
        return MARKDOWN_CORE_FINISH_CONSUMED;
    }
    markdown_core_chunk tail = markdown_core_chunk_dup(&source, (bufsize_t)start, (bufsize_t)remaining);
    if (!markdown_core_chunk_to_cstr(&tail)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return MARKDOWN_CORE_FINISH_FAILED;
    }
    set_sourcepos_from_range(parser, text, &source_map, start, remaining);
    *text->as.literal = tail;
    markdown_core_chunk_free(&source);
    return MARKDOWN_CORE_FINISH_CONTINUE;
}

/* The email scan is a finish STEP: it is asked, from inside the one finish
 * walk, at a Text's EXIT (the kind it acts on) and at a Link's ENTER and EXIT
 * (the kind whose extent it tracks). The Link events set and clear the
 * per-root state word -- a Text inside a Link is never scanned, an address
 * there is already a link's text -- and a Text's EXIT outside a Link is
 * scanned. The walk has already consolidated that Text with the siblings that
 * followed it when this is asked, so the scan sees the whole run, and the
 * EXIT's lookahead already names the following survivor, so the splits
 * inserted before the Text are never visited and the Text itself may be
 * freed. */
static markdown_core_finish_result finish_step(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                               markdown_core_node *node, markdown_core_node *parent,
                                               markdown_core_event_type event, void **state) {
    (void)self;
    if (node->kind == MARKDOWN_CORE_NODE_LINK) {
        *state = event == MARKDOWN_CORE_EVENT_ENTER ? node : NULL;
        return MARKDOWN_CORE_FINISH_CONTINUE;
    }
    assert(event == MARKDOWN_CORE_EVENT_EXIT);
    if (*state) {
        return MARKDOWN_CORE_FINISH_CONTINUE;
    }
    return postprocess_text(parser, node, parent);
}

static const markdown_core_node_type AUTOLINK_FINISH_KINDS[] = {MARKDOWN_CORE_NODE_TEXT, MARKDOWN_CORE_NODE_NONE};
static const markdown_core_node_type AUTOLINK_SCOPE_KINDS[] = {MARKDOWN_CORE_NODE_LINK, MARKDOWN_CORE_NODE_NONE};

static const markdown_core_finish_ops AUTOLINK_FINISH = {
    .step = finish_step,
    /* The step rewrites a Text -- the kind it acts on and the kind it is asked
     * at are the same -- and reads a Link's ENTER and EXIT to know when a
     * Text is inside one. */
    .acts = AUTOLINK_FINISH_KINDS,
    .exits = AUTOLINK_FINISH_KINDS,
    .scopes = AUTOLINK_SCOPE_KINDS,
};

/* THE AUTOLINKS (plan symbols-cleanroom.md 23): `<` and a URI or an
 * address and `>`, one token (`data` 1 for an address); at a `:`, a URL
 * whose scheme is the letters before the colon (`data`, which the
 * construct's resolve takes off the data before it), and otherwise the
 * colon of a bare address fenced off as text; at a `w`, a `www.` link.
 * Inside a label's body neither a URL nor a `www.` link forms. */
enum { AUTOLINK_ANGLE, AUTOLINK_URL, AUTOLINK_WWW, AUTOLINK_COLON, AUTOLINK_KINDS };

static markdown_core_state S_token(markdown_core_tokenizer *tokenizer, unsigned kind, uint32_t end, uint32_t data) {
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    const uint32_t token = markdown_core_enter(tokenizer, (uint16_t)(self->token_base + kind));
    if (token != UINT32_MAX) {
        tokenizer->events.tokens[token].data = data;
    }
    markdown_core_consume_to(tokenizer, end);
    markdown_core_exit(tokenizer, (uint16_t)(self->token_base + kind));
    return markdown_core_ok(tokenizer);
}

static S_scan S_scan_of(markdown_core_tokenizer *tokenizer) {
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    S_scan scan = {tokenizer->data, markdown_core_read_record(tokenizer, self->run_offset), self->state,
                   markdown_core_label_closer(tokenizer), false};
    return scan;
}

/* THE ANGLE AUTOLINK, code by code (micromark's autolink): `<`, then a
 * URI, a scheme of 2 to 32 letters, digits, `+`, `-` and `.` that a
 * letter starts, `:` and bytes that are no control, space, `<` or `>`; or
 * an address, a local part, `@` and labels of at most 63 letters, digits
 * and `-` that start and end with a letter or digit, joined by `.`; and
 * `>`. The frame's words: the token, a count, and whether the last byte
 * of a label was `-`. */
enum { ANGLE_TOKEN, ANGLE_COUNT, ANGLE_DASH };

static markdown_core_state S_uri_body(markdown_core_tokenizer *tokenizer, int code) {
    if (code == MARKDOWN_CORE_EOF || code <= 0x20 || code == 0x7F || code == '<') {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_consume(tokenizer);
    return code == '>' ? markdown_core_ok(tokenizer) : markdown_core_state_of(S_uri_body);
}

static markdown_core_state S_scheme(markdown_core_tokenizer *tokenizer, int code) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    if (code == ':' && words[ANGLE_COUNT] >= 2) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_uri_body);
    }
    if (code >= 0 && code < 0x80 && (markdown_core_isalnum((char)code) || code == '+' || code == '-' || code == '.') &&
        words[ANGLE_COUNT] < 32) {
        words[ANGLE_COUNT]++;
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_scheme);
    }
    return markdown_core_nok(tokenizer);
}

static markdown_core_state S_uri(markdown_core_tokenizer *tokenizer, int code) {
    if (code < 0 || code >= 0x80 || !markdown_core_isalpha((char)code)) {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_frame_local(tokenizer)[ANGLE_COUNT] = 1;
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_scheme);
}

static const markdown_core_construct ANGLE_URI = {.start = S_uri};

static inline bool S_alnum(int code) { return code >= 0 && code < 0x80 && markdown_core_isalnum((char)code); }

static markdown_core_state S_host(markdown_core_tokenizer *tokenizer, int code);

static markdown_core_state S_host_label(markdown_core_tokenizer *tokenizer, int code) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    if ((S_alnum(code) || code == '-') && words[ANGLE_COUNT] < 63) {
        words[ANGLE_COUNT]++;
        words[ANGLE_DASH] = code == '-';
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_host_label);
    }
    if ((code == '.' || code == '>') && !words[ANGLE_DASH]) {
        markdown_core_consume(tokenizer);
        return code == '>' ? markdown_core_ok(tokenizer) : markdown_core_state_of(S_host);
    }
    return markdown_core_nok(tokenizer);
}

static markdown_core_state S_host(markdown_core_tokenizer *tokenizer, int code) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    if (!S_alnum(code)) {
        return markdown_core_nok(tokenizer);
    }
    words[ANGLE_COUNT] = 1;
    words[ANGLE_DASH] = false;
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_host_label);
}

static inline bool S_atext(int code) {
    return S_alnum(code) || (code > ' ' && code < 0x7F && strchr(".!#$%&'*+/=?^_`{|}~-", code));
}

static markdown_core_state S_local(markdown_core_tokenizer *tokenizer, int code) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    if (S_atext(code)) {
        words[ANGLE_COUNT]++;
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_local);
    }
    if (code == '@' && words[ANGLE_COUNT]) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_host);
    }
    return markdown_core_nok(tokenizer);
}

static const markdown_core_construct ANGLE_ADDRESS = {.start = S_local};

/* The autolink read through its `>`, an address when `email`. */
static markdown_core_state S_angled(markdown_core_tokenizer *tokenizer, bool email) {
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    const uint32_t token = markdown_core_frame_local(tokenizer)[ANGLE_TOKEN];
    if (token != UINT32_MAX) {
        tokenizer->events.tokens[token].data = email;
    }
    markdown_core_exit(tokenizer, (uint16_t)(self->token_base + AUTOLINK_ANGLE));
    /* What follows an autolink may update it (no-flush.md 10.13). */
    markdown_core_text_made(tokenizer, MARKDOWN_CORE_NODE_LINK);
    return markdown_core_ok(tokenizer);
}

static markdown_core_state S_angle_uri(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return S_angled(tokenizer, false);
}

static markdown_core_state S_angle_address(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return S_angled(tokenizer, true);
}

/* Neither: either may still be read once the input goes on, unless the
 * byte after `<` starts neither a scheme nor a local part; its `<` opens
 * an autolink or HTML once the text goes on. */
static markdown_core_state S_angle_none(markdown_core_tokenizer *tokenizer, int code) {
    if (code == MARKDOWN_CORE_EOF || S_atext(code)) {
        const bufsize_t from = (bufsize_t)tokenizer->position;
        markdown_core_met_end(tokenizer);
        markdown_core_text_give_up(tokenizer, MARKDOWN_CORE_KIND_NONE, from - 1, from, NULL, 0);
    }
    return markdown_core_nok(tokenizer);
}

static markdown_core_state S_angle_next(markdown_core_tokenizer *tokenizer, int code) {
    return markdown_core_attempt(tokenizer, &ANGLE_ADDRESS, markdown_core_state_of(S_angle_address),
                                 markdown_core_state_of(S_angle_none), code);
}

static markdown_core_state S_angle_open(markdown_core_tokenizer *tokenizer, int code) {
    return markdown_core_attempt(tokenizer, &ANGLE_URI, markdown_core_state_of(S_angle_uri),
                                 markdown_core_state_of(S_angle_next), code);
}

static markdown_core_state S_angle(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    markdown_core_frame_local(tokenizer)[ANGLE_TOKEN] =
        markdown_core_enter(tokenizer, (uint16_t)(self->token_base + AUTOLINK_ANGLE));
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_angle_open);
}

static markdown_core_state S_url(markdown_core_tokenizer *tokenizer) {
    const uint32_t at = tokenizer->position;
    const uint8_t *data = tokenizer->data + at;
    const size_t size = tokenizer->size - at;
    if (size < 4 || data[1] != '/' || data[2] != '/') {
        if (size < 4 && (size < 2 || data[1] == '/') && (size < 3 || data[2] == '/')) {
            markdown_core_met_end(tokenizer);
        }
        return markdown_core_nok(tokenizer);
    }
    uint32_t rewind = 0;
    while (rewind < at && markdown_core_isalpha(data[-(ptrdiff_t)rewind - 1])) {
        rewind++;
    }
    if (!sd_autolink_issafe(data - rewind, size + rewind)) {
        return markdown_core_nok(tokenizer);
    }
    S_scan scan = S_scan_of(tokenizer);
    size_t link_end = strlen("://");
    const size_t domain = S_domain(&scan, data + link_end, size - link_end, 1);
    if (domain) {
        link_end = autolink_delim(data, S_extent(&scan, data, size, link_end + domain));
    }
    if (scan.end) {
        markdown_core_met_end(tokenizer);
    }
    if (!domain || !link_end) {
        return markdown_core_nok(tokenizer);
    }
    return S_token(tokenizer, AUTOLINK_URL, at + (uint32_t)link_end, rewind);
}

/* The colon of a bare address, `x:a@b.c`, which the autolink takes as one
 * opaque byte of text (address_match). */
static markdown_core_state S_address(markdown_core_tokenizer *tokenizer) {
    const uint8_t *data = tokenizer->data + tokenizer->position;
    const size_t size = tokenizer->size - tokenizer->position;
    size_t at, end, np = 0;
    for (at = 1; at < size && (markdown_core_isalnum(data[at]) || (data[at] && strchr(".+-_", data[at]) != NULL));
         at++) {
    }
    if (at >= size) {
        markdown_core_met_end(tokenizer);
    }
    if (at == 1 || at >= size || data[at] != '@') {
        return markdown_core_nok(tokenizer);
    }
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
    if (end + 1 >= size) {
        markdown_core_met_end(tokenizer);
    }
    if (end - at < 2 || np == 0 || (!markdown_core_isalpha(data[end - 1]) && data[end - 1] != '.')) {
        return markdown_core_nok(tokenizer);
    }
    return S_token(tokenizer, AUTOLINK_COLON, tokenizer->position + 1, 0);
}

static markdown_core_state S_after_url(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return markdown_core_ok(tokenizer);
}
static markdown_core_state S_no_url(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return S_address(tokenizer);
}
static markdown_core_state S_url_start(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return S_url(tokenizer);
}
/* The scheme's letters, already data, are the URL's. */
static void S_url_resolve(markdown_core_tokenizer *tokenizer, size_t from) {
    markdown_core_events *events = &tokenizer->events;
    markdown_core_token *link = markdown_core_event_token(events, from);
    uint32_t rewind = link->data;
    link->start -= rewind;
    for (size_t at = from; rewind && at >= 2;) {
        markdown_core_token *before = markdown_core_event_token(events, at - 1);
        if (before->kind != MARKDOWN_CORE_TOKEN_DATA || events->items[at - 1].phase != MARKDOWN_CORE_EXIT) {
            break;
        }
        const uint32_t take = before->end - before->start < rewind ? before->end - before->start : rewind;
        before->end -= take;
        rewind -= take;
        if (before->end == before->start) {
            /* The data is gone: its two events go with it. */
            markdown_core_events_splice(events, at - 2, 2, NULL, 0);
            at -= 2;
        } else {
            break;
        }
    }
}
static const markdown_core_construct S_URL = {.start = S_url_start, .resolve = S_url_resolve};

static markdown_core_state S_colon(markdown_core_tokenizer *tokenizer, int code) {
    if (markdown_core_label_in(tokenizer, 0) || markdown_core_label_in(tokenizer, 1)) {
        return S_address(tokenizer);
    }
    return markdown_core_attempt(tokenizer, &S_URL, markdown_core_state_of(S_after_url),
                                 markdown_core_state_of(S_no_url), code);
}

static markdown_core_state S_www(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    if (markdown_core_label_in(tokenizer, 0) || markdown_core_label_in(tokenizer, 1)) {
        return markdown_core_nok(tokenizer);
    }
    const uint32_t at = tokenizer->position;
    const uint8_t *data = tokenizer->data + at;
    const size_t size = tokenizer->size - at;
    if (at > markdown_core_label_body(tokenizer) && (!data[-1] || strchr("*_~(", data[-1]) == NULL) &&
        !markdown_core_is_whitespace(data[-1])) {
        return markdown_core_nok(tokenizer);
    }
    if (size < 4 || memcmp(data, "www.", strlen("www.")) != 0) {
        if (size < 4 && memcmp(data, "www.", size) == 0) {
            markdown_core_met_end(tokenizer);
        }
        return markdown_core_nok(tokenizer);
    }
    S_scan scan = S_scan_of(tokenizer);
    size_t link_end = S_domain(&scan, data, size, 0);
    if (link_end) {
        link_end = autolink_delim(data, S_extent(&scan, data, size, link_end));
    }
    if (scan.end) {
        markdown_core_met_end(tokenizer);
    }
    if (!link_end) {
        return markdown_core_nok(tokenizer);
    }
    return S_token(tokenizer, AUTOLINK_WWW, at + (uint32_t)link_end, 0);
}

static const markdown_core_text_construct AUTOLINK_CONSTRUCTS[] = {
    {"<", {.start = S_angle}}, {":", {.start = S_colon}}, {"w", {.start = S_www}}};

/* A literal link's Link and its Text, of the content bytes `from` up to
 * `to`, its destination `url`. */
static void S_literal(markdown_core_compiler *compiler, bufsize_t from, bufsize_t to, markdown_core_chunk url,
                      markdown_core_chunk text_literal) {
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    markdown_core_parser *parser = inline_state->owner_parser;
    markdown_core_node *node = markdown_core_parser_make_node(parser, MARKDOWN_CORE_NODE_LINK);
    if (!node) {
        markdown_core_chunk_free(&url);
        markdown_core_chunk_free(&text_literal);
        markdown_core_compile_append(compiler, NULL);
        return;
    }
    node->as.link->resource =
        url.data ? markdown_core_resource_new(parser->pool, url, markdown_core_optional_chunk_absent()) : NULL;
    if (!node->as.link->resource) {
        markdown_core_chunk_free(&url);
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
    }
    markdown_core_node *text = markdown_core_parser_make_node(parser, MARKDOWN_CORE_NODE_TEXT);
    if (!text) {
        markdown_core_chunk_free(&text_literal);
        markdown_core_parser_release_node(parser, node);
        markdown_core_compile_append(compiler, NULL);
        return;
    }
    *text->as.literal = text_literal;
    markdown_core_scaffold_append(node, text);
    markdown_core_inline_state_place(inline_state, node, from, to - 1);
    markdown_core_inline_state_place(inline_state, text, from, to - 1);
    const markdown_core_field_to fields = markdown_core_inline_fields(inline_state, node);
    markdown_core_field_put(&fields, MARKDOWN_CORE_SLOT_DEST, MARKDOWN_CORE_FIELD_BARE, from, to, false);
    markdown_core_compile_append(compiler, node);
}

static void S_made(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    const markdown_core_element_instance *self = markdown_core_compile_self(compiler);
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    const bufsize_t start = (bufsize_t)token->start, end = (bufsize_t)token->end;
    switch (token->kind - self->token_base) {
    case AUTOLINK_ANGLE: {
        markdown_core_chunk content = markdown_core_inline_take(inline_state, start + 1, end - start - 2);
        markdown_core_inline_seek(inline_state, end);
        markdown_core_compile_append(compiler,
                                     make_autolink(self, inline_state, start, end - 1, content, (int)token->data));
        break;
    }
    case AUTOLINK_URL: {
        markdown_core_chunk url = markdown_core_inline_take(inline_state, start, end - start);
        S_literal(compiler, start, end, url, url);
        break;
    }
    case AUTOLINK_WWW: {
        markdown_core_strbuf buf;
        markdown_core_strbuf_init(&buf, 10);
        markdown_core_strbuf_puts(&buf, "http://");
        markdown_core_strbuf_put(&buf, markdown_core_inline_window(inline_state, 0).data + start, end - start);
        S_literal(compiler, start, end, markdown_core_chunk_buf_detach(&buf),
                  markdown_core_inline_take(inline_state, start, end - start));
        break;
    }
    default:
        markdown_core_compile_text(compiler, start, start + 1);
        break;
    }
}
static const markdown_core_compile_ops AUTOLINK_TOKENS[AUTOLINK_KINDS] = {
    [AUTOLINK_ANGLE] = {.exit = S_made},
    [AUTOLINK_URL] = {.exit = S_made},
    [AUTOLINK_WWW] = {.exit = S_made},
    [AUTOLINK_COLON] = {.exit = S_made, .plain = true}};

static const markdown_core_inline_ops AUTOLINK_INLINES = {
    .constructs = AUTOLINK_CONSTRUCTS,
    .construct_count = 3,
    .tokens = AUTOLINK_TOKENS,
    .token_count = AUTOLINK_KINDS,
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_AUTOLINK = {
    .needs = AUTOLINK_NEEDS,
    .name = "autolink",
    .state_size = sizeof(markdown_core_autolink_work),
    .run_state_size = sizeof(autolink_run),
    .finish = &AUTOLINK_FINISH,
    .inlines = &AUTOLINK_INLINES,
};
