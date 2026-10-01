#include "list.h"
#include "tasklist.h"
#define BLOCK_PEEK(input, at) ((input)->data[(at)])
#include "block_internal.h"

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
            for (markdown_core_node *ancestor = container; ancestor; ancestor = ancestor->parent) {
                if ((ancestor->kind == MARKDOWN_CORE_NODE_LIST_ITEM || ancestor->kind == MARKDOWN_CORE_NODE_SPECIMEN) &&
                    data->start != 1) {
                    return 0;
                }
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
        return markdown_core_block_continue_indented(parser, input,
                                                     container->as.list->marker_offset + container->as.list->padding,
                                                     container->first_child != NULL || joining == container);
    }
    if (parser->blank) {
        if ((container->flags & MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK) && parser->indent == 0) {
            *taken = true;
            return true;
        }
        markdown_core_parser_set_flags(parser, container,
                                       (uint16_t)(container->flags | MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK));
    } else {
        markdown_core_parser_set_flags(parser, container,
                                       (uint16_t)(container->flags & ~MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK));
    }
    return true;
}

static bool continue_container(const markdown_core_element_instance *self, markdown_core_parser *parser,
                               markdown_core_node *node, markdown_core_chunk *input, const markdown_core_node *joining,
                               bool *taken) {
    (void)self;
    return markdown_core_list_continue(parser, node, input, joining, taken);
}
/* A LIST IS TIGHT OR LOOSE BY A FOLD OF ITS ITEMS (E4). An item's summary
 * as a child of its list is whether a blank line separates it from what
 * follows it: it ends with a blank line, or a child of it does, or any child
 * but its last ends with one -- the last item's is only the last of these,
 * as nothing follows it in the list. The list is loose when a summary is
 * set. An item folds its children by whether each ends with a blank line, so
 * those of an item are its totals.
 *
 * The folds run at the EXIT of a list and of an item that keeps its totals,
 * from inside the one finish walk, where their children are complete -- a
 * paragraph that was only definitions has been released at its own EXIT,
 * before this. */
static uint32_t fold_child(const markdown_core_element_instance *self, markdown_core_parser *parser,
                           markdown_core_node *container, markdown_core_node *child, bool last) {
    (void)self;
    if (container->kind == MARKDOWN_CORE_NODE_LIST_ITEM) {
        return markdown_core_block_ends_with_blank_line(parser, child);
    }
    uint32_t sum, end;
    markdown_core_parser_fold_totals(parser, child, &sum, &end);
    return last ? sum != 0 : markdown_core_block_last_line_blank(child) || sum || end;
}
static void fold_apply(const markdown_core_element_instance *self, markdown_core_parser *parser,
                       markdown_core_node *node, uint32_t sum, uint32_t last) {
    (void)self;
    (void)parser;
    if (node->kind == MARKDOWN_CORE_NODE_LIST) {
        node->as.list->tight = !sum && !last;
    }
}
/* AN OPEN LIST OR ITEM CARRIES what a later line asks of it in its fields,
 * which stay as they were set when it opened (E3): a list, the facts a new
 * item's marker must share to join it; an item, the indentation that
 * continues it. The word says them, and a reopened node still holds them,
 * so there is nothing to put back. Whether an item holds a block yet is its
 * children, which the spine and the block below it say. */
static uint64_t carry_save(const markdown_core_element_instance *self, const markdown_core_node *node) {
    (void)self;
    const markdown_core_list *list = node->as.list;
    if (node->kind == MARKDOWN_CORE_NODE_LIST_ITEM) {
        return (uint64_t)(list->marker_offset + list->padding);
    }
    return (uint64_t)list->flavor | (uint64_t)list->bullet_char << 2 | (uint64_t)list->variant.kind << 10 |
           (uint64_t)list->variant.lowercased << 13 | (uint64_t)list->delimiter.kind << 14 |
           (uint64_t)list->delimiter.closed << 16;
}
static markdown_core_finish_result finish_step(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                               markdown_core_node *node, markdown_core_event_type event, int is_root,
                                               void **state) {
    (void)self;
    (void)event;
    (void)is_root;
    (void)state;
    assert(event == MARKDOWN_CORE_EVENT_EXIT);
    if (node->kind == MARKDOWN_CORE_NODE_LIST || node->entry) {
        markdown_core_parser_fold(parser, node);
    }
    return MARKDOWN_CORE_FINISH_CONTINUE;
}
static const markdown_core_node_type LIST_EXIT_KINDS[] = {MARKDOWN_CORE_NODE_LIST, MARKDOWN_CORE_NODE_LIST_ITEM,
                                                          MARKDOWN_CORE_NODE_NONE};
static const markdown_core_node_type LIST_REOPEN_KINDS[] = {MARKDOWN_CORE_NODE_LIST, MARKDOWN_CORE_NODE_LIST_ITEM,
                                                            MARKDOWN_CORE_NODE_NONE};
static bool blank_line(const markdown_core_element_instance *self, markdown_core_parser *parser,
                       markdown_core_node *node) {
    (void)self;
    return !(node->kind == MARKDOWN_CORE_NODE_LIST_ITEM && !node->first_child &&
             markdown_core_parser_starts_on_line(parser, node, parser->line_number));
}

const markdown_core_element MARKDOWN_CORE_ELEMENT_LIST = {
    .state_size = sizeof(markdown_core_list_work),
    .finish_step = finish_step,
    .finish_exit_kinds = LIST_EXIT_KINDS,
    .blank_line = blank_line,
    .speculative_flags = MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK,
    .reopen_kinds = LIST_REOPEN_KINDS,
    .carry_save = carry_save,
    .fold_child = fold_child,
    .fold_apply = fold_apply,

    .name = "list",
    .continue_container = continue_container,
    .propagates_child_blank = true,
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
