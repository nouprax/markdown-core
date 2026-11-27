#include <string.h>
#include "html.h"
#include "inline_internal.h"
#include "services.h"

/* The services this element asks, as `self->needs` holds them. */
enum { HTML_COMMENTS };
static const char *const HTML_NEEDS[] = {[HTML_COMMENTS] = MARKDOWN_CORE_SERVICE_HTML_COMMENTS, NULL};

/* THE RAW HTML (plan symbols-cleanroom.md 23, micromark's html text),
 * code by code: `<` and a tag, a comment, a processing instruction, a
 * declaration or a CDATA section, one token; a comment one the dialect's
 * comments make (`data` 1). A `<` none follows is data. A comment, an
 * instruction, a declaration or a CDATA section that finds no closer
 * through the end is noted in the read's record (HTML_NEVER), where its
 * body began plus one: from there on none of its kind closes, so no later
 * one reads to the end again. The frame's words: the token, where the
 * body began, the bytes of a closer matched so far, a closing quote, and
 * where white space began. */
enum { HTML_RAW };
enum { NEVER_COMMENT, NEVER_CDATA, NEVER_DECLARATION, NEVER_INSTRUCTION, NEVER_KINDS };
typedef struct {
    uint32_t never[NEVER_KINDS];
} S_run;
enum { RAW_TOKEN, RAW_BODY, RAW_MATCHED, RAW_QUOTE, RAW_SPACE, RAW_KIND };

static inline S_run *S_run_of(markdown_core_tokenizer *tokenizer) {
    return markdown_core_read_record(
        tokenizer, ((const markdown_core_element_instance *)markdown_core_self(tokenizer))->run_offset);
}

static inline bool S_ascii(int code, int (*is)(char)) { return code >= 0 && code < 0x80 && is((char)code); }
static int S_letter(char c) { return markdown_core_isalpha(c); }
static int S_name_byte(char c) { return markdown_core_isalnum(c) || c == '-'; }
static int S_attribute_first(char c) { return markdown_core_isalpha(c) || c == '_' || c == ':'; }
static int S_attribute_byte(char c) { return markdown_core_isalnum(c) || c == '_' || c == ':' || c == '.' || c == '-'; }

/* The end read: what follows decides. */
static markdown_core_state S_end(markdown_core_tokenizer *tokenizer) {
    markdown_core_met_end(tokenizer);
    return markdown_core_nok(tokenizer);
}

/* The construct read through its `>`, a comment when `comment`. */
static markdown_core_state S_done(markdown_core_tokenizer *tokenizer, bool comment) {
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    const uint32_t token = markdown_core_frame_local(tokenizer)[RAW_TOKEN];
    markdown_core_consume(tokenizer);
    if (token != UINT32_MAX) {
        tokenizer->events.tokens[token].data = comment && self->needs[HTML_COMMENTS].ops;
    }
    markdown_core_exit(tokenizer, (uint16_t)(self->token_base + HTML_RAW));
    return markdown_core_ok(tokenizer);
}

/* A BODY THAT RUNS TO A CLOSER, the first `-->`, `]]>`, `?>` or `>`
 * after it (the closer's last byte `>`, its others all one byte, as many
 * as the kind's `closer` says), of kind RAW_KIND. */
static const struct {
    unsigned char byte, count;
} CLOSERS[NEVER_KINDS] = {
    [NEVER_COMMENT] = {'-', 2}, [NEVER_CDATA] = {']', 2}, [NEVER_DECLARATION] = {0, 0}, [NEVER_INSTRUCTION] = {'?', 1}};

static markdown_core_state S_body(markdown_core_tokenizer *tokenizer, int code) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    const uint32_t kind = words[RAW_KIND];
    if (code == MARKDOWN_CORE_EOF) {
        S_run *run = S_run_of(tokenizer);
        if (!run->never[kind] || words[RAW_BODY] + 1 < run->never[kind]) {
            run->never[kind] = words[RAW_BODY] + 1;
        }
        return S_end(tokenizer);
    }
    const unsigned char byte = CLOSERS[kind].byte, count = CLOSERS[kind].count;
    if (code == '>' && words[RAW_MATCHED] >= count) {
        return S_done(tokenizer, kind == NEVER_COMMENT);
    }
    if (byte && code == byte) {
        words[RAW_MATCHED] += words[RAW_MATCHED] < count;
    } else {
        words[RAW_MATCHED] = 0;
    }
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_body);
}

/* The body of kind `kind` begins, unless one of its kind found no closer
 * from here. */
