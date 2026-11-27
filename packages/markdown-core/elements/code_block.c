#include "services.h"
#include "code_block.h"
#include "block_internal.h"
#include <string.h>

/* The services this element asks, as `self->needs` holds them: where an
 * attribute tail ends an info string's first word, and the escapes and
 * references the info string decodes. */
enum { CODE_BLOCK_ATTRIBUTES, CODE_BLOCK_ESCAPES, CODE_BLOCK_REFERENCES };
static const char *const CODE_BLOCK_NEEDS[] = {[CODE_BLOCK_ATTRIBUTES] = MARKDOWN_CORE_SERVICE_ATTRIBUTES,
                                               [CODE_BLOCK_ESCAPES] = MARKDOWN_CORE_SERVICE_ESCAPES,
                                               [CODE_BLOCK_REFERENCES] = MARKDOWN_CORE_SERVICE_REFERENCES,
                                               NULL};

/* The end of an indented code block's literal: past the line ending of its
 * last line that is not blank, or its start when every line is: a first
 * line that opened on the continuation bytes of a scalar holds none of
 * them (UTF-8 is assumed well formed, never checked), and a look reads
 * only the lines up to the cursor. */
static bufsize_t S_indented_literal_end(const markdown_core_strbuf *content) {
    const bufsize_t size = content->size;
    bufsize_t i = size;
    /* One pass back over the trailing blank bytes: the last line ending it
     * passes is the first one past the last byte that is not blank. */
    bufsize_t end = size;
    /* A content in two parts (THE BACK OF A PARAGRAPH, blocks.c) reads its
     * bytes through the parts. */
    if (content->split) {
        while (i > 0) {
            const unsigned char c = markdown_core_strbuf_at(content, i - 1);
            if (!markdown_core_is_whitespace(c)) {
                return end;
            }
            end = c == '\n' ? i : end;
            i--;
        }
        return 0;
    }
    const unsigned char *bytes = content->ptr;
    while (i > 0) {
        const unsigned char c = bytes[i - 1];
        if (!markdown_core_is_whitespace(c)) {
            return end;
        }
        end = c == '\n' ? i : end;
        i--;
    }
    return 0;
}

/* A code block's literal is a view of its content: an indented block's
 * through its last line that is not blank, a fenced block's after its first
 * line, which is the rest of the fence line and gave the info string as the
 * block opened. */
static void close_code(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
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
    /* The matcher only decides; what its fence closed is the close's to
     * write. */
    b->as.code->fence_closed = b->as.code->fenced && (b->flags & MARKDOWN_CORE_NODE__CLOSED_BY_END_CONDITION) != 0;
    const markdown_core_strbuf *content = &b->content;
    const bufsize_t start = b->as.code->literal_start;
    const bufsize_t end = b->as.code->fenced ? content->size : S_indented_literal_end(content);
    markdown_core_literal_set(b, &b->as.code->literal, start, end);
}
/* The character reference at `at`, an `&`, when it spells whitespace, as
 * the info string's decoding reads it: its length; 0 when it spells
 * anything else or nothing; -1 when the data ends while it may still be
 * one. */
static bufsize_t space_reference(const unsigned char *data, bufsize_t at, bufsize_t length) {
    bufsize_t i = at + 1;
    if (i < length && data[i] == '#') {
        i++;
        const bool hex = i < length && (data[i] | 0x20) == 'x';
        i += hex;
        const bufsize_t digits = i, most = hex ? 6 : 7;
        long point = 0;
        for (; i < length && i - digits < most; i++) {
            const unsigned char c = data[i], low = c | 0x20;
            if (markdown_core_isdigit((char)c)) {
                point = point * (hex ? 16 : 10) + (c - '0');
            } else if (hex && low >= 'a' && low <= 'f') {
                point = point * 16 + (low - 'a' + 10);
            } else {
                break;
            }
        }
        if (i >= length) {
            return -1;
        }
        return i > digits && data[i] == ';' && (point == 9 || point == 10 || point == 13 || point == 32) ? i + 1 - at
                                                                                                         : 0;
    }
    static const char *const names[] = {"Tab;", "NewLine;"};
    for (size_t n = 0; n < sizeof(names) / sizeof(*names); n++) {
        const bufsize_t size = (bufsize_t)strlen(names[n]);
        const bufsize_t have = length - i < size ? length - i : size;
        if (!memcmp(data + i, names[n], (size_t)have)) {
            return have < size ? -1 : 1 + size;
        }
    }
    return 0;
}

