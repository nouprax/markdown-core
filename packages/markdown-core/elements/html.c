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

/* The bytes an HTML scan reads, and whether it read their end. */
typedef struct {
    const unsigned char *data;
    bufsize_t size;
    bool end;
} S_view;
static unsigned char S_at(S_view *view, bufsize_t at) {
    if (at >= view->size) {
        view->end = true;
        return 0;
    }
    return view->data[at];
}

/* An HTML comment (CommonMark 0.31.2, 6.6) after the `<` at `pos - 1`:
 * whether one opens there, and then its `length`, 0 when no closer follows
 * through the end of the input. */
static bool S_scan_comment(S_view *view, bufsize_t pos, bufsize_t *length) {
    if (S_at(view, pos) != '!' || S_at(view, pos + 1) != '-' || S_at(view, pos + 2) != '-') {
        return false;
    }
    if (S_at(view, pos + 3) == '>') {
        *length = 4;
    } else if (S_at(view, pos + 3) == '-' && S_at(view, pos + 4) == '>') {
        *length = 5;
    } else {
        *length = scan_html_comment(view->data, view->size, pos + 1);
        /* 0 when no closer follows through the end of the input. */
        if (*length > 0) {
            *length += 1; // prefix "<"
        }
    }
    return true;
}

static bufsize_t S_scan_html(S_view *view, bufsize_t pos, unsigned *flags, bool *is_comment) {
    bufsize_t matchlen = 0;
    bool comment = false;
    // finally, try to match an html tag
    if (pos + 2 <= view->size) {
        int c = S_at(view, pos);
        if (c == '!' && (*flags & FLAG_SKIP_HTML_COMMENT) == 0) {
            c = S_at(view, pos + 1);
            if (S_scan_comment(view, pos, &matchlen)) {
                /* No closer through the end of the input: later scans
                 * would not find one either. */
                comment = matchlen > 0;
                if (!comment) {
                    *flags |= FLAG_SKIP_HTML_COMMENT;
                }
            } else if (c == '[') {
                if ((*flags & FLAG_SKIP_HTML_CDATA) == 0) {
                    matchlen = scan_html_cdata(view->data, view->size, pos + 2);
                    if (matchlen > 0) {
                        // The regex doesn't require the final "]]>". But if we're not at
                        // the end of input, it must come after the match. Otherwise,
                        // disable subsequent scans to avoid quadratic behavior.
                        matchlen += 5; // prefix "![", suffix "]]>"
                        if (pos + matchlen > view->size) {
                            *flags |= FLAG_SKIP_HTML_CDATA;
                            matchlen = 0;
                        }
                    }
                }
            } else if ((*flags & FLAG_SKIP_HTML_DECLARATION) == 0) {
                matchlen = scan_html_declaration(view->data, view->size, pos + 1);
                if (matchlen > 0) {
                    matchlen += 2; // prefix "!", suffix ">"
                    if (pos + matchlen > view->size) {
                        *flags |= FLAG_SKIP_HTML_DECLARATION;
                        matchlen = 0;
                    }
                }
            }
        } else if (c == '?') {
            if ((*flags & FLAG_SKIP_HTML_PI) == 0) {
                // Note that we allow an empty match.
                matchlen = scan_html_pi(view->data, view->size, pos + 1);
                matchlen += 3; // prefix "?", suffix "?>"
                if (pos + matchlen > view->size) {
                    *flags |= FLAG_SKIP_HTML_PI;
                    matchlen = 0;
                }
            }
        } else {
            matchlen = scan_html_tag(view->data, view->size, pos);
            /* A tag ends at its `>`; one a name or `/` begins may end once
             * the input goes on. */
            if (!matchlen && (markdown_core_isalpha((char)c) || c == '/')) {
                view->end = true;
            }
        }
        /* A declaration, processing instruction or CDATA section that does
         * not close may close once the input goes on. */
        if (!matchlen && (S_at(view, pos) == '!' || S_at(view, pos) == '?')) {
            view->end = true;
        }
    } else {
        view->end = true;
    }
    if (is_comment) {
        *is_comment = comment;
    }
    return matchlen;
}

/* THE RAW HTML (plan symbols-cleanroom.md 23): `<` and a tag, a comment,
 * a processing instruction, a declaration or a CDATA section, one token;
 * a comment one the dialect's comments make (`data` 1). A `<` none
 * follows is data. The constructs that never close are noted in the
 * read's record, so no later `<` scans for them again. */
enum { HTML_RAW };
static markdown_core_state S_raw(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    S_view view = {tokenizer->data, (bufsize_t)tokenizer->size, false};
    bool comment = false;
    const bufsize_t length = S_scan_html(&view, (bufsize_t)tokenizer->position + 1,
                                         markdown_core_read_record(tokenizer, self->run_offset), &comment);
    if (view.end) {
        markdown_core_met_end(tokenizer);
    }
    if (!length) {
        return markdown_core_nok(tokenizer);
    }
    const uint32_t token = markdown_core_enter(tokenizer, (uint16_t)(self->token_base + HTML_RAW));
    if (token != UINT32_MAX) {
        tokenizer->events.tokens[token].data = comment && self->needs[HTML_COMMENTS].ops;
    }
    markdown_core_consume_to(tokenizer, tokenizer->position + 1 + (uint32_t)length);
    markdown_core_exit(tokenizer, (uint16_t)(self->token_base + HTML_RAW));
    return markdown_core_ok(tokenizer);
}
static const markdown_core_text_construct HTML_CONSTRUCTS[] = {{"<", {.start = S_raw}}};

static void S_made(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    const markdown_core_element_instance *self = markdown_core_compile_self(compiler);
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    const bufsize_t start = (bufsize_t)token->start, length = (bufsize_t)(token->end - token->start) - 1;
    markdown_core_node *node;
    if (token->data) {
        const markdown_core_need comments = self->needs[HTML_COMMENTS];
        node = ((const markdown_core_make_ops *)comments.ops)->make(comments.instance, inline_state, start + 1, length);
    } else {
        node = markdown_core_inline_make_literal(inline_state, MARKDOWN_CORE_NODE_HTML, start, start + length,
                                                 markdown_core_inline_take(inline_state, start, length + 1));
    }
    markdown_core_compile_append(compiler, node);
}
static const markdown_core_compile_ops HTML_TOKENS[] = {[HTML_RAW] = {.exit = S_made}};

static const markdown_core_inline_ops HTML_INLINES = {
    .constructs = HTML_CONSTRUCTS,
    .construct_count = 1,
    .tokens = HTML_TOKENS,
    .token_count = 1,
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
    S_view view = {markdown_core_inline_window(inline_state, 0).data, markdown_core_inline_size(inline_state), false};
    const bufsize_t width = S_scan_html(&view, at + 1, &flags, NULL);
    if (view.end) {
        markdown_core_inline_saw_end(inline_state);
    }
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
    .run_state_size = sizeof(unsigned),
    .provides = HTML_SERVICES,
    .needs = HTML_NEEDS,
    .inlines = &HTML_INLINES,
};