static markdown_core_state S_body_start(markdown_core_tokenizer *tokenizer, uint32_t kind, uint32_t matched, int code) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    const uint32_t never = S_run_of(tokenizer)->never[kind];
    words[RAW_KIND] = kind;
    words[RAW_BODY] = tokenizer->position - matched;
    words[RAW_MATCHED] = matched;
    if (never && words[RAW_BODY] + 1 >= never) {
        return S_end(tokenizer);
    }
    return S_body(tokenizer, code);
}

/* `<!--` read: `>` and `->` close at once (`<!-->`, `<!--->`). */
static markdown_core_state S_comment_short(markdown_core_tokenizer *tokenizer, int code) {
    if (code == '>') {
        return S_done(tokenizer, true);
    }
    return S_body_start(tokenizer, NEVER_COMMENT, 1, code);
}

static markdown_core_state S_comment_open(markdown_core_tokenizer *tokenizer, int code) {
    if (code == '>') {
        return S_done(tokenizer, true);
    }
    if (code == '-') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_comment_short);
    }
    return S_body_start(tokenizer, NEVER_COMMENT, 0, code);
}

static markdown_core_state S_comment_dash(markdown_core_tokenizer *tokenizer, int code) {
    if (code != '-') {
        return code == MARKDOWN_CORE_EOF ? S_end(tokenizer) : markdown_core_nok(tokenizer);
    }
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_comment_open);
}

/* `<![` and `CDATA[`. */
static markdown_core_state S_cdata(markdown_core_tokenizer *tokenizer, int code) {
    static const char CDATA[] = "CDATA[";
    uint32_t *words = markdown_core_frame_local(tokenizer);
    if (words[RAW_MATCHED] == sizeof(CDATA) - 1) {
        return S_body_start(tokenizer, NEVER_CDATA, 0, code);
    }
    if (code == MARKDOWN_CORE_EOF) {
        return S_end(tokenizer);
    }
    if (code != CDATA[words[RAW_MATCHED]]) {
        return markdown_core_nok(tokenizer);
    }
    words[RAW_MATCHED]++;
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_cdata);
}

/* `<!` and a letter: a declaration, to its `>`. */
static markdown_core_state S_declaration(markdown_core_tokenizer *tokenizer, int code) {
    return S_body_start(tokenizer, NEVER_DECLARATION, 0, code);
}

static markdown_core_state S_bang(markdown_core_tokenizer *tokenizer, int code) {
    if (code == '-') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_comment_dash);
    }
    if (code == '[') {
        markdown_core_consume(tokenizer);
        markdown_core_frame_local(tokenizer)[RAW_MATCHED] = 0;
        return markdown_core_state_of(S_cdata);
    }
    if (S_ascii(code, S_letter)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_declaration);
    }
    return code == MARKDOWN_CORE_EOF ? S_end(tokenizer) : markdown_core_nok(tokenizer);
}

/* SPACE IN A TAG: spaces and tabs, at most one line ending, spaces and
 * tabs; none at all too. */
static markdown_core_state S_space_more(markdown_core_tokenizer *tokenizer, int code) {
    if (code == ' ' || code == '\t') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_space_more);
    }
    return markdown_core_ok(tokenizer);
}

static markdown_core_state S_space_feed(markdown_core_tokenizer *tokenizer, int code) {
    if (code == '\n') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_space_more);
    }
    return S_space_more(tokenizer, code);
}

static markdown_core_state S_space(markdown_core_tokenizer *tokenizer, int code) {
    if (code == ' ' || code == '\t') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_space);
    }
    if (code == '\r') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_space_feed);
    }
    if (code == '\n') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_space_more);
    }
    return markdown_core_ok(tokenizer);
}

static const markdown_core_construct HTML_SPACE = {.start = S_space};

static markdown_core_state S_tag_spaced(markdown_core_tokenizer *tokenizer, int code);

static inline markdown_core_state S_spaces(markdown_core_tokenizer *tokenizer, markdown_core_state next, int code) {
    markdown_core_frame_local(tokenizer)[RAW_SPACE] = tokenizer->position;
    return markdown_core_attempt(tokenizer, &HTML_SPACE, next, next, code);
}

static markdown_core_state S_tag_after(markdown_core_tokenizer *tokenizer, int code) {
    return S_spaces(tokenizer, markdown_core_state_of(S_tag_spaced), code);
}

static markdown_core_state S_quoted(markdown_core_tokenizer *tokenizer, int code) {
    if (code == MARKDOWN_CORE_EOF) {
        return S_end(tokenizer);
    }
    markdown_core_consume(tokenizer);
    return (uint32_t)code == markdown_core_frame_local(tokenizer)[RAW_QUOTE] ? markdown_core_state_of(S_tag_after)
                                                                             : markdown_core_state_of(S_quoted);
}

