#include "list.h"
#include "tasklist.h"
#include "block_internal.h"

/* What a list's start found (WHAT THE START FOUND, block_internal.h): the
 * item's facts, then where its marker and numeral stand on the line, the
 * column it started at and the numeral's first byte. */
typedef struct {
    /* First: the task marker's construct writes it (tasklist.h). */
    markdown_core_task_found task;
    markdown_core_list list;
    bufsize_t marker, begin, end;
    /* The columns where the start began, its marker, and past the marker. */
    uint32_t column, from, past;
    int first;
    bool closed;
} list_found;
MARKDOWN_CORE_BLOCK_FOUND_FITS(list_found, list_found_fits);

/* The list element's flags, on a List. */
enum {
    /* The list's last line was blank: a second blank line ends it. */
    MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK = MARKDOWN_CORE_NODE__ELEMENT_FIRST,
    /* An item that settled into the list makes it loose (fold_child). */
    MARKDOWN_CORE_NODE__LIST_LOOSE = MARKDOWN_CORE_NODE__ELEMENT_FIRST << 1,
    /* The last item that settled makes the list loose once another item
     * follows it. */
    MARKDOWN_CORE_NODE__LIST_LOOSE_FOLLOWED = MARKDOWN_CORE_NODE__ELEMENT_FIRST << 2,
};

static bool markdown_core_block_list_facts_match(const markdown_core_list *list, const markdown_core_list *item);
/* The upper-case roman letters: the terms of ordered_numeral's roman variant. */
static bool roman_letter(unsigned char c) { return c && strchr("MDCLXVI", c) != NULL; }
static bool ordered_numeral(markdown_core_list_work *counts, const unsigned char *input, bufsize_t begin, bufsize_t end,
                            markdown_core_ordered_list_variant variant, int *value) {
    int number = 0;
    // An automatic marker has value 1 in every variant, including the
    // variant inherited from a preceding authored marker.
    if (end == begin + 1 && input[begin] == '#') {
        *value = 1;
        return true;
    }
    if (variant.kind == MARKDOWN_CORE_ORDERED_LIST_VARIANT_DEFAULT) {
        return false;
    }
    if (variant.kind == MARKDOWN_CORE_ORDERED_LIST_VARIANT_ALPHA) {
        unsigned char first = variant.lowercased ? 'a' : 'A';
        if (end != begin + 1 || input[begin] < first || input[begin] > first + 25) {
            return false;
        }
        *value = input[begin] - first + 1;
        return true;
    }
    if (variant.kind == MARKDOWN_CORE_ORDERED_LIST_VARIANT_DECIMAL) {
        if (end - begin > 9) {
            return false;
        }
        for (bufsize_t at = begin; at < end; at++) {
            counts->markers++;
            if (!markdown_core_isdigit(input[at])) {
                return false;
            }
            number = number * 10 + input[at] - '0';
        }
    } else {
        static const struct {
            const char *text;
            int value;
            bool repeat;
        } terms[] = {{"M", 1000, true}, {"CM", 900, false}, {"D", 500, false}, {"CD", 400, false}, {"C", 100, true},
                     {"XC", 90, false}, {"L", 50, false},   {"XL", 40, false}, {"X", 10, true},    {"IX", 9, false},
                     {"V", 5, false},   {"IV", 4, false},   {"I", 1, true}};
        bufsize_t at = begin;
        for (size_t term = 0; term < sizeof(terms) / sizeof(terms[0]); term++) {
            bufsize_t width = terms[term].text[1] ? 2 : 1;
            unsigned char offset = variant.lowercased ? 'a' - 'A' : 0;
            while (at + width <= end) {
                counts->markers++;
                if (input[at] != terms[term].text[0] + offset ||
                    (width == 2 && input[at + 1] != terms[term].text[1] + offset)) {
                    break;
                }
                if (number > 999999999 - terms[term].value) {
                    return false;
                }
                number += terms[term].value;
                at += width;
                if (!terms[term].repeat) {
                    break;
                }
            }
        }
        if (at != end) {
            return false;
        }
    }
    *value = number;
    return end > begin;
}