/* Where an info string's first word ends: at a space or a reference that
 * spells one, or at `tail`, where the attribute block that ends the data but
 * for white space starts, -1 when none does, or -2 when storage could not be
 * had; a brace that opens no tail is a byte of the word. `cut` says whether
 * a reference the data's end cuts short may yet spell a space. */
static bufsize_t info_word(const unsigned char *data, bufsize_t length, bufsize_t tail, bool *cut) {
    if (tail < -1) {
        return -1;
    }
    bufsize_t end = 0;
    *cut = false;
    while (end < length && !markdown_core_is_whitespace(data[end]) && end != tail) {
        /* References decode before escapes, as S_fence_info decodes them:
         * a backslash before one escapes what it spells, and a space it
         * spells still ends the word. */
        if (data[end] == '\\' && end + 1 < length && markdown_core_ispunct(data[end + 1]) && data[end + 1] != '&') {
            end += 2;
            continue;
        }
        if (data[end] == '&') {
            const bufsize_t space = space_reference(data, end, length);
            if (space > 0) {
                break;
            }
            *cut = *cut || space < 0;
        }
        end++;
    }
    return end;
}
/* THE FENCE LINE GIVES THE INFO STRING AS THE BLOCK OPENS, once: `rest`,
 * `length` bytes, is the line past the fence, the reader's offset, which the block's content
 * begins with, and the literal starts past it. The info string is the rest
 * of the fence line, less an attribute tail, trimmed of spaces and tabs as
 * written. References and escapes are decoded after the trim, so a space one
 * of them spells is the info string's own. */
static void S_fence_info(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         markdown_core_node *b, markdown_core_reader *reader, const unsigned char *line) {
    /* The info's check read the rest of the line, to its ending: the line
     * in hand ends with a line feed, its last byte, which an input that
     * ends without one is given. */
    const unsigned char *rest = line + reader->offset;
    const bufsize_t pos = markdown_core_reader_length(reader) - 1 - reader->offset;
    /* The line's end is offered (element.h, markdown_core_follow): the
     * info string ends where what is taken begins. */
    const bufsize_t offset = reader->offset;
    const markdown_core_field_to fields = {parser, b, NULL, reader, 0, NULL};
    bufsize_t info_end =
        pos - markdown_core_parser_offer(parser, b, rest - offset, offset, offset + pos, true, &fields);
    b->as.code->literal_start = pos + 1;
    markdown_core_strbuf tmp = MARKDOWN_CORE_BUF_INIT();
    bufsize_t info_start = 0;
    while (info_start < info_end && markdown_core_is_space_or_tab(rest[info_start])) {
        info_start++;
    }
    while (info_end > info_start && markdown_core_is_space_or_tab(rest[info_end - 1])) {
        info_end--;
    }
    /* The info string's first word as written, after the spaces and the
     * references that spell them before it, is the block's field, its
     * language, empty when the fence wrote none; the rest of the string is
     * a field of no slot kind. Its literal starts on the line after the
     * fence's. */
    bufsize_t word = info_start, space;
    while (word < info_end) {
        if (markdown_core_is_space_or_tab(rest[word])) {
            word++;
        } else if (rest[word] == '&' && (space = space_reference(rest, word, info_end)) > 0) {
            word += space;
        } else {
            break;
        }
    }
    bool cut;
    const bufsize_t language = word + info_word(rest + word, info_end - word, -1, &cut);
    const markdown_core_field_spot spots[] = {
        {offset + word, offset + language, MARKDOWN_CORE_SLOT_INFO, true, MARKDOWN_CORE_FIELD},
        {offset + info_start, offset + word, MARKDOWN_CORE_SLOT_NONE, false, MARKDOWN_CORE_FIELD},
        {offset + language, offset + info_end, MARKDOWN_CORE_SLOT_NONE, false, MARKDOWN_CORE_FIELD},
        {offset + pos, offset + pos, MARKDOWN_CORE_SLOT_NONE, false, MARKDOWN_CORE_FIELD_LINES}};
    markdown_core_field_many(&fields, spots, sizeof(spots) / sizeof(spots[0]));
    markdown_core_decoder_write(self->needs[CODE_BLOCK_REFERENCES], &tmp, rest + info_start, info_end - info_start);
    markdown_core_decoder_strip(self->needs[CODE_BLOCK_ESCAPES], &tmp);
    /* WHETHER THE SOURCE WROTE AN INFO STRING IS DECIDED HERE, ONCE.
     * A fence with nothing but whitespace after it wrote none, and
     * this is the only place that still knows the difference between
     * that and the `js` in ```` ```js ````. The facade used to decide
     * it again by testing the length, which is the fold requirement 14
     * forbids. */
    if (tmp.oom) {
        /* A buffer that could not be grown has `size == 0` and it is
         * NOT an absent info string -- it is an info string the parse
         * lost. The strict OOM sweep requires that loss to terminate
         * the parse. */
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        markdown_core_strbuf_free(&tmp);
        b->as.code->info = markdown_core_optional_chunk_absent();
    } else if (tmp.size == 0) {
        markdown_core_strbuf_free(&tmp);
        b->as.code->info = markdown_core_optional_chunk_absent();
    } else {
        markdown_core_chunk info = markdown_core_chunk_buf_detach(&tmp);
        if (!info.data) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
        b->as.code->info = markdown_core_optional_chunk_present(info);
    }
}
/* WHAT THE FENCE'S START READ (the start's record): where its sequence
 * begins and ends, offsets on the line. */
