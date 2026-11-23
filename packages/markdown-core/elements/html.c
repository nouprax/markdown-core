#include <string.h>
#include "html_scanners.h"
#include "html.h"
#include "inline_internal.h"
#define advance(inline_state) markdown_core_inline_seek((inline_state), markdown_core_inline_offset(inline_state) + 1)

#include "comment_scanners.h"
#include "services.h"

/* The services this element asks, as `self->needs` holds them. */
enum { HTML_COMMENTS };
static const char *const HTML_NEEDS[] = {[HTML_COMMENTS] = MARKDOWN_CORE_SERVICE_HTML_COMMENTS, NULL};

/* An HTML comment (CommonMark 0.31.2, 6.6) after the `<` at `pos - 1`:
 * whether one opens there, and then its `length`, 0 when no closer follows
 * through the end of the input. */
static bool S_scan_comment(markdown_core_inline_state *inline_state, bufsize_t pos, bufsize_t *length) {
    if (markdown_core_inline_char_at(inline_state, pos) != '!' ||
        markdown_core_inline_char_at(inline_state, pos + 1) != '-' ||
        markdown_core_inline_char_at(inline_state, pos + 2) != '-') {
        return false;
    }
    if (markdown_core_inline_char_at(inline_state, pos + 3) == '>') {
        *length = 4;
    } else if (markdown_core_inline_char_at(inline_state, pos + 3) == '-' &&
               markdown_core_inline_char_at(inline_state, pos + 4) == '>') {
        *length = 5;
    } else {
        *length = scan_html_comment(markdown_core_inline_window(inline_state, 0).data,
                                    markdown_core_inline_window(inline_state, 0).len, pos + 1);
        /* 0 when no closer follows through the end of the input. */
        if (*length > 0) {
            *length += 1; // prefix "<"
        }
    }
    return true;
}

static bufsize_t markdown_core_inline_scan_inline_html(markdown_core_inline_state *inline_state, bufsize_t pos,
                                                       unsigned *flags, bool *is_comment) {
    bufsize_t matchlen = 0;
    bool comment = false;
    // finally, try to match an html tag
    if (pos + 2 <= markdown_core_inline_size(inline_state)) {
        int c = markdown_core_inline_char_at(inline_state, pos);
        if (c == '!' && (*flags & FLAG_SKIP_HTML_COMMENT) == 0) {
            c = markdown_core_inline_char_at(inline_state, pos + 1);
            if (S_scan_comment(inline_state, pos, &matchlen)) {
                /* No closer through the end of the input: later scans
                 * would not find one either. */
                comment = matchlen > 0;
                if (!comment) {
                    *flags |= FLAG_SKIP_HTML_COMMENT;
                }
            } else if (c == '[') {
                if ((*flags & FLAG_SKIP_HTML_CDATA) == 0) {
                    matchlen = scan_html_cdata(markdown_core_inline_window(inline_state, 0).data,
                                               markdown_core_inline_window(inline_state, 0).len, pos + 2);
                    if (matchlen > 0) {
                        // The regex doesn't require the final "]]>". But if we're not at
                        // the end of input, it must come after the match. Otherwise,
                        // disable subsequent scans to avoid quadratic behavior.
                        matchlen += 5; // prefix "![", suffix "]]>"
                        if (pos + matchlen > markdown_core_inline_size(inline_state)) {
                            *flags |= FLAG_SKIP_HTML_CDATA;
                            matchlen = 0;
                        }
                    }
                }
            } else if ((*flags & FLAG_SKIP_HTML_DECLARATION) == 0) {
                matchlen = scan_html_declaration(markdown_core_inline_window(inline_state, 0).data,
                                                 markdown_core_inline_window(inline_state, 0).len, pos + 1);
                if (matchlen > 0) {
                    matchlen += 2; // prefix "!", suffix ">"
                    if (pos + matchlen > markdown_core_inline_size(inline_state)) {
                        *flags |= FLAG_SKIP_HTML_DECLARATION;
                        matchlen = 0;
                    }
                }
            }
        } else if (c == '?') {
            if ((*flags & FLAG_SKIP_HTML_PI) == 0) {
                // Note that we allow an empty match.
                matchlen = scan_html_pi(markdown_core_inline_window(inline_state, 0).data,
                                        markdown_core_inline_window(inline_state, 0).len, pos + 1);
                matchlen += 3; // prefix "?", suffix "?>"
                if (pos + matchlen > markdown_core_inline_size(inline_state)) {
                    *flags |= FLAG_SKIP_HTML_PI;
                    matchlen = 0;
                }
            }
        } else {
            matchlen = scan_html_tag(markdown_core_inline_window(inline_state, 0).data,
                                     markdown_core_inline_window(inline_state, 0).len, pos);
            /* A tag ends at its `>`; one a name or `/` begins may end once
             * the input goes on. */
            if (!matchlen && (markdown_core_isalpha((char)c) || c == '/')) {
                markdown_core_inline_saw_end(inline_state);
            }
        }
        /* A declaration, processing instruction or CDATA section that does
         * not close may close once the input goes on. */
        if (!matchlen && (markdown_core_inline_char_at(inline_state, pos) == '!' ||
                          markdown_core_inline_char_at(inline_state, pos) == '?')) {
            markdown_core_inline_saw_end(inline_state);
        }
    } else {
        markdown_core_inline_saw_end(inline_state);
    }
    if (is_comment) {
        *is_comment = comment;
    }
    return matchlen;
}