static bool markdown_core_block_list_facts_match(const markdown_core_list *list, const markdown_core_list *item) {
    return list->flavor == item->flavor && list->bullet_char == item->bullet_char &&
           list->variant.kind == item->variant.kind && list->variant.lowercased == item->variant.lowercased &&
           list->delimiter.kind == item->delimiter.kind && list->delimiter.closed == item->delimiter.closed;
}

/* A LIST IS LOOSE when a blank line separates two of its items or two of
 * the blocks an item holds that the blank-line facts see: an item that is
 * not the last ends with one, or a child that some block follows does. Each
 * item folds into the list as it settles, its children having folded into it
 * (blocks.c, S_fold): one that BLANK_SPACED marks makes the list loose, and
 * one whose last line, or the last child it sees, ends blank makes it loose
 * once another item follows. The list is laid out as each item folds, so it
 * is laid out as it closes, its last item having folded into it. */
static markdown_core_node_internal_flags fold_child(const markdown_core_element_instance *self,
                                                    markdown_core_node *node, markdown_core_node_internal_flags flags,
                                                    markdown_core_node_internal_flags child) {
    (void)self;
    assert(node->kind == MARKDOWN_CORE_NODE_LIST);
    if (flags & MARKDOWN_CORE_NODE__LIST_LOOSE_FOLLOWED || child & MARKDOWN_CORE_NODE__BLANK_SPACED) {
        flags |= MARKDOWN_CORE_NODE__LIST_LOOSE;
    }
    const markdown_core_node_internal_flags ends = MARKDOWN_CORE_NODE__BLANK_FOLDED | MARKDOWN_CORE_NODE__ENDS_BLANK;
    const bool followed_loose = (child & MARKDOWN_CORE_NODE__LAST_LINE_BLANK) || (child & ends) == ends;
    node->as.list->tight = !(flags & MARKDOWN_CORE_NODE__LIST_LOOSE);
    return (markdown_core_node_internal_flags)((flags & ~MARKDOWN_CORE_NODE__LIST_LOOSE_FOLLOWED) |
                                               (followed_loose ? MARKDOWN_CORE_NODE__LIST_LOOSE_FOLLOWED : 0));
}

/* A list's open builds what its start read: the item, in the open list it
 * continues or in a list it opens, and the task marker after it. The
 * reading stands past the item's padding. */
static bool markdown_core_list_open(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                    markdown_core_node **container, markdown_core_reader *reader, block_start *start,
                                    const unsigned char *line) {
    (void)self;
    list_found *found = MARKDOWN_CORE_BLOCK_FOUND(start, list_found);
    markdown_core_list *data = &found->list;
    markdown_core_node_type cont_type = (*container)->kind;
    if (cont_type != MARKDOWN_CORE_NODE_LIST || !markdown_core_block_list_facts_match((*container)->as.list, data)) {
        /* The open list it does not continue gave it what its marker
         * leaves open, as a variant: its opener reads that list. */
        if (cont_type == MARKDOWN_CORE_NODE_LIST) {
            markdown_core_reader_open(reader, *container);
        }
        *container = markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_LIST, found->marker + 1);
        if (!*container) {
            return false;
        }
        memcpy((*container)->as.list, data, sizeof(*data));
    }
    *container = markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_LIST_ITEM, found->marker + 1);
    if (!*container) {
        return false;
    }
    memcpy((*container)->as.list, data, sizeof(*data));
    markdown_core_task_take(parser, *container, reader, line, &found->task);
    return !parser->error;
}

enum { LIST_ITEM, LIST_BLOCKS };

static void S_list_item(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)token;
    (void)index;
    markdown_core_block_turn *turn = markdown_core_block_turned(compiler);
    turn->opened = markdown_core_list_open(markdown_core_compile_self(compiler), turn->parser, turn->container,
                                           turn->context->reader, turn->start, compiler->data + turn->base);
}

/* THE MARKER (micromark's list, code by code): up to three spaces, a
 * bullet, or a numeral -- `(` before it, `)` or `.` after it -- whitespace
 * after it, and the item's padding, the start's; then, as an indent, every
 * later line's. */
