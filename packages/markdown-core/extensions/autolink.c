#include "autolink.h"
#include "extension.h"
#include <iterator.h>
#include <node.h>
#include <parser.h>
#include <string.h>
#include <utf8.h>
#include <stddef.h>

#if defined(_WIN32)
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

static int is_valid_hostchar(const uint8_t *link, size_t link_len) {
    int32_t ch;
    int r = markdown_core_utf8proc_iterate(link, (bufsize_t)link_len, &ch);
    if (r < 0) {
        return 0;
    }
    return !markdown_core_utf8proc_is_space(ch) && !markdown_core_utf8proc_is_punctuation(ch);
}

static int sd_autolink_issafe(const uint8_t *link, size_t link_len) {
    static const size_t valid_uris_count = 3;
    static const char *valid_uris[] = {"http://", "https://", "ftp://"};

    size_t i;

    for (i = 0; i < valid_uris_count; ++i) {
        size_t len = strlen(valid_uris[i]);

        if (link_len > len && strncasecmp((char *)link, valid_uris[i], len) == 0 &&
            is_valid_hostchar(link + len, link_len - len)) {
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

static size_t check_domain(uint8_t *data, size_t size, int allow_short) {
    size_t i, np = 0, uscore1 = 0, uscore2 = 0;

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
     */
    for (i = 1; i < size - 1; i++) {
        if (data[i] == '\\' && i < size - 2) {
            i++;
        }
        if (data[i] == '_') {
            uscore2++;
        } else if (data[i] == '.') {
            uscore1 = uscore2;
            uscore2 = 0;
            np++;
        } else if (!is_valid_hostchar(data + i, size - i) && data[i] != '-') {
            break;
        }
    }

    if (uscore1 > 0 || uscore2 > 0) {
        /* If the url is very long then accept it despite the underscores,
         * to avoid quadratic behavior causing a denial of service. See:
         * https://github.com/advisories/GHSA-29g3-96g3-jg6c
         * Reasonable urls are unlikely to have more than 10 segments, so
         * this extra condition shouldn't have any impact on normal usage.
         */
        if (np <= 10) {
            return 0;
        }
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

static void clear_sourcepos(markdown_core_node *node) {
    node->start_line = 0;
    node->start_column = 0;
    node->end_line = 0;
    node->end_column = 0;
}

static void set_sourcepos_from_range(markdown_core_node *node, int source_start_line, int source_start_column,
                                     markdown_core_chunk *source, size_t start, size_t len) {
    clear_sourcepos(node);

    if (source_start_line == 0 || len == 0) {
        return;
    }

    int line = source_start_line;
    int column = source_start_column;
    for (size_t i = 0; i < start; i++) {
        if (source->data[i] == '\n') {
            line++;
            column = 1;
        } else {
            column++;
        }
    }

    node->start_line = line;
    node->start_column = column;

    int end_line = line;
    int end_column = column - 1;
    for (size_t i = start; i < start + len; i++) {
        if (source->data[i] == '\n') {
            end_line++;
            end_column = 0;
        } else {
            end_column++;
        }
    }

    node->end_line = end_line;
    node->end_column = end_column;
}

static markdown_core_node *www_match(markdown_core_parser *parser, markdown_core_node *parent,
                                     markdown_core_inline_parser *inline_parser) {
    markdown_core_chunk *chunk = markdown_core_inline_parser_get_chunk(inline_parser);
    size_t max_rewind = markdown_core_inline_parser_get_offset(inline_parser);
    uint8_t *data = chunk->data + max_rewind;
    size_t size = chunk->len - max_rewind;
    int start = markdown_core_inline_parser_get_column(inline_parser);

    size_t link_end;

    if (max_rewind > 0 && strchr("*_~(", data[-1]) == NULL && !markdown_core_isspace(data[-1])) {
        return 0;
    }

    if (size < 4 || memcmp(data, "www.", strlen("www.")) != 0) {
        return 0;
    }

    link_end = check_domain(data, size, 0);

    if (link_end == 0) {
        return NULL;
    }

    while (link_end < size && !markdown_core_isspace(data[link_end]) && data[link_end] != '<') {
        link_end++;
    }

    link_end = autolink_delim(data, link_end);

    if (link_end == 0) {
        return NULL;
    }

    markdown_core_inline_parser_set_offset(inline_parser, (int)(max_rewind + link_end));

    markdown_core_node *node = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_LINK, parser->mem);
    if (!node) {
        parser->oom = true;
        return NULL;
    }

    markdown_core_strbuf buf;
    markdown_core_strbuf_init(parser->mem, &buf, 10);
    markdown_core_strbuf_puts(&buf, "http://");
    markdown_core_strbuf_put(&buf, data, (bufsize_t)link_end);
    node->as.link.url = markdown_core_chunk_buf_detach(&buf);
    if (!node->as.link.url.data) {
        parser->oom = true;
    }

    markdown_core_node *text = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_TEXT, parser->mem);
    if (!text) {
        parser->oom = true;
        markdown_core_node_free(node);
        return NULL;
    }
    text->as.literal = markdown_core_chunk_dup(chunk, (bufsize_t)max_rewind, (bufsize_t)link_end);
    markdown_core_node_append_child(node, text);

    node->start_line = text->start_line = node->end_line = text->end_line =
        markdown_core_inline_parser_get_line(inline_parser);

    node->start_column = text->start_column = start;
    node->end_column = text->end_column = markdown_core_inline_parser_get_column(inline_parser) - 1;

    return node;
}

static markdown_core_node *url_match(markdown_core_parser *parser, markdown_core_node *parent,
                                     markdown_core_inline_parser *inline_parser) {
    size_t link_end, domain_len;
    int rewind = 0;

    markdown_core_chunk *chunk = markdown_core_inline_parser_get_chunk(inline_parser);
    int max_rewind = markdown_core_inline_parser_get_offset(inline_parser);
    uint8_t *data = chunk->data + max_rewind;
    size_t size = chunk->len - max_rewind;
    int start_column;

    if (size < 4 || data[1] != '/' || data[2] != '/') {
        return 0;
    }

    while (rewind < max_rewind && markdown_core_isalpha(data[-rewind - 1])) {
        rewind++;
    }
    start_column = markdown_core_inline_parser_get_column(inline_parser) - rewind;

    if (!sd_autolink_issafe(data - rewind, size + rewind)) {
        return 0;
    }

    link_end = strlen("://");

    domain_len = check_domain(data + link_end, size - link_end, 1);

    if (domain_len == 0) {
        return 0;
    }

    link_end += domain_len;
    while (link_end < size && !markdown_core_isspace(data[link_end]) && data[link_end] != '<') {
        link_end++;
    }

    link_end = autolink_delim(data, link_end);

    if (link_end == 0) {
        return NULL;
    }

    markdown_core_inline_parser_set_offset(inline_parser, (int)(max_rewind + link_end));
    markdown_core_node_unput(parent, rewind);

    markdown_core_node *node = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_LINK, parser->mem);
    if (!node) {
        parser->oom = true;
        return NULL;
    }

    markdown_core_chunk url = markdown_core_chunk_dup(chunk, max_rewind - rewind, (bufsize_t)(link_end + rewind));
    node->as.link.url = url;

    markdown_core_node *text = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_TEXT, parser->mem);
    if (!text) {
        parser->oom = true;
        markdown_core_node_free(node);
        return NULL;
    }
    text->as.literal = url;
    markdown_core_node_append_child(node, text);

    node->start_line = text->start_line = node->end_line = text->end_line =
        markdown_core_inline_parser_get_line(inline_parser);

    node->start_column = text->start_column = start_column;
    node->end_column = text->end_column = markdown_core_inline_parser_get_column(inline_parser) - 1;

    return node;
}

static markdown_core_node *match(markdown_core_extension *ext, markdown_core_parser *parser, markdown_core_node *parent,
                                 unsigned char c, markdown_core_inline_parser *inline_parser) {
    if (markdown_core_inline_parser_in_bracket(inline_parser, false) ||
        markdown_core_inline_parser_in_bracket(inline_parser, true)) {
        return NULL;
    }

    if (c == ':') {
        return url_match(parser, parent, inline_parser);
    }

    if (c == 'w') {
        return www_match(parser, parent, inline_parser);
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

static void postprocess_text(markdown_core_parser *parser, markdown_core_node *text) {
    size_t start = 0;
    size_t offset = 0;
    int source_start_line = text->start_line;
    int source_start_column = text->start_column;
    // `text` is going to be split into a list of nodes containing shorter segments
    // of text, so we detach the memory buffer from text and use `markdown_core_chunk_dup` to
    // create references to it. Later, `markdown_core_chunk_to_cstr` is used to convert
    // the references into allocated buffers. The detached buffer is freed before we
    // return.
    markdown_core_chunk detached_chunk = text->as.literal;
    text->as.literal = markdown_core_chunk_dup(&detached_chunk, 0, detached_chunk.len);

    uint8_t *data = text->as.literal.data;
    size_t remaining = text->as.literal.len;

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

        markdown_core_node *link_node = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_LINK, parser->mem);
        if (!link_node) {
            parser->oom = true;
            break;
        }
        size_t prefix_start = start;
        size_t prefix_len = offset + max_rewind - rewind;
        size_t link_start = start + offset + max_rewind - rewind;
        size_t link_len = link_end + rewind;
        size_t post_start = start + offset + max_rewind + link_end;
        size_t post_len = remaining - offset - max_rewind - link_end;
        markdown_core_strbuf buf;
        markdown_core_strbuf_init(parser->mem, &buf, 10);
        if (auto_mailto) {
            markdown_core_strbuf_puts(&buf, "mailto:");
        }
        markdown_core_strbuf_put(&buf, data + start + offset + max_rewind - rewind, (bufsize_t)(link_end + rewind));
        link_node->as.link.url = markdown_core_chunk_buf_detach(&buf);
        if (!link_node->as.link.url.data) {
            parser->oom = true;
        }
        set_sourcepos_from_range(link_node, source_start_line, source_start_column, &detached_chunk, link_start,
                                 link_len);

        markdown_core_node *link_text = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_TEXT, parser->mem);
        if (!link_text) {
            parser->oom = true;
            markdown_core_node_free(link_node);
            break;
        }
        markdown_core_chunk email = markdown_core_chunk_dup(
            &detached_chunk, (bufsize_t)(start + offset + max_rewind - rewind), (bufsize_t)(link_end + rewind));
        /* The copy must own its bytes before detached_chunk is freed below. */
        if (!markdown_core_chunk_to_cstr(parser->mem, &email)) {
            parser->oom = true;
            markdown_core_chunk_set_cstr(parser->mem, &email, NULL);
        }
        link_text->as.literal = email;
        set_sourcepos_from_range(link_text, source_start_line, source_start_column, &detached_chunk, link_start,
                                 link_len);
        markdown_core_node_append_child(link_node, link_text);

        markdown_core_node_insert_after(text, link_node);

        markdown_core_node *post = markdown_core_node_new_with_mem(MARKDOWN_CORE_NODE_TEXT, parser->mem);
        if (!post) {
            parser->oom = true;
            break;
        }
        post->as.literal = markdown_core_chunk_dup(&detached_chunk, (bufsize_t)post_start, (bufsize_t)post_len);
        set_sourcepos_from_range(post, source_start_line, source_start_column, &detached_chunk, post_start, post_len);

        markdown_core_node_insert_after(link_node, post);

        text->as.literal = markdown_core_chunk_dup(&detached_chunk, (bufsize_t)prefix_start, (bufsize_t)prefix_len);
        if (!markdown_core_chunk_to_cstr(parser->mem, &text->as.literal)) {
            parser->oom = true;
            markdown_core_chunk_set_cstr(parser->mem, &text->as.literal, NULL);
        }
        set_sourcepos_from_range(text, source_start_line, source_start_column, &detached_chunk, prefix_start,
                                 prefix_len);

        text = post;
        start += offset + max_rewind + link_end;
        remaining -= offset + max_rewind + link_end;
        offset = 0;
    }

    // Convert the reference to allocated memory.
    assert(!text->as.literal.alloc);
    if (!markdown_core_chunk_to_cstr(parser->mem, &text->as.literal)) {
        parser->oom = true;
        markdown_core_chunk_set_cstr(parser->mem, &text->as.literal, NULL);
    }

    // Free the detached buffer.
    markdown_core_chunk_free(parser->mem, &detached_chunk);
}

