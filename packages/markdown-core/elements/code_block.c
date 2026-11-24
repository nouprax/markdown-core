#include "services.h"
#include "code_block_scanners.h"
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
#define peek_at(input, at) ((input)->data[(at)])

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

static int continue_code(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         markdown_core_reader *reader, markdown_core_node *container) {
    bool res = false;

    if (!container->as.code->fenced) { // indented
        if (reader->indent >= CODE_INDENT) {
            markdown_core_reader_advance(reader, CODE_INDENT, true);
            res = true;
        } else if (reader->blank) {
            markdown_core_reader_skip(reader);
            res = true;
        }
    } else { // fenced
        bufsize_t matched = 0;

        if (reader->indent <= 3 &&
            (markdown_core_reader_peek(reader, reader->nonspace) == container->as.code->fence_char)) {
            matched = scan_close_code_fence(markdown_core_reader_rest(reader, 0), markdown_core_reader_length(reader),
                                            reader->nonspace);
        }

        if (matched >= container->as.code->fence_length) {
            // closing fence - and since we're at
            // the end of a line, we can stop processing it; the close
            // notes the fence closed it (close_code):
            markdown_core_reader_advance(reader, matched, false);
            return MARKDOWN_CORE_BLOCK_CLOSED;
        } else {
            // skip opt. spaces of fence reader->offset
            int i = container->as.code->fence_offset;

            while (i > 0 && markdown_core_is_space_or_tab(markdown_core_reader_peek(reader, reader->offset))) {
                markdown_core_reader_advance(reader, 1, true);
                i--;
            }
            res = true;
        }
    }

    return res;
}

/* A code block's literal is a view of its content: an indented block's
 * through its last line that is not blank, a fenced block's after its first
 * line, which is the rest of the fence line and gave the info string as the
 * block opened. */
static bool close_code(const markdown_core_element_instance *self, markdown_core_parser *parser, markdown_core_node *b,
                       markdown_core_node *parent, markdown_core_reader *reader) {
    (void)self;
    (void)parser;
    (void)parent;
    /* The matcher only decides; what its fence closed is the close's to
     * write. */
    b->as.code->fence_closed = b->as.code->fenced && (b->flags & MARKDOWN_CORE_NODE__CLOSED_BY_END_CONDITION) != 0;
    const markdown_core_strbuf *content = &b->content;
    const bufsize_t start = b->as.code->literal_start;
    const bufsize_t end = b->as.code->fenced ? content->size : S_indented_literal_end(content);
    markdown_core_literal_set(b, &b->as.code->literal, start, end);
    return true;
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
                         markdown_core_node *b, markdown_core_reader *reader) {
    const unsigned char *rest = markdown_core_reader_rest(reader, reader->offset);
    const bufsize_t length = markdown_core_reader_length(reader) - reader->offset;
    const unsigned char *line_end = memchr(rest, '\n', (size_t)length);
    assert(line_end);
    const bufsize_t pos = (bufsize_t)(line_end - rest);
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
static bool open_fenced(const markdown_core_element_instance *self, markdown_core_parser *parser,
                        markdown_core_node **container, markdown_core_reader *reader, block_start *start) {
    bufsize_t matched = start->matched;

    *container =
        markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_CODE_BLOCK, reader->nonspace + 1);
    if (!*container) {
        return false;
    }
    (*container)->as.code->fenced = true;
    (*container)->as.code->fence_char = markdown_core_reader_peek(reader, reader->nonspace);
    (*container)->as.code->fence_length = (matched > 255) ? 255 : (uint8_t)matched;
    (*container)->as.code->fence_offset = (int8_t)(reader->nonspace - reader->offset);
    (*container)->as.code->fence_closed = false;
    markdown_core_reader_advance(reader, reader->nonspace + matched - reader->offset, false);
    S_fence_info(self, parser, *container, reader);

    return true;
}
static bool open_indented(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_node **container, markdown_core_reader *reader, block_start *start) {
    (void)self;

    markdown_core_reader_advance(reader, CODE_INDENT, true);
    *container = markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_CODE_BLOCK, reader->offset + 1);
    if (!*container) {
        return false;
    }
    (*container)->as.code->fenced = false;
    (*container)->as.code->fence_char = 0;
    (*container)->as.code->fence_length = 0;
    (*container)->as.code->fence_offset = 0;
    (*container)->as.code->fence_closed = false;
    /* An indented code block has no fence and therefore no info
     * string, ever. */
    (*container)->as.code->info = markdown_core_optional_chunk_absent();
    (*container)->as.code->literal_start = 0;
    /* Its literal starts where it does. */
    const markdown_core_field_to fields = {parser, *container, NULL, reader, 0, NULL};
    markdown_core_field_put(&fields, MARKDOWN_CORE_SLOT_NONE, MARKDOWN_CORE_FIELD_LINES, reader->offset, reader->offset,
                            false);

    return true;
}
static bool scan_code(const markdown_core_element_instance *self, markdown_core_parser *parser,
                      block_start_context *context, block_start *start) {
    (void)self;
    if (context->reader->indent >= CODE_INDENT) {
        /* Indented code interrupts no paragraph, and a lazy line is text. */
        if (context->paragraph || context->lazy ||
            markdown_core_is_line_end(markdown_core_reader_peek(context->reader, context->reader->nonspace))) {
            return false;
        }
        start->open = open_indented;
    } else {
        start->matched = scan_open_code_fence(markdown_core_reader_rest(context->reader, 0),
                                              markdown_core_reader_length(context->reader), context->reader->nonspace);
        if (!start->matched) {
            return false;
        }
        start->open = open_fenced;
    }
    start->kind = MARKDOWN_CORE_NODE_CODE_BLOCK;
    return true;
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

static const markdown_core_open_ops CODE_BLOCK_OPENS = {
    .indent = INT_MAX,
    .scan = scan_code,
    /* A fence; an indented block is reached through the indent bound. */
    .scan_needs = {.bytes = "`~"},
};

static const markdown_core_line_ops CODE_BLOCK_LINES = {
    .matches = continue_code,
    .close = close_code,
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