static inline bufsize_t S_offset(const markdown_core_tokenizer *tokenizer) {
    return (bufsize_t)(tokenizer->position - tokenizer->base);
}

static inline list_found *S_found(const markdown_core_tokenizer *tokenizer) {
    return MARKDOWN_CORE_BLOCK_FOUND(markdown_core_block_turn_of(tokenizer)->start, list_found);
}

static inline void S_count(markdown_core_tokenizer *tokenizer) {
    ((markdown_core_list_work *)((const markdown_core_element_instance *)markdown_core_self(tokenizer))->state)
        ->markers++;
}

/* The item, past its task marker if it has one. */
static markdown_core_state S_item_found(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return markdown_core_block_found(tokenizer, LIST_ITEM);
}

/* The item's padding is its marker's columns and the columns of space
 * past it, or one when the padding is refused. */
static markdown_core_state S_item_at(markdown_core_tokenizer *tokenizer, uint32_t space, int code) {
    list_found *found = S_found(tokenizer);
    found->list.marker_offset = (int)(found->column - found->from);
    found->list.padding = (int)(found->past - found->column + space);
    found->task.end = 0;
    return markdown_core_attempt(tokenizer, &markdown_core_task_marker, markdown_core_state_of(S_item_found),
                                 markdown_core_state_of(S_item_found), code);
}

static markdown_core_state S_item(markdown_core_tokenizer *tokenizer, int code) {
    return S_item_at(tokenizer, markdown_core_column(tokenizer) - S_found(tokenizer)->past, code);
}

static markdown_core_state S_item_one(markdown_core_tokenizer *tokenizer, int code) {
    return S_item_at(tokenizer, 1, code);
}

static markdown_core_state S_padded(markdown_core_tokenizer *tokenizer, int code) {
    if (markdown_core_code_space(code)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_item_one);
    }
    return S_item_one(tokenizer, code);
}

/* The marker stands: whitespace follows it, and the line is not blank
 * past it when it interrupts a paragraph. */
static markdown_core_state S_spaced(markdown_core_tokenizer *tokenizer, int code) {
    S_found(tokenizer)->past = markdown_core_column(tokenizer);
    return markdown_core_attempt(tokenizer, &markdown_core_item_padding, markdown_core_state_of(S_item),
                                 markdown_core_state_of(S_padded), code);
}

static markdown_core_state S_refused(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return markdown_core_nok(tokenizer);
}

static markdown_core_state S_marked(markdown_core_tokenizer *tokenizer, int code) {
    S_count(tokenizer);
    if (!markdown_core_code_space(code) && !markdown_core_code_ending(code)) {
        return markdown_core_nok(tokenizer);
    }
    if (markdown_core_block_turn_of(tokenizer)->context->paragraph) {
        return markdown_core_check(tokenizer, &markdown_core_blank_line, markdown_core_state_of(S_refused),
                                   markdown_core_state_of(S_spaced), code);
    }
    return S_spaced(tokenizer, code);
}

/* An upper-case letter and a period stand as a marker when two columns of
 * space or the line's end follow. */
static markdown_core_state S_wide_end(markdown_core_tokenizer *tokenizer, int code) {
    return markdown_core_code_ending(code) ||
                   markdown_core_column(tokenizer) - markdown_core_frame_local(tokenizer)[0] >= 2
               ? markdown_core_ok(tokenizer)
               : markdown_core_nok(tokenizer);
}

static markdown_core_state S_wide(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_frame_local(tokenizer)[0] = markdown_core_column(tokenizer);
    return markdown_core_spaces(tokenizer, markdown_core_state_of(S_wide_end), 0, 2, code);
}

static const markdown_core_construct LIST_WIDE = {.start = S_wide};

/* The numeral, past its delimiter, reads as its list's: a value in the
 * variant of the list it continues or of its own first byte, which only a
 * first item, or one in no item, may start past 1. */
