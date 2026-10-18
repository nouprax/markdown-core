#include "list.h"
#include "tasklist.h"
#define BLOCK_PEEK(input, at) ((input)->data[(at)])
#include "block_internal.h"

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
static bufsize_t markdown_core_block_parse_list_marker(markdown_core_list_work *counts, markdown_core_parser *parser,
                                                       markdown_core_chunk *input, bufsize_t pos,
                                                       markdown_core_node *container, int first_column,
                                                       bool interrupts_paragraph, markdown_core_list *data);
static bool markdown_core_list_scan(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                    block_start_context *context, block_start *start);
/* The upper-case roman letters: the terms of ordered_numeral's roman variant. */
static bool roman_letter(unsigned char c) { return c && strchr("MDCLXVI", c) != NULL; }
static bool ordered_numeral(markdown_core_list_work *counts, markdown_core_chunk *input, bufsize_t begin, bufsize_t end,
                            markdown_core_ordered_list_variant variant, int *value) {
    int number = 0;
    // An automatic marker has value 1 in every variant, including the
    // variant inherited from a preceding authored marker.
    if (end == begin + 1 && input->data[begin] == '#') {
        *value = 1;
        return true;
    }
    if (variant.kind == MARKDOWN_CORE_ORDERED_LIST_VARIANT_DEFAULT) {
        return false;
    }
    if (variant.kind == MARKDOWN_CORE_ORDERED_LIST_VARIANT_ALPHA) {
        unsigned char first = variant.lowercased ? 'a' : 'A';
        if (end != begin + 1 || input->data[begin] < first || input->data[begin] > first + 25) {
            return false;
        }
        *value = input->data[begin] - first + 1;
        return true;
    }
    if (variant.kind == MARKDOWN_CORE_ORDERED_LIST_VARIANT_DECIMAL) {
        if (end - begin > 9) {
            return false;
        }
        for (bufsize_t at = begin; at < end; at++) {
            counts->markers++;
            if (!markdown_core_isdigit(input->data[at])) {
                return false;
            }
            number = number * 10 + input->data[at] - '0';
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
                if (input->data[at] != terms[term].text[0] + offset ||
                    (width == 2 && input->data[at + 1] != terms[term].text[1] + offset)) {
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

static bufsize_t markdown_core_block_parse_list_marker(markdown_core_list_work *counts, markdown_core_parser *parser,
                                                       markdown_core_chunk *input, bufsize_t pos,
                                                       markdown_core_node *container, int first_column,
                                                       bool interrupts_paragraph, markdown_core_list *data) {
    bufsize_t startpos = pos;
    unsigned char c = BLOCK_PEEK(input, pos);
    const markdown_core_list *committed = container->kind == MARKDOWN_CORE_NODE_LIST ? container->as.list : NULL;
    *data = (markdown_core_list){0};
    counts->markers++;
    if (c == '*' || c == '-' || c == '+') {
        data->flavor = MARKDOWN_CORE_LIST_FLAVOR_BULLET;
        data->bullet_char = c;
        pos++;
    } else {
        bool closed = c == '(';
        pos += closed;
        bufsize_t begin = pos;
        c = BLOCK_PEEK(input, pos);
        /* The numeral is scanned by the class its first byte names, which is
         * the set ordered_numeral accepts: one automatic marker, decimal
         * digits (ten fail there as surely as here), or letters -- ONE, or
         * roman letters of the first one's case. A run that leaves its class
         * was refused by the numeral, so the scan that stops at the class
         * boundary refuses it at the delimiter instead, and a prose word
         * that reached this far ends the probe on its second byte rather
         * than at its end. */
        if (c == '#') {
            pos++;
        } else if (markdown_core_isdigit(c)) {
            while (markdown_core_isdigit(BLOCK_PEEK(input, pos)) && pos - begin < 10) {
                counts->markers++;
                pos++;
            }
        } else if (markdown_core_isalpha(c)) {
            unsigned char offset = c >= 'a' ? 'a' - 'A' : 0;
            do {
                counts->markers++;
                pos++;
            } while (roman_letter((unsigned char)(c - offset)) &&
                     roman_letter((unsigned char)(BLOCK_PEEK(input, pos) - offset)));
        }
        if (pos == begin) {
            return 0;
        }
        bufsize_t end = pos;
        unsigned char delim = BLOCK_PEEK(input, pos++);
        if (delim != ')' && (closed || delim != '.')) {
            return 0;
        }
        data->flavor = MARKDOWN_CORE_LIST_FLAVOR_ORDERED;
        data->delimiter =
            (markdown_core_ordered_list_delimiter){delim == '.' ? MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PERIOD
                                                                : MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PARENTHESIS,
                                                   closed};
        data->variant.lowercased = c >= 'a' && c <= 'z';
        data->variant.kind = c == '#'                                   ? MARKDOWN_CORE_ORDERED_LIST_VARIANT_DEFAULT
                             : markdown_core_isdigit(c)                 ? MARKDOWN_CORE_ORDERED_LIST_VARIANT_DECIMAL
                             : end == begin + 1 && c != 'i' && c != 'I' ? MARKDOWN_CORE_ORDERED_LIST_VARIANT_ALPHA
                                                                        : MARKDOWN_CORE_ORDERED_LIST_VARIANT_ROMAN;
        if (committed && committed->flavor == MARKDOWN_CORE_LIST_FLAVOR_ORDERED &&
            ordered_numeral(counts, input, begin, end, committed->variant, &data->start)) {
            data->variant = committed->variant;
        } else if (!ordered_numeral(counts, input, begin, end, data->variant, &data->start)) {
            return 0;
        }
        if (c == '#' && delim == '.' &&
            (!committed || committed->delimiter.kind == MARKDOWN_CORE_ORDERED_LIST_DELIMITER_DEFAULT)) {
            data->delimiter.kind = MARKDOWN_CORE_ORDERED_LIST_DELIMITER_DEFAULT;
        }
        if (interrupts_paragraph && data->start != 1) {
            return 0;
        }
        if (!committed || !markdown_core_block_list_facts_match(committed, data)) {
            if (data->start != 1 &&
                markdown_core_parser_inside(parser, container,
                                            markdown_core_node_block_kind_bit(MARKDOWN_CORE_NODE_LIST_ITEM) |
                                                markdown_core_node_block_kind_bit(MARKDOWN_CORE_NODE_SPECIMEN))) {
                return 0;
            }
        }
        if (end == begin + 1 && c >= 'A' && c <= 'Z' && delim == '.') {
            int column = first_column + (pos - startpos);
            int initial_column = column;
            bufsize_t at = pos;
            while (markdown_core_is_space_or_tab(BLOCK_PEEK(input, at))) {
                counts->markers++;
                column += input->data[at++] == '\t' ? 4 - column % 4 : 1;
                if (column - initial_column >= 2) {
                    break;
                }
            }
            if (column - initial_column < 2 && !markdown_core_is_line_end(BLOCK_PEEK(input, at))) {
                return 0;
            }
        }
    }
    if (!markdown_core_is_whitespace(BLOCK_PEEK(input, pos))) {
        return 0;
    }
    if (interrupts_paragraph) {
        bufsize_t at = pos;
        while (markdown_core_is_space_or_tab(BLOCK_PEEK(input, at))) {
            counts->markers++;
            at++;
        }
        if (markdown_core_is_line_end(BLOCK_PEEK(input, at))) {
            return 0;
        }
    }
    return pos - startpos;
}

/* A LIST IS LOOSE when a blank line separates two of its items or two of
 * the blocks an item holds that the blank-line facts see: an item that is
 * not the last ends with one, or a child that some block follows does. Each
 * item folds into the list as it settles, its children having folded into it
 * (blocks.c, S_fold): one that BLANK_SPACED marks makes the list loose, and
 * one whose last line, or the last child it sees, ends blank makes it loose
 * once another item follows. */
static void fold_child(const markdown_core_element_instance *self, markdown_core_parser *parser,
                       markdown_core_node *node, const markdown_core_node *child) {
    (void)self;
    (void)parser;
    assert(node->kind == MARKDOWN_CORE_NODE_LIST);
    markdown_core_node_internal_flags flags = node->flags;
    if (flags & MARKDOWN_CORE_NODE__LIST_LOOSE_FOLLOWED || child->flags & MARKDOWN_CORE_NODE__BLANK_SPACED) {
        flags |= MARKDOWN_CORE_NODE__LIST_LOOSE;
    }
    const markdown_core_node_internal_flags ends = MARKDOWN_CORE_NODE__BLANK_FOLDED | MARKDOWN_CORE_NODE__ENDS_BLANK;
    const bool followed_loose = (child->flags & MARKDOWN_CORE_NODE__LAST_LINE_BLANK) || (child->flags & ends) == ends;
    node->flags = (markdown_core_node_internal_flags)((flags & ~MARKDOWN_CORE_NODE__LIST_LOOSE_FOLLOWED) |
                                                      (followed_loose ? MARKDOWN_CORE_NODE__LIST_LOOSE_FOLLOWED : 0));
}

static bool markdown_core_list_open(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                    markdown_core_node **container, markdown_core_chunk *input, block_start *start) {
    (void)self;
    bufsize_t matched = start->matched;
    markdown_core_list *data = &start->list;
    markdown_core_node_type cont_type = (*container)->kind;

    data->padding = markdown_core_block_consume_item_marker(parser, input, matched);

    // check container; if it's a list, see if this list item
    // can continue the list; otherwise, create a list container.

    data->marker_offset = parser->indent;

    if (cont_type != MARKDOWN_CORE_NODE_LIST || !markdown_core_block_list_facts_match((*container)->as.list, data)) {
        *container =
            markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_LIST, parser->first_nonspace + 1);
        if (!*container) {
            return false;
        }

        memcpy((*container)->as.list, data, sizeof(*data));
    }

    // add the list item
    *container =
        markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_LIST_ITEM, parser->first_nonspace + 1);
    if (!*container) {
        return false;
    }
    memcpy((*container)->as.list, data, sizeof(*data));
    markdown_core_block_find_first_nonspace(parser, input);
    markdown_core_parse_task_prefix(parser, *container, input->data, input->len);
    if (parser->error) {
        return false;
    }
    return true;
}

static bool markdown_core_list_scan(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                    block_start_context *context, block_start *start) {
    markdown_core_chunk *input = context->input;
    int first = context->first;
    if (!((start->matched =
               markdown_core_block_parse_list_marker(self->state, parser, input, first, context->container,
                                                     context->column, context->paragraph, &start->list)))) {
        return false;
    }
    start->kind = MARKDOWN_CORE_NODE_LIST;
    start->open = markdown_core_list_open;
    return true;
}

bool markdown_core_list_continue(markdown_core_parser *parser, markdown_core_node *container,
                                 markdown_core_chunk *input, const markdown_core_node *joining, bool *taken) {
    if (container->kind == MARKDOWN_CORE_NODE_LIST_ITEM) {
        return markdown_core_block_continue_indented(
            parser, input, container->as.list->marker_offset + container->as.list->padding,
            markdown_core_parser_last_child(parser, container) != NULL || joining == container);
    }
    if (parser->blank) {
        if ((container->flags & MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK) && parser->indent == 0) {
            *taken = true;
            return true;
        }
        container->flags |= MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK;
    } else {
        container->flags &= ~MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK;
    }
    return true;
}

static bool continue_container(const markdown_core_element_instance *self, markdown_core_parser *parser,
                               markdown_core_node *node, markdown_core_chunk *input, const markdown_core_node *joining,
                               bool *taken) {
    (void)self;
    return markdown_core_list_continue(parser, node, input, joining, taken);
}
/* A LIST IS LAID OUT AT ITS EXIT, as completion reaches it: its items, which
 * settled before it, have folded into it whether it is loose. */
static markdown_core_finish_result finish_step(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                               markdown_core_node *node, markdown_core_node *parent,
                                               markdown_core_event_type event, void **state) {
    (void)self;
    (void)parser;
    (void)event;
    (void)parent;
    (void)state;
    assert(event == MARKDOWN_CORE_EVENT_EXIT && node->kind == MARKDOWN_CORE_NODE_LIST);
    node->as.list->tight = !(node->flags & MARKDOWN_CORE_NODE__LIST_LOOSE);
    return MARKDOWN_CORE_FINISH_CONTINUE;
}
static const markdown_core_node_type LIST_KINDS[] = {MARKDOWN_CORE_NODE_LIST, MARKDOWN_CORE_NODE_NONE};
static bool blank_line(const markdown_core_element_instance *self, markdown_core_parser *parser,
                       markdown_core_node *node) {
    (void)self;
    return !(node->kind == MARKDOWN_CORE_NODE_LIST_ITEM && !markdown_core_parser_last_child(parser, node) &&
             markdown_core_parser_starts_on_line(parser, node, parser->line_number));
}

const markdown_core_element MARKDOWN_CORE_ELEMENT_LIST = {
    .state_size = sizeof(markdown_core_list_work),
    .finish_step = finish_step,
    .finish_exit_kinds = LIST_KINDS,
    .fold_child = fold_child,
    .fold_kinds = LIST_KINDS,
    .blank_line = blank_line,
    .speculative_flags = MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK,

    .name = "list",
    .continue_container = continue_container,
    .propagates_child_blank = true,
    .continue_reach = markdown_core_block_indent_reach,
    .blank_runs = true,
    .maximum_block_indent = 3,
    .scan_block_start = markdown_core_list_scan,
    /* A bullet, or an ordered marker: a numeral or `#`, closed or not. */
    .scan_block_gate = {.bytes = "*-+(#0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"},
};

int markdown_core_block_consume_item_marker(markdown_core_parser *parser, markdown_core_chunk *input,
                                            int marker_width) {
    markdown_core_block_advance_offset(parser, input, parser->first_nonspace + marker_width - parser->offset, false);
    int offset = parser->offset, column = parser->column;
    bool partial = parser->partially_consumed_tab;
    while (parser->column - column < 5 && markdown_core_is_space_or_tab(input->data[parser->offset])) {
        markdown_core_block_advance_offset(parser, input, 1, true);
    }
    int padding = parser->column - column;
    if (padding < 1 || padding >= 5 || markdown_core_is_line_end(input->data[parser->offset])) {
        parser->offset = offset;
        parser->column = column;
        parser->partially_consumed_tab = partial;
        if (padding > 0) {
            markdown_core_block_advance_offset(parser, input, 1, true);
        }
        padding = 1;
    }
    return marker_width + padding;
}
