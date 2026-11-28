#include "escape.h"
#include "entities.h"
#include "code_block_scanners.h"
#include "code_block.h"
#include "block_internal.h"
#include "attributes.h"
#include <string.h>

/* The elements whose syntax this element asks: where an attribute tail
 * ends an info string's first word, and the escapes and references the
 * info string decodes. */
enum { CODE_BLOCK_ATTRIBUTES, CODE_BLOCK_ESCAPE, CODE_BLOCK_ENTITIES };
static const markdown_core_element *const CODE_BLOCK_PEERS[] = {[CODE_BLOCK_ATTRIBUTES] =
                                                                    &MARKDOWN_CORE_ELEMENT_ATTRIBUTES,
                                                                [CODE_BLOCK_ESCAPE] = &MARKDOWN_CORE_ELEMENT_ESCAPE,
                                                                [CODE_BLOCK_ENTITIES] = &MARKDOWN_CORE_ELEMENT_ENTITIES,
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
    bufsize_t info_end = pos - markdown_core_parser_offer(parser, b, rest - offset, offset, offset + pos, true);
    b->as.code->literal_start = pos + 1;
    markdown_core_strbuf tmp = MARKDOWN_CORE_BUF_INIT();
    bufsize_t info_start = 0;
    while (info_start < info_end && markdown_core_is_space_or_tab(rest[info_start])) {
        info_start++;
    }
    while (info_end > info_start && markdown_core_is_space_or_tab(rest[info_end - 1])) {
        info_end--;
    }
    markdown_core_entities_copy(self->peers[CODE_BLOCK_ENTITIES], &tmp, rest + info_start, info_end - info_start);
    markdown_core_escape_strip(self->peers[CODE_BLOCK_ESCAPE], &tmp);
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

const markdown_core_element MARKDOWN_CORE_ELEMENT_CODE_BLOCK = {
    .peers = CODE_BLOCK_PEERS,
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

static bool view_code(markdown_core_node_pool *pool, markdown_core_node *copy, const markdown_core_node *node,
                      bool failed) {
    (void)pool;
    (void)node;
    (void)failed;
    markdown_core_chunk_view(&copy->as.code->info.value);
    markdown_core_literal_view(&copy->as.code->literal);
    return true;
}

static bool own_code(markdown_core_node *copy, const markdown_core_node *node) {
    return markdown_core_chunk_own(&copy->as.code->info.value, &node->as.code->info.value) &&
           markdown_core_chunk_own(&copy->as.code->literal, &node->as.code->literal);
}

static void adopt_code(markdown_core_node *view, markdown_core_node *node) {
    markdown_core_chunk_adopt(&view->as.code->info.value, &node->as.code->info.value);
    markdown_core_chunk_adopt(&view->as.code->literal, &node->as.code->literal);
}

const markdown_core_record_ops MARKDOWN_CORE_RECORD_CODE_BLOCK = {
    .size = sizeof(markdown_core_code),
    .free = free_code,
    .view = view_code,
    .own = own_code,
    .adopt = adopt_code,
};