static markdown_core_state S_delimited(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    markdown_core_list_work *counts = ((const markdown_core_element_instance *)markdown_core_self(tokenizer))->state;
    list_found *found = S_found(tokenizer);
    markdown_core_list *data = &found->list;
    markdown_core_node *container = turn->context->container;
    const markdown_core_list *committed = container->kind == MARKDOWN_CORE_NODE_LIST ? container->as.list : NULL;
    const unsigned char *line = tokenizer->data + tokenizer->base;
    const int c = found->first;
    const bufsize_t begin = found->begin, end = found->end;
    const bool period = data->delimiter.kind == MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PERIOD;
    data->variant.lowercased = c >= 'a' && c <= 'z';
    data->variant.kind = c == '#'                                   ? MARKDOWN_CORE_ORDERED_LIST_VARIANT_DEFAULT
                         : markdown_core_isdigit((unsigned char)c)  ? MARKDOWN_CORE_ORDERED_LIST_VARIANT_DECIMAL
                         : end == begin + 1 && c != 'i' && c != 'I' ? MARKDOWN_CORE_ORDERED_LIST_VARIANT_ALPHA
                                                                    : MARKDOWN_CORE_ORDERED_LIST_VARIANT_ROMAN;
    if (committed && committed->flavor == MARKDOWN_CORE_LIST_FLAVOR_ORDERED &&
        ordered_numeral(counts, line, begin, end, committed->variant, &data->start)) {
        data->variant = committed->variant;
    } else if (!ordered_numeral(counts, line, begin, end, data->variant, &data->start)) {
        return markdown_core_nok(tokenizer);
    }
    if (c == '#' && period &&
        (!committed || committed->delimiter.kind == MARKDOWN_CORE_ORDERED_LIST_DELIMITER_DEFAULT)) {
        data->delimiter.kind = MARKDOWN_CORE_ORDERED_LIST_DELIMITER_DEFAULT;
    }
    if (turn->context->paragraph && data->start != 1) {
        return markdown_core_nok(tokenizer);
    }
    if ((!committed || !markdown_core_block_list_facts_match(committed, data)) && data->start != 1 &&
        markdown_core_parser_inside(turn->parser, container,
                                    markdown_core_node_block_kind_bit(MARKDOWN_CORE_NODE_LIST_ITEM) |
                                        markdown_core_node_block_kind_bit(MARKDOWN_CORE_NODE_SPECIMEN))) {
        return markdown_core_nok(tokenizer);
    }
    if (end == begin + 1 && c >= 'A' && c <= 'Z' && period) {
        return markdown_core_check(tokenizer, &LIST_WIDE, markdown_core_state_of(S_marked),
                                   markdown_core_state_of(S_refused), code);
    }
    return S_marked(tokenizer, code);
}

static markdown_core_state S_delimiter(markdown_core_tokenizer *tokenizer, int code) {
    list_found *found = S_found(tokenizer);
    if (code != ')' && (found->closed || code != '.')) {
        return markdown_core_nok(tokenizer);
    }
    found->end = S_offset(tokenizer);
    found->list.flavor = MARKDOWN_CORE_LIST_FLAVOR_ORDERED;
    found->list.delimiter = (markdown_core_ordered_list_delimiter){
        code == '.' ? MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PERIOD : MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PARENTHESIS,
        found->closed};
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_delimited);
}

/* The numeral is read by the class its first byte names, which is the set
 * ordered_numeral accepts: one automatic marker, decimal digits (ten fail
 * there as surely as here), or letters -- ONE, or roman letters of the
 * first one's case. A run that leaves its class was refused by the
 * numeral, so the reading that stops at the class boundary refuses it at
 * the delimiter instead, and a prose word that reached this far ends the
 * reading on its second byte rather than at its end. */
static markdown_core_state S_digits(markdown_core_tokenizer *tokenizer, int code) {
    if (code >= '0' && code <= '9' && S_offset(tokenizer) - S_found(tokenizer)->begin < 10) {
        S_count(tokenizer);
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_digits);
    }
    return S_delimiter(tokenizer, code);
}

