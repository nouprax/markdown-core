#include <string.h>
#include "html_block.h"
#include "block_internal.h"

/* An HTML block's literal is its content; a type-2 block its own end
 * condition closed and that holds one comment and nothing else is that
 * comment. */
static void close_html(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)token;
    (void)index;
    markdown_core_block_turn *turn = markdown_core_block_turned(compiler);
    const markdown_core_element_instance *self = compiler->current->context;
    markdown_core_parser *parser = turn->parser;
    markdown_core_node *b = turn->node;
    markdown_core_node *parent = *turn->container;
    markdown_core_reader *reader = turn->reader;
    (void)self;
    (void)parser;
    (void)b;
    (void)parent;
    (void)reader;
    markdown_core_literal_set(b, &b->as.html_block->literal, 0, b->content.size);
}
enum { HTML_BLOCK_OPEN, HTML_BLOCK_SHUT, HTML_BLOCK_BLOCKS };

/* THE HTML BLOCK opens as its token is compiled, of the kind its start
 * found; what the start read is the block's text, the line's to give it,
 * from where the start began (its indentation is the block's too). */
static void S_html_block_open(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    markdown_core_block_turn *turn = markdown_core_block_turned(compiler);
    const block_start *start = turn->start;
    *turn->container = markdown_core_parser_add_child(turn->parser, *turn->container, MARKDOWN_CORE_NODE_HTML_BLOCK,
                                                      *MARKDOWN_CORE_BLOCK_FOUND(start, bufsize_t) + 1);
    turn->gives = token->start;
    if (!*turn->container) {
        return;
    }
    (*turn->container)->as.html_block->block_type = start->matched;
    turn->opened = true;
}

/* THE START (micromark's htmlFlow, as CommonMark's seven kinds): `<` then
 * a raw tag (1), a comment (2), an instruction (3), a declaration (4),
 * CDATA (5), a block tag (6), or any complete tag alone on its line (7),
 * which interrupts no paragraph. The frame keeps whether the tag closes,
 * the tag name's length and its first bytes, folded, and for an
 * attribute's value its quote or whether spaces came before a name. */
enum { HTML_CLOSES, HTML_LENGTH, HTML_NAME, HTML_LAST = HTML_NAME + 3 };
/* The last word holds the quote while a value is quoted, whether spaces
 * came before a name otherwise, and an end condition's kind. */
enum { HTML_QUOTE = HTML_LAST, HTML_SPACED = HTML_LAST, HTML_KIND = HTML_LAST };
#define HTML_NAME_ROOM 12

static const char *const HTML_RAW[] = {"script", "pre", "textarea", "style", NULL};
static const char *const HTML_BLOCK_NAMES[] = {
    "address",  "article",    "aside",  "base",    "basefont", "blockquote", "body",     "caption",  "center",
    "col",      "colgroup",   "dd",     "details", "dialog",   "dir",        "div",      "dl",       "dt",
    "fieldset", "figcaption", "figure", "footer",  "form",     "frame",      "frameset", "h1",       "h2",
    "h3",       "h4",         "h5",     "h6",      "head",     "header",     "hr",       "html",     "iframe",
    "legend",   "li",         "link",   "main",    "menu",     "menuitem",   "nav",      "noframes", "ol",
    "optgroup", "option",     "p",      "param",   "search",   "section",    "summary",  "table",    "tbody",
    "td",       "tfoot",      "th",     "thead",   "title",    "tr",         "track",    "ul",       NULL};

