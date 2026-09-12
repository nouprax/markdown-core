#include "list.h"
#include "tasklist.h"
#define BLOCK_PEEK(input, at) ((input)->data[(at)])
#include "block_internal.h"

static bool markdown_core_block_list_facts_match(const markdown_core_list *list, const markdown_core_list *item);
static bufsize_t markdown_core_block_parse_list_marker(markdown_core_parser *parser, markdown_core_chunk *input,
                                                       bufsize_t pos, markdown_core_node *container, int first_column,
                                                       bool interrupts_paragraph, markdown_core_list *data);
static bool markdown_core_list_scan(markdown_core_parser *parser, block_start_context *context, block_start *start);
static bool ordered_numeral(markdown_core_parser *parser, markdown_core_chunk *input, bufsize_t begin, bufsize_t end,
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
            parser->list_marker_work++;
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
                parser->list_marker_work++;
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
    return list->list_type == item->list_type && list->bullet_char == item->bullet_char &&
           list->variant.kind == item->variant.kind && list->variant.lowercased == item->variant.lowercased &&
           list->delimiter.kind == item->delimiter.kind && list->delimiter.closed == item->delimiter.closed;
}

static bufsize_t markdown_core_block_parse_list_marker(markdown_core_parser *parser, markdown_core_chunk *input,
                                                       bufsize_t pos, markdown_core_node *container, int first_column,
                                                       bool interrupts_paragraph, markdown_core_list *data) {
    bufsize_t startpos = pos;
    unsigned char c = BLOCK_PEEK(input, pos);
    const markdown_core_list *committed = container->kind == MARKDOWN_CORE_NODE_LIST ? container->as.list : NULL;
    *data = (markdown_core_list){0};
    parser->list_marker_work++;
    if (c == '*' || c == '-' || c == '+') {
        data->list_type = MARKDOWN_CORE_BULLET_LIST;
        data->bullet_char = c;
        pos++;
    } else {
        bool closed = c == '(';
        pos += closed;
        bufsize_t begin = pos;
        c = BLOCK_PEEK(input, pos);
        if (c == '#') {
            pos++;
        } else {
            while (markdown_core_isalnum(BLOCK_PEEK(input, pos))) {
                parser->list_marker_work++;
                pos++;
            }
        }
        if (pos == begin) {
            return 0;
        }
        bufsize_t end = pos;
        unsigned char delim = BLOCK_PEEK(input, pos++);
        if (delim != ')' && (closed || delim != '.')) {
            return 0;
        }
        data->list_type = MARKDOWN_CORE_ORDERED_LIST;
        data->delimiter =
            (markdown_core_ordered_list_delimiter){delim == '.' ? MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PERIOD
                                                                : MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PARENTHESIS,
                                                   closed};
        data->variant.lowercased = c >= 'a' && c <= 'z';
        data->variant.kind = c == '#'                                   ? MARKDOWN_CORE_ORDERED_LIST_VARIANT_DEFAULT
                             : markdown_core_isdigit(c)                 ? MARKDOWN_CORE_ORDERED_LIST_VARIANT_DECIMAL
                             : end == begin + 1 && c != 'i' && c != 'I' ? MARKDOWN_CORE_ORDERED_LIST_VARIANT_ALPHA
                                                                        : MARKDOWN_CORE_ORDERED_LIST_VARIANT_ROMAN;
        if (committed && committed->list_type == MARKDOWN_CORE_ORDERED_LIST &&
            ordered_numeral(parser, input, begin, end, committed->variant, &data->start)) {
            data->variant = committed->variant;
        } else if (!ordered_numeral(parser, input, begin, end, data->variant, &data->start)) {
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
            while (markdown_core_block_is_space_or_tab(BLOCK_PEEK(input, at))) {
                parser->list_marker_work++;
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
    if (!markdown_core_isspace(BLOCK_PEEK(input, pos))) {
        return 0;
    }
    if (interrupts_paragraph) {
        bufsize_t at = pos;
        while (markdown_core_block_is_space_or_tab(BLOCK_PEEK(input, at))) {
            parser->list_marker_work++;
            at++;
        }
        if (markdown_core_is_line_end(BLOCK_PEEK(input, at))) {
            return 0;
        }
    }
    return pos - startpos;
}

void markdown_core_block_finalize_list(markdown_core_node *list) {
    list->as.list->tight = true;
    for (markdown_core_node *item = list->first_child; item; item = item->next) {
        if (markdown_core_block_last_line_blank(item) && item->next) {
            list->as.list->tight = false;
            return;
        }
        for (markdown_core_node *child = item->first_child; child; child = child->next) {
            if ((item->next || child->next) && markdown_core_block_ends_with_blank_line(child)) {
                list->as.list->tight = false;
                return;
            }
        }
    }
}

static bool markdown_core_list_open(markdown_core_parser *parser, markdown_core_node **container,
                                    markdown_core_chunk *input, block_start *start) {
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
    if (parser->oom) {
        return false;
    }
    return true;
}

static bool markdown_core_list_scan(markdown_core_parser *parser, block_start_context *context, block_start *start) {
    markdown_core_chunk *input = context->input;
    int first = context->first;
    if (!((start->matched = markdown_core_block_parse_list_marker(
               parser, input, first, context->container, context->column, context->paragraph, &start->list)))) {
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
        container->flags |= MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK;
    } else {
        container->flags &= ~MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK;
    }
    return true;
}

static bool continue_container(markdown_core_parser *parser, markdown_core_node *node, markdown_core_chunk *input,
                               const markdown_core_node *joining, bool *taken) {
    return markdown_core_list_continue(parser, node, input, joining, taken);
}
static void complete_block(markdown_core_parser *parser, markdown_core_node *node) {
    if (node->kind == MARKDOWN_CORE_NODE_LIST) {
        markdown_core_block_finalize_list(node);
    }
}
static bool blank_line(markdown_core_parser *parser, markdown_core_node *node) {
    return !(node->kind == MARKDOWN_CORE_NODE_LIST_ITEM && !node->first_child &&
             node->start_line == parser->line_number);
}

const markdown_core_extension MARKDOWN_CORE_EXTENSION_LIST = {
    .complete_block = complete_block,
    .blank_line = blank_line,
    .speculative_flags = MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK,

    .name = "list",
    .continue_container = continue_container,
    .propagates_child_blank = true,
    .blank_runs = true,
    .maximum_block_indent = 3,
    .scan_block_start = markdown_core_list_scan,
};

int markdown_core_block_consume_item_marker(markdown_core_parser *parser, markdown_core_chunk *input,
                                            int marker_width) {
    markdown_core_block_advance_offset(parser, input, parser->first_nonspace + marker_width - parser->offset, false);
    int offset = parser->offset, column = parser->column;
    bool partial = parser->partially_consumed_tab;
    while (parser->column - column < 5 && markdown_core_block_is_space_or_tab(input->data[parser->offset])) {
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