static markdown_core_state S_letters(markdown_core_tokenizer *tokenizer, int code) {
    const int first = S_found(tokenizer)->first;
    const unsigned char offset = first >= 'a' ? 'a' - 'A' : 0;
    if (code >= 0 && code < 0x100 && roman_letter((unsigned char)(first - offset)) &&
        roman_letter((unsigned char)(code - offset))) {
        S_count(tokenizer);
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_letters);
    }
    return S_delimiter(tokenizer, code);
}

static markdown_core_state S_numeral(markdown_core_tokenizer *tokenizer, int code) {
    list_found *found = S_found(tokenizer);
    found->begin = S_offset(tokenizer);
    found->first = code;
    if (code == '#') {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_delimiter);
    }
    if (code >= '0' && code <= '9') {
        S_count(tokenizer);
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_digits);
    }
    if (code >= 0 && code < 0x80 && markdown_core_isalpha((unsigned char)code)) {
        S_count(tokenizer);
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_letters);
    }
    return markdown_core_nok(tokenizer);
}

static markdown_core_state S_marker(markdown_core_tokenizer *tokenizer, int code) {
    list_found *found = S_found(tokenizer);
    const uint32_t from = found->from;
    *found = (list_found){.marker = S_offset(tokenizer), .column = markdown_core_column(tokenizer), .from = from};
    S_count(tokenizer);
    if (code == '*' || code == '-' || code == '+') {
        found->list.flavor = MARKDOWN_CORE_LIST_FLAVOR_BULLET;
        found->list.bullet_char = (unsigned char)code;
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_marked);
    }
    if (code == '(') {
        found->closed = true;
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_numeral);
    }
    return S_numeral(tokenizer, code);
}

static markdown_core_state markdown_core_list_scan(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_block_turn_of(tokenizer)->start->kind = MARKDOWN_CORE_NODE_LIST;
    S_found(tokenizer)->from = markdown_core_column(tokenizer);
    return markdown_core_spaces(tokenizer, markdown_core_state_of(S_marker), MARKDOWN_CORE_TOKEN_SPACE, 3, code);
}

/* THE PADDING (block_internal.h): one to four columns of space, not
 * followed by the line's end, are the item's padding; else the one column
 * the reading then reads, if a space. */
static markdown_core_state S_pad_end(markdown_core_tokenizer *tokenizer, int code) {
    const uint32_t padding = markdown_core_column(tokenizer) - markdown_core_frame_local(tokenizer)[0];
    return padding >= 1 && padding < 5 && !markdown_core_code_ending(code) ? markdown_core_ok(tokenizer)
                                                                           : markdown_core_nok(tokenizer);
}

static markdown_core_state S_pad(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_frame_local(tokenizer)[0] = markdown_core_column(tokenizer);
    return markdown_core_spaces(tokenizer, markdown_core_state_of(S_pad_end), 0, 5, code);
}

const markdown_core_construct markdown_core_item_padding = {.start = S_pad};

/* THE CONTINUATION (element.h, markdown_core_line_ops `continuation`): an
 * item goes on past its indent, its marker's columns and padding, or on a
 * blank line once it holds a block or a lookahead joins it; a list goes on
 * through one blank line and ends at a second one with no indent. */
static markdown_core_state S_blank_on(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    markdown_core_block_turn_of(tokenizer)->node->flags |= MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK;
    return markdown_core_ok(tokenizer);
}

static markdown_core_state S_blank_off(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    markdown_core_block_turn_of(tokenizer)->node->flags &= ~MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK;
    return markdown_core_ok(tokenizer);
}

static markdown_core_state S_continue(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    markdown_core_node *node = turn->node;
    if (node->kind == MARKDOWN_CORE_NODE_LIST_ITEM) {
        return markdown_core_block_indented(tokenizer,
                                            (uint32_t)(node->as.list->marker_offset + node->as.list->padding),
                                            markdown_core_parser_last_child(node) || turn->joining == node, code);
    }
    if (markdown_core_code_ending(code) && (node->flags & MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK)) {
        return markdown_core_block_continued(tokenizer, MARKDOWN_CORE_TOKEN_TAKE);
    }
    return markdown_core_check(tokenizer, &markdown_core_blank_line, markdown_core_state_of(S_blank_on),
                               markdown_core_state_of(S_blank_off), code);
}