static markdown_core_node *postprocess_block(markdown_core_extension *ext, markdown_core_parser *parser,
                                             markdown_core_node *block) {
    markdown_core_iter *iter;
    markdown_core_event_type ev;
    markdown_core_node *node;
    bool in_link = false;

    // Text nodes only live under inline-owning units; the driver has already
    // consolidated adjacent text nodes inside this block.
    if (!markdown_core_node_owns_inlines(block)) {
        return block;
    }

    iter = markdown_core_iter_new(block);
    if (!iter) {
        parser->oom = true;
        return block;
    }

    while ((ev = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        node = markdown_core_iter_get_node(iter);
        if (in_link) {
            if (ev == MARKDOWN_CORE_EVENT_EXIT && node->type == MARKDOWN_CORE_NODE_LINK) {
                in_link = false;
            }
            continue;
        }

        if (ev == MARKDOWN_CORE_EVENT_ENTER && node->type == MARKDOWN_CORE_NODE_LINK) {
            in_link = true;
            continue;
        }

        if (ev == MARKDOWN_CORE_EVENT_ENTER && node->type == MARKDOWN_CORE_NODE_TEXT) {
            postprocess_text(parser, node);
        }
    }

    markdown_core_iter_free(iter);

    return block;
}

static const unsigned char autolink_special_chars[] = {':', 'w'};

static const markdown_core_extension autolink_extension = {
    .name = "autolink",
    .match_inline = match,
    .postprocess_block = postprocess_block,
    .special_inline_chars = autolink_special_chars,
    .special_inline_char_count = sizeof(autolink_special_chars),
};

markdown_core_extension *markdown_core_autolink_extension(void) {
    // Immutable descriptor; the cast keeps the pre-existing pointer plumbing
    // without permitting writes (see extension.h).
    return (markdown_core_extension *)&autolink_extension;
}