static bool S_letter(int code) { return (code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z'); }
static bool S_digit(int code) { return code >= '0' && code <= '9'; }
static int S_fold(int code) { return code >= 'A' && code <= 'Z' ? code + 32 : code; }

/* The tag name read so far is one of `names`. */
static bool S_named(markdown_core_tokenizer *tokenizer, const char *const *names) {
    const uint32_t *words = markdown_core_frame_local(tokenizer);
    const size_t length = words[HTML_LENGTH];
    if (length > HTML_NAME_ROOM) {
        return false;
    }
    const char *name = (const char *)(words + HTML_NAME);
    for (; *names; names++) {
        if (strlen(*names) == length && !memcmp(*names, name, length)) {
            return true;
        }
    }
    return false;
}

static markdown_core_state S_found(markdown_core_tokenizer *tokenizer, uint32_t kind) {
    markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    turn->start->matched = (bufsize_t)kind;
    turn->start->kind = MARKDOWN_CORE_NODE_HTML_BLOCK;
    markdown_core_exit(tokenizer, markdown_core_block_token(tokenizer, HTML_BLOCK_OPEN));
    return markdown_core_ok(tokenizer);
}

/* A complete tag ends with spaces and the line's end (7), which interrupts
 * no paragraph and opens on no lazy line. */
static markdown_core_state S_complete_end(markdown_core_tokenizer *tokenizer, int code) {
    if (markdown_core_code_space(code)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_complete_end);
    }
    return markdown_core_code_ending(code) ? S_found(tokenizer, 7) : markdown_core_nok(tokenizer);
}

static markdown_core_state S_complete_gt(markdown_core_tokenizer *tokenizer, int code) {
    if (code != '>') {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_complete_end);
}

static markdown_core_state S_attribute_before(markdown_core_tokenizer *tokenizer, int code);

static bool S_unquoted(int code) {
    return !markdown_core_code_space(code) && !markdown_core_code_ending(code) && code != '"' && code != '\'' &&
           code != '=' && code != '<' && code != '>' && code != '`';
}

static markdown_core_state S_value_unquoted(markdown_core_tokenizer *tokenizer, int code) {
    if (S_unquoted(code)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_value_unquoted);
    }
    markdown_core_frame_local(tokenizer)[HTML_SPACED] = 0;
    return S_attribute_before(tokenizer, code);
}

static markdown_core_state S_value_quoted(markdown_core_tokenizer *tokenizer, int code) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    if (markdown_core_code_ending(code)) {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_consume(tokenizer);
    if (code == (int)words[HTML_QUOTE]) {
        words[HTML_SPACED] = 0;
        return markdown_core_state_of(S_attribute_before);
    }
    return markdown_core_state_of(S_value_quoted);
}

static markdown_core_state S_value_before(markdown_core_tokenizer *tokenizer, int code) {
    if (markdown_core_code_space(code)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_value_before);
    }
    if (code == '"' || code == '\'') {
        markdown_core_frame_local(tokenizer)[HTML_QUOTE] = (uint32_t)code;
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_value_quoted);
    }
    return S_unquoted(code) ? S_value_unquoted(tokenizer, code) : markdown_core_nok(tokenizer);
}

static markdown_core_state S_attribute_after(markdown_core_tokenizer *tokenizer, int code) {
    if (markdown_core_code_space(code)) {
        markdown_core_frame_local(tokenizer)[HTML_SPACED] = 1;
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_attribute_after);
    }
    if (code == '=') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_value_before);
    }
    return S_attribute_before(tokenizer, code);
}

static markdown_core_state S_attribute_name(markdown_core_tokenizer *tokenizer, int code) {
    if (S_letter(code) || S_digit(code) || code == ':' || code == '.' || code == '_' || code == '-') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_attribute_name);
    }
    markdown_core_frame_local(tokenizer)[HTML_SPACED] = 0;
    return S_attribute_after(tokenizer, code);
}

/* Before an attribute: one begins only after spaces. */
static markdown_core_state S_attribute_before(markdown_core_tokenizer *tokenizer, int code) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    if (markdown_core_code_space(code)) {
        words[HTML_SPACED] = 1;
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_attribute_before);
    }
    if (code == '/') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_complete_gt);
    }
    if (code == '>') {
        return S_complete_gt(tokenizer, code);
    }
    if (words[HTML_SPACED] && (S_letter(code) || code == '_' || code == ':')) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_attribute_name);
    }
    return markdown_core_nok(tokenizer);
}

static markdown_core_state S_closing_after(markdown_core_tokenizer *tokenizer, int code) {
    if (markdown_core_code_space(code)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_closing_after);
    }
    return S_complete_gt(tokenizer, code);
}

/* The tag name read: a raw tag's (1), a block tag's (6), or a complete
 * tag's (7). */
static markdown_core_state S_name_after(markdown_core_tokenizer *tokenizer, int code) {
    const uint32_t *words = markdown_core_frame_local(tokenizer);
    const bool space = markdown_core_code_space(code) || markdown_core_code_ending(code);
    if (!words[HTML_CLOSES] && (space || code == '>') && S_named(tokenizer, HTML_RAW)) {
        return S_found(tokenizer, 1);
    }
    if ((space || code == '>' || (code == '/' && markdown_core_code_at(tokenizer, 1) == '>')) &&
        S_named(tokenizer, HTML_BLOCK_NAMES)) {
        return S_found(tokenizer, 6);
    }
    const block_start_context *context = markdown_core_block_turn_of(tokenizer)->context;
    if (context->paragraph || context->lazy) {
        return markdown_core_nok(tokenizer);
    }
    if (words[HTML_CLOSES]) {
        return S_closing_after(tokenizer, code);
    }
    markdown_core_frame_local(tokenizer)[HTML_SPACED] = 0;
    return S_attribute_before(tokenizer, code);
}