static const markdown_core_node_type LIST_KINDS[] = {MARKDOWN_CORE_NODE_LIST, MARKDOWN_CORE_NODE_NONE};
/* A list reopened at an item's line (blocks.c, THE NESTED REOPEN) is laid
 * out by none of its items yet, and its last line was blank as the line
 * before the item's was; an item keeps no state of its own. */
static void reopen_block(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         markdown_core_node *node, const markdown_core_node *next, bool blank) {
    (void)self;
    (void)parser;
    (void)next;
    if (node->kind != MARKDOWN_CORE_NODE_LIST) {
        return;
    }
    node->flags &=
        (markdown_core_node_internal_flags) ~(MARKDOWN_CORE_NODE__LIST_LOOSE | MARKDOWN_CORE_NODE__LIST_LOOSE_FOLLOWED |
                                              MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK);
    if (blank) {
        node->flags |= MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK;
    }
    node->as.list->tight = false;
}
static bool blank_line(const markdown_core_element_instance *self, markdown_core_parser *parser,
                       markdown_core_node *node, markdown_core_reader *reader) {
    (void)self;
    return !(node->kind == MARKDOWN_CORE_NODE_LIST_ITEM && !markdown_core_parser_last_child(node) &&
             markdown_core_reader_starts(reader, parser, node, reader->number));
}

static const markdown_core_edit_ops LIST_EDIT = {
    .reopens = true,
    .reopen_block = reopen_block,
    .abuts = LIST_KINDS,
    /* A line after it, past blank lines, may still be its next item, and a
     * list it does not continue takes what its marker leaves open from it
     * (markdown_core_list_open). */
    .awaits = LIST_KINDS,
};

static const markdown_core_fold_ops LIST_FOLD = {
    .child = fold_child,
    .flags = MARKDOWN_CORE_NODE__LIST_LOOSE | MARKDOWN_CORE_NODE__LIST_LOOSE_FOLLOWED,
    .kinds = LIST_KINDS,
};

static const markdown_core_block_construct LIST_STARTS[] = {
    {.bytes = "*-+(#0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz",
     .indent = 3,
     .construct = {.start = markdown_core_list_scan}},
};

static const markdown_core_compile_ops LIST_BLOCK_TOKENS[LIST_BLOCKS] = {
    [LIST_ITEM] = {.enter = S_list_item},
};

static const markdown_core_open_ops LIST_OPENS = {
    .starts = LIST_STARTS,
    .start_count = 1,
    .tokens = LIST_BLOCK_TOKENS,
    .token_count = LIST_BLOCKS,
};

static const markdown_core_line_ops LIST_LINES = {
    .continuation = {.start = S_continue},
    .blank_last = blank_line,
    .speculative = MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK,
    .blank_spreads = true,
    .blank_runs = true,
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_LIST = {
    .edit = &LIST_EDIT,
    .state_size = sizeof(markdown_core_list_work),
    .name = "list",
    .fold = &LIST_FOLD,
    .opens = &LIST_OPENS,
    .lines = &LIST_LINES,
};

/* A list's record (markdown_core_record_ops): a bullet list of decimal
 * numbers until its first item says otherwise. */
static void init_list(markdown_core_node *node) {
    node->as.list->flavor = MARKDOWN_CORE_LIST_FLAVOR_BULLET;
    node->as.list->variant.kind = MARKDOWN_CORE_ORDERED_LIST_VARIANT_DECIMAL;
}

const markdown_core_record_ops MARKDOWN_CORE_RECORD_LIST = {
    .size = sizeof(markdown_core_list),
    .init = init_list,
};

/* An item's record, a list's, which owns its task marker. */
static void free_item(markdown_core_node_pool *pool, markdown_core_node *node) {
    (void)pool;
    markdown_core_optional_chunk_free(&node->as.list->task_marker);
}

const markdown_core_record_ops MARKDOWN_CORE_RECORD_LIST_ITEM = {
    .size = sizeof(markdown_core_list),
    .free = free_item,
};