typedef struct {
    bufsize_t first, past;
} code_fence;
MARKDOWN_CORE_BLOCK_FOUND_FITS(code_fence, code_fence_fits);

enum { CODE_BLOCK_INDENTED, CODE_BLOCK_FENCED, CODE_BLOCK_SEQUENCE, CODE_BLOCK_SHUT, CODE_BLOCK_BLOCKS };

static void S_fence_sequence(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    markdown_core_block_turn *turn = markdown_core_block_turned(compiler);
    code_fence *fence = MARKDOWN_CORE_BLOCK_FOUND(turn->start, code_fence);
    fence->first = (bufsize_t)(token->start - turn->base);
    fence->past = (bufsize_t)(token->end - turn->base);
}

/* THE FENCED BLOCK opens as its token exits: at its sequence, which the
 * spaces before it, its fence's offset, are stripped of every line of. The
 * rest of its line is its first line of content, which gives its info
 * string (S_fence_info). */
static void S_code_block_fenced(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    markdown_core_block_turn *turn = markdown_core_block_turned(compiler);
    const code_fence *fence = MARKDOWN_CORE_BLOCK_FOUND(turn->start, code_fence);
    const bufsize_t matched = fence->past - fence->first;
    markdown_core_node *const block =
        markdown_core_parser_add_child(turn->parser, *turn->container, MARKDOWN_CORE_NODE_CODE_BLOCK, fence->first + 1);
    *turn->container = block;
    if (!block) {
        return;
    }
    block->as.code->fenced = true;
    block->as.code->fence_char = compiler->data[turn->base + (uint32_t)fence->first];
    block->as.code->fence_length = (matched > 255) ? 255 : (uint8_t)matched;
    block->as.code->fence_offset = (int8_t)(fence->first - (bufsize_t)(token->start - turn->base));
    block->as.code->fence_closed = false;
    S_fence_info(markdown_core_compile_self(compiler), turn->parser, block, turn->reader, compiler->data + turn->base);
    turn->opened = true;
}

/* THE INDENTED BLOCK opens past its four columns of indentation. */
static void S_code_block_indented(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    markdown_core_block_turn *turn = markdown_core_block_turned(compiler);
    markdown_core_parser *parser = turn->parser;
    const bufsize_t at = (bufsize_t)(token->end - turn->base);
    markdown_core_node *const block =
        markdown_core_parser_add_child(parser, *turn->container, MARKDOWN_CORE_NODE_CODE_BLOCK, at + 1);
    *turn->container = block;
    if (!block) {
        return;
    }
    block->as.code->fenced = false;
    block->as.code->fence_char = 0;
    block->as.code->fence_length = 0;
    block->as.code->fence_offset = 0;
    block->as.code->fence_closed = false;
    /* An indented code block has no fence and therefore no info
     * string, ever. */
    block->as.code->info = markdown_core_optional_chunk_absent();
    block->as.code->literal_start = 0;
    /* Its literal starts where it does. */
    const markdown_core_field_to fields = {parser, block, NULL, turn->reader, 0, NULL};
    markdown_core_field_put(&fields, MARKDOWN_CORE_SLOT_NONE, MARKDOWN_CORE_FIELD_LINES, at, at, false);
    turn->opened = true;
}