static markdown_core_state S_name(markdown_core_tokenizer *tokenizer, int code) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    if (S_letter(code) || S_digit(code) || code == '-') {
        if (words[HTML_LENGTH] < HTML_NAME_ROOM) {
            ((char *)(words + HTML_NAME))[words[HTML_LENGTH]] = (char)S_fold(code);
        }
        words[HTML_LENGTH]++;
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_name);
    }
    return S_name_after(tokenizer, code);
}

static markdown_core_state S_name_first(markdown_core_tokenizer *tokenizer, int code) {
    return S_letter(code) ? S_name(tokenizer, code) : markdown_core_nok(tokenizer);
}

/* `<![CDATA[`, read a byte at a time. */
static markdown_core_state S_cdata(markdown_core_tokenizer *tokenizer, int code) {
    static const char CDATA[] = "CDATA[";
    uint32_t *words = markdown_core_frame_local(tokenizer);
    if (code != CDATA[words[HTML_LENGTH]]) {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_consume(tokenizer);
    return ++words[HTML_LENGTH] == sizeof(CDATA) - 1 ? S_found(tokenizer, 5) : markdown_core_state_of(S_cdata);
}

static markdown_core_state S_comment_dash(markdown_core_tokenizer *tokenizer, int code) {
    if (code != '-') {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_consume(tokenizer);
    return S_found(tokenizer, 2);
}

static markdown_core_state S_bang(markdown_core_tokenizer *tokenizer, int code) {
    if (code == '-') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_comment_dash);
    }
    if (code == '[') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_cdata);
    }
    if (S_letter(code)) {
        markdown_core_consume(tokenizer);
        return S_found(tokenizer, 4);
    }
    return markdown_core_nok(tokenizer);
}

static markdown_core_state S_open(markdown_core_tokenizer *tokenizer, int code) {
    if (code == '!') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_bang);
    }
    if (code == '?') {
        markdown_core_consume(tokenizer);
        return S_found(tokenizer, 3);
    }
    if (code == '/') {
        markdown_core_frame_local(tokenizer)[HTML_CLOSES] = 1;
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_name_first);
    }
    return S_name_first(tokenizer, code);
}

static markdown_core_state S_less(markdown_core_tokenizer *tokenizer, int code) {
    if (code != '<') {
        return markdown_core_nok(tokenizer);
    }
    /* Where the block's text begins on the line, past its indentation. */
    *MARKDOWN_CORE_BLOCK_FOUND(markdown_core_block_turn_of(tokenizer)->start, bufsize_t) =
        (bufsize_t)(tokenizer->position - tokenizer->base);
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_open);
}

static markdown_core_state scan_html(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_enter(tokenizer, markdown_core_block_token(tokenizer, HTML_BLOCK_OPEN));
    return markdown_core_spaces(tokenizer, markdown_core_state_of(S_less), MARKDOWN_CORE_TOKEN_SPACE, UINT32_MAX, code);
}

/* THE END CONDITION (element.h, markdown_core_line_ops `ends`): a line of
 * the block that holds its kind's closer -- a raw tag's closing tag, `-->`,
 * `?>`, `>`, `]]>` -- closes it. The frame keeps how much of the closer the
 * line holds so far and, for a raw tag, the name read. */
static markdown_core_state S_ends_next(markdown_core_tokenizer *tokenizer, int code);

static markdown_core_state S_ends_raw(markdown_core_tokenizer *tokenizer, int code) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    if (code == '>' && S_named(tokenizer, HTML_RAW)) {
        return markdown_core_ok(tokenizer);
    }
    if (S_letter(code) && words[HTML_LENGTH] < HTML_NAME_ROOM) {
        ((char *)(words + HTML_NAME))[words[HTML_LENGTH]++] = (char)S_fold(code);
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_ends_raw);
    }
    words[HTML_CLOSES] = 0;
    return S_ends_next(tokenizer, code);
}