static inline bool S_unquoted(int code) { return code >= 0 && !strchr(" \t\r\n\"'=<>`", code ? code : 1); }

static markdown_core_state S_unquoted_more(markdown_core_tokenizer *tokenizer, int code) {
    if (S_unquoted(code)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_unquoted_more);
    }
    return S_tag_after(tokenizer, code);
}

static markdown_core_state S_value(markdown_core_tokenizer *tokenizer, int code) {
    if (code == '"' || code == '\'') {
        markdown_core_frame_local(tokenizer)[RAW_QUOTE] = (uint32_t)code;
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_quoted);
    }
    if (S_unquoted(code)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_unquoted_more);
    }
    return code == MARKDOWN_CORE_EOF ? S_end(tokenizer) : markdown_core_nok(tokenizer);
}

static markdown_core_state S_value_before(markdown_core_tokenizer *tokenizer, int code) {
    return S_spaces(tokenizer, markdown_core_state_of(S_value), code);
}

/* After an attribute's name and white space: its value, or what follows a
 * name without one. */
static markdown_core_state S_named(markdown_core_tokenizer *tokenizer, int code) {
    if (code == '=') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_value_before);
    }
    return S_tag_spaced(tokenizer, code);
}

static markdown_core_state S_attribute(markdown_core_tokenizer *tokenizer, int code) {
    if (S_ascii(code, S_attribute_byte)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_attribute);
    }
    return S_spaces(tokenizer, markdown_core_state_of(S_named), code);
}

static markdown_core_state S_slash(markdown_core_tokenizer *tokenizer, int code) {
    if (code == '>') {
        return S_done(tokenizer, false);
    }
    return code == MARKDOWN_CORE_EOF ? S_end(tokenizer) : markdown_core_nok(tokenizer);
}

/* An open tag after its name or an attribute and white space: an
 * attribute after some, `/>` or `>`. */
static markdown_core_state S_tag_spaced(markdown_core_tokenizer *tokenizer, int code) {
    const bool spaced = tokenizer->position > markdown_core_frame_local(tokenizer)[RAW_SPACE];
    if (code == '>') {
        return S_done(tokenizer, false);
    }
    if (code == '/') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_slash);
    }
    if (spaced && S_ascii(code, S_attribute_first)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_attribute);
    }
    return code == MARKDOWN_CORE_EOF ? S_end(tokenizer) : markdown_core_nok(tokenizer);
}

static markdown_core_state S_tag_name(markdown_core_tokenizer *tokenizer, int code) {
    if (S_ascii(code, S_name_byte)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_tag_name);
    }
    return S_tag_after(tokenizer, code);
}

static markdown_core_state S_close_end(markdown_core_tokenizer *tokenizer, int code) {
    if (code == '>') {
        return S_done(tokenizer, false);
    }
    return code == MARKDOWN_CORE_EOF ? S_end(tokenizer) : markdown_core_nok(tokenizer);
}

static markdown_core_state S_close_name(markdown_core_tokenizer *tokenizer, int code) {
    if (S_ascii(code, S_name_byte)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_close_name);
    }
    return S_spaces(tokenizer, markdown_core_state_of(S_close_end), code);
}

static markdown_core_state S_close(markdown_core_tokenizer *tokenizer, int code) {
    if (S_ascii(code, S_letter)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_close_name);
    }
    return code == MARKDOWN_CORE_EOF ? S_end(tokenizer) : markdown_core_nok(tokenizer);
}

static markdown_core_state S_instruction(markdown_core_tokenizer *tokenizer, int code) {
    return S_body_start(tokenizer, NEVER_INSTRUCTION, 0, code);
}

static markdown_core_state S_open(markdown_core_tokenizer *tokenizer, int code) {
    if (code == '!') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_bang);
    }
    if (code == '?') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_instruction);
    }
    if (code == '/') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_close);
    }
    if (S_ascii(code, S_letter)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_tag_name);
    }
    return code == MARKDOWN_CORE_EOF ? S_end(tokenizer) : markdown_core_nok(tokenizer);
}

static markdown_core_state S_raw(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    markdown_core_frame_local(tokenizer)[RAW_TOKEN] =
        markdown_core_enter(tokenizer, (uint16_t)(self->token_base + HTML_RAW));
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_open);
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

const markdown_core_element MARKDOWN_CORE_ELEMENT_HTML = {
    .facets = MARKDOWN_CORE_HTML_FACETS,
    .name = "html",
    .run_state_size = sizeof(S_run),
    .needs = HTML_NEEDS,
    .inlines = &HTML_INLINES,
};