/* THE CODE'S CONSTRUCTS (micromark's codeIndented and codeFenced). The
 * frame keeps the column the construct began at, the fence's marker and
 * its sequence's length. */
enum { CODE_COLUMN, CODE_MARKER, CODE_COUNT };

/* The line's end, after spaces. */
static markdown_core_state S_blank_after(markdown_core_tokenizer *tokenizer, int code) {
    return markdown_core_code_ending(code) ? markdown_core_ok(tokenizer) : markdown_core_nok(tokenizer);
}

static markdown_core_state S_found(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return markdown_core_ok(tokenizer);
}

static markdown_core_state S_lost(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return markdown_core_nok(tokenizer);
}

/* Four columns of indentation, a line not blank past them. */
static markdown_core_state S_indented_after(markdown_core_tokenizer *tokenizer, int code) {
    if (markdown_core_column(tokenizer) - markdown_core_frame_local(tokenizer)[CODE_COLUMN] < CODE_INDENT) {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_exit(tokenizer, markdown_core_block_token(tokenizer, CODE_BLOCK_INDENTED));
    /* A blank line opens none. The indentation's codes read the bytes that
     * continue a scalar after them as theirs (markdown_core_consume): a line
     * of a stray one is not blank, though the indentation took it. */
    if (tokenizer->data[tokenizer->position - 1] != ' ' && tokenizer->data[tokenizer->position - 1] != '\t') {
        return S_found(tokenizer, code);
    }
    return markdown_core_check(tokenizer, &markdown_core_blank_line, markdown_core_state_of(S_lost),
                               markdown_core_state_of(S_found), code);
}

static markdown_core_state S_indented(markdown_core_tokenizer *tokenizer, int code) {
    const block_start_context *context = markdown_core_block_turn_of(tokenizer)->context;
    /* Indented code interrupts no paragraph, and a lazy line is text. */
    if (context->paragraph || context->lazy) {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_block_turn_of(tokenizer)->start->kind = MARKDOWN_CORE_NODE_CODE_BLOCK;
    markdown_core_frame_local(tokenizer)[CODE_COLUMN] = markdown_core_column(tokenizer);
    markdown_core_enter(tokenizer, markdown_core_block_token(tokenizer, CODE_BLOCK_INDENTED));
    return markdown_core_spaces(tokenizer, markdown_core_state_of(S_indented_after), 0, CODE_INDENT, code);
}

/* THE INFO: the rest of the fence's line holds no backtick after a fence
 * of backticks, the sequence's last byte. */
static markdown_core_state S_info(markdown_core_tokenizer *tokenizer, int code) {
    if (markdown_core_code_ending(code)) {
        return markdown_core_ok(tokenizer);
    }
    if (code == '`') {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_info);
}

static markdown_core_state S_info_start(markdown_core_tokenizer *tokenizer, int code) {
    if (markdown_core_code_at(tokenizer, -1) != '`') {
        /* A fence of tildes takes any info. */
        return markdown_core_ok(tokenizer);
    }
    return S_info(tokenizer, code);
}

static const markdown_core_construct CODE_INFO = {.start = S_info_start};

static markdown_core_state S_fence(markdown_core_tokenizer *tokenizer, int code) {
    uint32_t *words = markdown_core_frame_local(tokenizer);
    if (code == (int)words[CODE_MARKER]) {
        markdown_core_consume(tokenizer);
        words[CODE_COUNT]++;
        return markdown_core_state_of(S_fence);
    }
    if (words[CODE_COUNT] < 3) {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_exit(tokenizer, markdown_core_block_token(tokenizer, CODE_BLOCK_SEQUENCE));
    markdown_core_exit(tokenizer, markdown_core_block_token(tokenizer, CODE_BLOCK_FENCED));
    return markdown_core_check(tokenizer, &CODE_INFO, markdown_core_state_of(S_found), markdown_core_state_of(S_lost),
                               code);
}

static markdown_core_state S_fence_first(markdown_core_tokenizer *tokenizer, int code) {
    if (code != '`' && code != '~') {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_frame_local(tokenizer)[CODE_MARKER] = (uint32_t)code;
    markdown_core_enter(tokenizer, markdown_core_block_token(tokenizer, CODE_BLOCK_SEQUENCE));
    return S_fence(tokenizer, code);
}

static markdown_core_state S_fenced(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_block_turn_of(tokenizer)->start->kind = MARKDOWN_CORE_NODE_CODE_BLOCK;
    markdown_core_enter(tokenizer, markdown_core_block_token(tokenizer, CODE_BLOCK_FENCED));
    return markdown_core_spaces(tokenizer, markdown_core_state_of(S_fence_first), MARKDOWN_CORE_TOKEN_SPACE, UINT32_MAX,
                                code);
}

static bool blank_line(const markdown_core_element_instance *self, markdown_core_parser *parser,
                       markdown_core_node *node, markdown_core_reader *reader) {
    (void)reader;
    (void)self;
    return !node->as.code->fenced;
}
/* A fenced block an edit reopens at a later line (blocks.c, THE LEAP) has
 * met no closing fence yet. */
static void reopen_code(const markdown_core_element_instance *self, markdown_core_parser *parser,
                        markdown_core_node *node, const markdown_core_node *next, bool blank) {
    (void)self;
    (void)parser;
    (void)next;
    (void)blank;
    node->as.code->fence_closed = false;
}

/* A fenced block an edit's reading closes as it did (blocks.c, THE SPLICE
 * IN A BLOCK) met its closing fence as it did: the fence closed it when the
 * end condition did. */
static void spliced_code(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         markdown_core_node *node) {
    (void)self;
    (void)parser;
    node->as.code->fence_closed =
        node->as.code->fenced && (node->flags & MARKDOWN_CORE_NODE__CLOSED_BY_END_CONDITION) != 0;
}

static const markdown_core_edit_ops CODE_BLOCK_EDIT = {
    .reopen_block = reopen_code,
    .spliced = spliced_code,
};

static const markdown_core_content_ops CODE_BLOCK_CONTENT = {
    .mode = MARKDOWN_CORE_CONTENT_LITERAL,
};

static const markdown_core_block_construct CODE_BLOCK_STARTS[] = {
    {.bytes = "", .indent = INT_MAX, .construct = {.start = S_indented}},
    {.bytes = "`~", .indent = 3, .construct = {.start = S_fenced}},
};

static const markdown_core_compile_ops CODE_BLOCK_BLOCK_TOKENS[CODE_BLOCK_BLOCKS] = {
    [CODE_BLOCK_INDENTED] = {.enter = S_code_block_indented},
    [CODE_BLOCK_FENCED] = {.exit = S_code_block_fenced},
    [CODE_BLOCK_SEQUENCE] = {.enter = S_fence_sequence},
    [CODE_BLOCK_SHUT] = {.exit = close_code},
};

static const markdown_core_open_ops CODE_BLOCK_OPENS = {
    .starts = CODE_BLOCK_STARTS,
    .start_count = 2,
    .tokens = CODE_BLOCK_BLOCK_TOKENS,
    .token_count = CODE_BLOCK_BLOCKS,
};

/* THE CONTINUATION (element.h, markdown_core_line_ops `continuation`): an
 * indented block's four columns of indentation, or a blank line; a fenced
 * block's closing fence, which closes it on its line, or the spaces of its
 * fence's offset. A block of its kind holds no block a lookahead adds. */
static markdown_core_state S_closing_after(markdown_core_tokenizer *tokenizer, int code) {
    if (!markdown_core_code_ending(code)) {
        return markdown_core_nok(tokenizer);
    }
    return markdown_core_block_continued(tokenizer, MARKDOWN_CORE_TOKEN_CLOSE);
}

static markdown_core_state S_closing(markdown_core_tokenizer *tokenizer, int code) {
    const markdown_core_node *node = markdown_core_block_turn_of(tokenizer)->node;
    uint32_t *words = markdown_core_frame_local(tokenizer);
    if (code == node->as.code->fence_char) {
        markdown_core_consume(tokenizer);
        words[CODE_COUNT]++;
        return markdown_core_state_of(S_closing);
    }
    if (words[CODE_COUNT] < 3 || words[CODE_COUNT] < node->as.code->fence_length) {
        return markdown_core_nok(tokenizer);
    }
    return markdown_core_spaces(tokenizer, markdown_core_state_of(S_closing_after), MARKDOWN_CORE_TOKEN_PREFIX,
                                UINT32_MAX, code);
}

static markdown_core_state S_closing_first(markdown_core_tokenizer *tokenizer, int code) {
    return code == markdown_core_block_turn_of(tokenizer)->node->as.code->fence_char ? S_closing(tokenizer, code)
                                                                                     : markdown_core_nok(tokenizer);
}

static markdown_core_state S_closing_start(markdown_core_tokenizer *tokenizer, int code) {
    return markdown_core_spaces(tokenizer, markdown_core_state_of(S_closing_first), MARKDOWN_CORE_TOKEN_PREFIX, 3,
                                code);
}

static const markdown_core_construct CODE_CLOSING = {.start = S_closing_start};

static markdown_core_state S_offset(markdown_core_tokenizer *tokenizer, int code) {
    const markdown_core_node *node = markdown_core_block_turn_of(tokenizer)->node;
    return markdown_core_spaces(tokenizer, markdown_core_state_of(S_found), MARKDOWN_CORE_TOKEN_PREFIX,
                                (uint32_t)node->as.code->fence_offset, code);
}

static markdown_core_state S_indent_after(markdown_core_tokenizer *tokenizer, int code) {
    if (markdown_core_column(tokenizer) - markdown_core_frame_local(tokenizer)[CODE_COLUMN] >= CODE_INDENT) {
        return markdown_core_ok(tokenizer);
    }
    /* A blank line goes on in the block, past its spaces. */
    return markdown_core_spaces(tokenizer, markdown_core_state_of(S_blank_after), MARKDOWN_CORE_TOKEN_PREFIX,
                                UINT32_MAX, code);
}

static markdown_core_state S_continue(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    if (turn->look) {
        return markdown_core_nok(tokenizer);
    }
    if (turn->node->as.code->fenced) {
        return markdown_core_attempt(tokenizer, &CODE_CLOSING, markdown_core_state_of(S_found),
                                     markdown_core_state_of(S_offset), code);
    }
    markdown_core_frame_local(tokenizer)[CODE_COLUMN] = markdown_core_column(tokenizer);
    return markdown_core_spaces(tokenizer, markdown_core_state_of(S_indent_after), MARKDOWN_CORE_TOKEN_PREFIX,
                                CODE_INDENT, code);
}

static const markdown_core_line_ops CODE_BLOCK_LINES = {
    .shut = &CODE_BLOCK_BLOCK_TOKENS[CODE_BLOCK_SHUT],
    .continuation = {.start = S_continue},
    .beyond = true,
    .blank_last = blank_line,
};

/* A code block's literal (element.h, markdown_core_facet `literal`). */
static markdown_core_chunk *S_literal(const markdown_core_node *node, bufsize_t *start) {
    *start = node->as.code->literal_start;
    return &node->as.code->literal;
}

const markdown_core_facet MARKDOWN_CORE_CODE_BLOCK_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_CODE_BLOCK, .literal = S_literal},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_CODE_BLOCK = {
    .needs = CODE_BLOCK_NEEDS,
    .facets = MARKDOWN_CORE_CODE_BLOCK_FACETS,
    .edit = &CODE_BLOCK_EDIT,
    .name = "code_block",
    .content = &CODE_BLOCK_CONTENT,
    .opens = &CODE_BLOCK_OPENS,
    .lines = &CODE_BLOCK_LINES,
};

/* A code block's record (markdown_core_record_ops): its info string and its
 * literal, which may be pending (markdown_core_literal_set). */
static void free_code(markdown_core_node_pool *pool, markdown_core_node *node) {
    (void)pool;
    markdown_core_optional_chunk_free(&node->as.code->info);
    markdown_core_chunk_free(&node->as.code->literal);
}

const markdown_core_record_ops MARKDOWN_CORE_RECORD_CODE_BLOCK = {
    .size = sizeof(markdown_core_code),
    .free = free_code,
};