static markdown_core_state S_ends_next(markdown_core_tokenizer *tokenizer, int code) {
    /* The last three bytes read, the latest lowest. */
    static const uint32_t CLOSERS[] = {
        0, 0, ('-' << 16) | ('-' << 8) | '>', ('?' << 8) | '>', '>', (']' << 16) | (']' << 8) | '>'};
    static const uint32_t MASKS[] = {0, 0xFFFF, 0xFFFFFF, 0xFFFF, 0xFF, 0xFFFFFF};
    uint32_t *words = markdown_core_frame_local(tokenizer);
    if (markdown_core_code_ending(code)) {
        return markdown_core_nok(tokenizer);
    }
    const uint32_t kind = words[HTML_KIND];
    const uint32_t last = words[HTML_CLOSES] = (words[HTML_CLOSES] << 8) | ((uint32_t)code & 0xFF);
    markdown_core_consume(tokenizer);
    if (kind == 1) {
        if ((last & MASKS[1]) == (('<' << 8) | '/')) {
            words[HTML_LENGTH] = 0;
            return markdown_core_state_of(S_ends_raw);
        }
        return markdown_core_state_of(S_ends_next);
    }
    return (last & MASKS[kind]) == CLOSERS[kind] ? markdown_core_ok(tokenizer) : markdown_core_state_of(S_ends_next);
}

static markdown_core_state S_ends_check(markdown_core_tokenizer *tokenizer, int code) {
    const int kind = markdown_core_block_turn_of(tokenizer)->node->as.html_block->block_type;
    if (kind > 5) {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_frame_local(tokenizer)[HTML_KIND] = (uint32_t)kind;
    return S_ends_next(tokenizer, code);
}

static bool blank_line(const markdown_core_element_instance *self, markdown_core_parser *parser,
                       markdown_core_node *node, markdown_core_reader *reader) {
    (void)reader;
    (void)self;
    (void)parser;
    (void)node;
    return true;
}

static const markdown_core_content_ops HTML_BLOCK_CONTENT = {
    .mode = MARKDOWN_CORE_CONTENT_LITERAL,
};

static const markdown_core_block_construct HTML_BLOCK_STARTS[] = {
    {.bytes = "<", .indent = 3, .construct = {.start = scan_html}},
};

static const markdown_core_compile_ops HTML_BLOCK_BLOCK_TOKENS[HTML_BLOCK_BLOCKS] = {
    [HTML_BLOCK_OPEN] = {.enter = S_html_block_open},
    [HTML_BLOCK_SHUT] = {.exit = close_html},
};

static const markdown_core_open_ops HTML_BLOCK_OPENS = {
    .starts = HTML_BLOCK_STARTS,
    .start_count = 1,
    .tokens = HTML_BLOCK_BLOCK_TOKENS,
    .token_count = HTML_BLOCK_BLOCKS,
};

/* THE CONTINUATION (element.h, markdown_core_line_ops `continuation`): a
 * block of the first five kinds goes on to its end condition, one of the
 * last two to a blank line. A block of its kind holds no block a lookahead
 * adds. */
static markdown_core_state S_continued(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return markdown_core_ok(tokenizer);
}

static markdown_core_state S_discontinued(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return markdown_core_nok(tokenizer);
}

static markdown_core_state S_continue(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    if (turn->look) {
        return markdown_core_nok(tokenizer);
    }
    if (turn->node->as.html_block->block_type <= 5) {
        return markdown_core_ok(tokenizer);
    }
    return markdown_core_check(tokenizer, &markdown_core_blank_line, markdown_core_state_of(S_discontinued),
                               markdown_core_state_of(S_continued), code);
}

static const markdown_core_line_ops HTML_BLOCK_LINES = {
    .shut = &HTML_BLOCK_BLOCK_TOKENS[HTML_BLOCK_SHUT],
    .continuation = {.start = S_continue},
    .beyond = true,
    .blank_last = blank_line,
    .ends = {.start = S_ends_check},
};

const markdown_core_facet MARKDOWN_CORE_HTML_BLOCK_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_HTML_BLOCK, .whole = true},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_HTML_BLOCK = {
    .facets = MARKDOWN_CORE_HTML_BLOCK_FACETS,
    .name = "html_block",
    .content = &HTML_BLOCK_CONTENT,
    .opens = &HTML_BLOCK_OPENS,
    .lines = &HTML_BLOCK_LINES,
};

/* An HTML block's record (markdown_core_record_ops): its literal, which may
 * be pending (markdown_core_literal_set). */
static void free_html_block(markdown_core_node_pool *pool, markdown_core_node *node) {
    (void)pool;
    markdown_core_chunk_free(&node->as.html_block->literal);
}

const markdown_core_record_ops MARKDOWN_CORE_RECORD_HTML_BLOCK = {
    .size = sizeof(markdown_core_html_block),
    .free = free_html_block,
};