static markdown_core_node *handle_pointy_brace(const markdown_core_element_instance *self,
                                               markdown_core_inline_state *inline_state) {
    bufsize_t matchlen = 0;
    bool comment = false;
    markdown_core_chunk contents;

    advance(inline_state); // advance past first <

    matchlen = markdown_core_inline_scan_inline_html(inline_state, markdown_core_inline_offset(inline_state),
                                                     &inline_state->flags, &comment);
    if (matchlen > 0) {
        const markdown_core_need comments = self->needs[HTML_COMMENTS];
        if (comment && comments.ops) {
            return ((const markdown_core_make_ops *)comments.ops)
                ->make(comments.instance, inline_state, markdown_core_inline_offset(inline_state), matchlen);
        }
        contents = markdown_core_inline_take(inline_state, markdown_core_inline_offset(inline_state) - 1, matchlen + 1);
        markdown_core_inline_seek(inline_state, markdown_core_inline_offset(inline_state) + (matchlen));
        markdown_core_node *node = markdown_core_inline_make_literal(
            inline_state, MARKDOWN_CORE_NODE_HTML, markdown_core_inline_offset(inline_state) - matchlen - 1,
            markdown_core_inline_offset(inline_state) - 1, contents);
        return node;
    }

    // if nothing matches, just return the opening <:
    return make_str(inline_state, markdown_core_inline_offset(inline_state) - 1,
                    markdown_core_inline_offset(inline_state) - 1,
                    markdown_core_inline_take(inline_state, markdown_core_inline_offset(inline_state) - 1, 1));
}

static markdown_core_node *match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    return character == '<' ? handle_pointy_brace(self, inline_state) : NULL;
}
static const markdown_core_inline_ops HTML_INLINES = {
    .match = match,
    .terminates = "<",
    .dispatch = "<",
};

const markdown_core_facet MARKDOWN_CORE_HTML_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_HTML, .whole = true},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

/* (services.h, MARKDOWN_CORE_SERVICE_HTML_TAGS) A tag another element's
 * scan steps over, the flags of the constructs that never close shared by
 * its asks, its memo. */
static bufsize_t S_skip(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state,
                        bufsize_t at, markdown_core_skip_memo *memo, bufsize_t *open) {
    (void)self;
    unsigned flags = (unsigned)memo->words[0];
    const bufsize_t width = markdown_core_inline_scan_inline_html(inline_state, at + 1, &flags, NULL);
    memo->words[0] = flags;
    if (width) {
        return at + 1 + width;
    }
    if (markdown_core_inline_size(inline_state) - at >= 4 &&
        !memcmp(markdown_core_inline_window(inline_state, 0).data + at, "<!--", 4)) {
        *open = at + 4;
    }
    return 0;
}

static const markdown_core_skip_ops HTML_TAGS = {.skip = S_skip};
static const markdown_core_service HTML_SERVICES[] = {{MARKDOWN_CORE_SERVICE_HTML_TAGS, &HTML_TAGS}, {0}};

const markdown_core_element MARKDOWN_CORE_ELEMENT_HTML = {
    .facets = MARKDOWN_CORE_HTML_FACETS,
    .name = "html",
    .provides = HTML_SERVICES,
    .needs = HTML_NEEDS,
    .inlines = &HTML_INLINES,
};
