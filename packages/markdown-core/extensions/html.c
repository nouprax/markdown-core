#include "html_scanners.h"
#include "html.h"
#include "inline_internal.h"
#define advance(inline_parser) ((inline_parser)->pos += 1)

#include "comment.h"
bufsize_t markdown_core_inline_scan_inline_html(subject *inline_parser, bufsize_t pos, unsigned *flags,
                                                bool *is_comment) {
    bufsize_t matchlen = 0;
    bool comment = false;
    // finally, try to match an html tag
    if (pos + 2 <= inline_parser->input.len) {
        int c = inline_parser->input.data[pos];
        if (c == '!' && (*flags & FLAG_SKIP_HTML_COMMENT) == 0) {
            c = inline_parser->input.data[pos + 1];
            if (markdown_core_comment_scan_html(inline_parser, pos, flags, &matchlen)) {
                comment = matchlen > 0;
            } else if (c == '[') {
                if ((*flags & FLAG_SKIP_HTML_CDATA) == 0) {
                    matchlen = scan_html_cdata(&inline_parser->input, pos + 2);
                    if (matchlen > 0) {
                        // The regex doesn't require the final "]]>". But if we're not at
                        // the end of input, it must come after the match. Otherwise,
                        // disable subsequent scans to avoid quadratic behavior.
                        matchlen += 5; // prefix "![", suffix "]]>"
                        if (pos + matchlen > inline_parser->input.len) {
                            *flags |= FLAG_SKIP_HTML_CDATA;
                            matchlen = 0;
                        }
                    }
                }
            } else if ((*flags & FLAG_SKIP_HTML_DECLARATION) == 0) {
                matchlen = scan_html_declaration(&inline_parser->input, pos + 1);
                if (matchlen > 0) {
                    matchlen += 2; // prefix "!", suffix ">"
                    if (pos + matchlen > inline_parser->input.len) {
                        *flags |= FLAG_SKIP_HTML_DECLARATION;
                        matchlen = 0;
                    }
                }
            }
        } else if (c == '?') {
            if ((*flags & FLAG_SKIP_HTML_PI) == 0) {
                // Note that we allow an empty match.
                matchlen = scan_html_pi(&inline_parser->input, pos + 1);
                matchlen += 3; // prefix "?", suffix "?>"
                if (pos + matchlen > inline_parser->input.len) {
                    *flags |= FLAG_SKIP_HTML_PI;
                    matchlen = 0;
                }
            }
        } else {
            matchlen = scan_html_tag(&inline_parser->input, pos);
        }
    }
    if (is_comment) {
        *is_comment = comment;
    }
    return matchlen;
}

static markdown_core_node *handle_pointy_brace(subject *inline_parser) {
    bufsize_t matchlen = 0;
    bool comment = false;
    markdown_core_chunk contents;

    advance(inline_parser); // advance past first <

    matchlen =
        markdown_core_inline_scan_inline_html(inline_parser, inline_parser->pos, &inline_parser->flags, &comment);
    if (matchlen > 0) {
        if (comment) {
            return markdown_core_comment_make_html(inline_parser, inline_parser->pos, matchlen);
        }
        contents = markdown_core_chunk_dup(&inline_parser->input, inline_parser->pos - 1, matchlen + 1);
        inline_parser->pos += matchlen;
        markdown_core_node *node =
            markdown_core_inline_make_literal(inline_parser, MARKDOWN_CORE_NODE_HTML, inline_parser->pos - matchlen - 1,
                                              inline_parser->pos - 1, contents);
        return node;
    }

    // if nothing matches, just return the opening <:
    return make_str(inline_parser, inline_parser->pos - 1, inline_parser->pos - 1, markdown_core_chunk_literal("<"));
}

static markdown_core_node *match(const markdown_core_extension *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_parser *inline_parser) {
    return character == '<' ? handle_pointy_brace(inline_parser) : NULL;
}
const markdown_core_extension MARKDOWN_CORE_EXTENSION_HTML = {
    .name = "html",
    .match_inline = match,
    .terminates_text = "<",
    .dispatch = "<",
};
