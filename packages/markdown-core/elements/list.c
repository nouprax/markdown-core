#include "list.h"
#include "tasklist.h"
#include "block_internal.h"

MARKDOWN_CORE_BLOCK_FOUND_FITS(markdown_core_list, list_marker_fits);

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
                                                       markdown_core_reader *reader, bufsize_t pos,
                                                       markdown_core_node *container, int first_column,
                                                       bool interrupts_paragraph, markdown_core_list *data);
/* What the open reads past the marker (block_start.reads): its padding,
 * the item's first non-space byte and a task prefix there. */
static void S_list_reads(const markdown_core_element_instance *self, markdown_core_reader *reader,
                         const block_start *start) {
    (void)self;
    markdown_core_block_consume_item_marker(reader, start->matched);
    markdown_core_reader_find(reader);
    bufsize_t width;
    (void)markdown_core_task_prefix_end(reader, &width);
}

static bool markdown_core_list_scan(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                    block_start_context *context, block_start *start);
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

static bufsize_t markdown_core_block_parse_list_marker(markdown_core_list_work *counts, markdown_core_parser *parser,
                                                       markdown_core_reader *reader, bufsize_t pos,
                                                       markdown_core_node *container, int first_column,
                                                       bool interrupts_paragraph, markdown_core_list *data) {
    bufsize_t startpos = pos;
    unsigned char c = markdown_core_reader_peek(reader, pos);
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
        c = markdown_core_reader_peek(reader, pos);
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
            while (markdown_core_isdigit(markdown_core_reader_peek(reader, pos)) && pos - begin < 10) {
                counts->markers++;
                pos++;
            }
        } else if (markdown_core_isalpha(c)) {
            unsigned char offset = c >= 'a' ? 'a' - 'A' : 0;
            do {
                counts->markers++;
                pos++;
            } while (roman_letter((unsigned char)(c - offset)) &&
                     roman_letter((unsigned char)(markdown_core_reader_peek(reader, pos) - offset)));
        }
        if (pos == begin) {
            return 0;
        }
        bufsize_t end = pos;
        unsigned char delim = markdown_core_reader_peek(reader, pos++);
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
            ordered_numeral(counts, markdown_core_reader_bytes(reader, 0), begin, end, committed->variant,
                            &data->start)) {
            data->variant = committed->variant;
        } else if (!ordered_numeral(counts, markdown_core_reader_bytes(reader, 0), begin, end, data->variant,
                                    &data->start)) {
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
            while (markdown_core_is_space_or_tab(markdown_core_reader_peek(reader, at))) {
                counts->markers++;
                column += markdown_core_reader_peek(reader, at++) == '\t' ? 4 - column % 4 : 1;
                if (column - initial_column >= 2) {
                    break;
                }
            }
            if (column - initial_column < 2 && !markdown_core_is_line_end(markdown_core_reader_peek(reader, at))) {
                return 0;
            }
        }
    }
    if (!markdown_core_is_whitespace(markdown_core_reader_peek(reader, pos))) {
        return 0;
    }
    if (interrupts_paragraph) {
        bufsize_t at = pos;
        while (markdown_core_is_space_or_tab(markdown_core_reader_peek(reader, at))) {
            counts->markers++;
            at++;
        }
        if (markdown_core_is_line_end(markdown_core_reader_peek(reader, at))) {
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

static bool markdown_core_list_open(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                    markdown_core_node **container, markdown_core_reader *reader, block_start *start) {
    (void)self;
    bufsize_t matched = start->matched;
    markdown_core_list *data = MARKDOWN_CORE_BLOCK_FOUND(start, markdown_core_list);
    markdown_core_node_type cont_type = (*container)->kind;

    data->padding = markdown_core_block_consume_item_marker(reader, matched);

    // check container; if it's a list, see if this list item
    // can continue the list; otherwise, create a list container.

    data->marker_offset = reader->indent;

    if (cont_type != MARKDOWN_CORE_NODE_LIST || !markdown_core_block_list_facts_match((*container)->as.list, data)) {
        /* The open list it does not continue gave it what its marker
         * leaves open, as a variant: its opener reads that list. */
        if (cont_type == MARKDOWN_CORE_NODE_LIST) {
            markdown_core_reader_open(reader, *container);
        }
        *container = markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_LIST, reader->nonspace + 1);
        if (!*container) {
            return false;
        }

        memcpy((*container)->as.list, data, sizeof(*data));
    }

    // add the list item
    *container = markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_LIST_ITEM, reader->nonspace + 1);
    if (!*container) {
        return false;
    }
    memcpy((*container)->as.list, data, sizeof(*data));
    markdown_core_reader_find(reader);
    markdown_core_parse_task_prefix(parser, *container, reader);
    if (parser->error) {
        return false;
    }
    return true;
}

static bool markdown_core_list_scan(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                    block_start_context *context, block_start *start) {
    if (!((start->matched = markdown_core_block_parse_list_marker(
               self->state, parser, context->reader, context->reader->nonspace, context->container,
               context->reader->nonspace_column, context->paragraph,
               MARKDOWN_CORE_BLOCK_FOUND(start, markdown_core_list))))) {
        return false;
    }
    start->kind = MARKDOWN_CORE_NODE_LIST;
    start->open = markdown_core_list_open;
    start->reads = S_list_reads;
    return true;
}

bool markdown_core_list_continue(markdown_core_parser *parser, markdown_core_node *container,
                                 markdown_core_reader *reader, const markdown_core_node *joining, bool *taken) {
    if (container->kind == MARKDOWN_CORE_NODE_LIST_ITEM) {
        return markdown_core_reader_indented(reader, container->as.list->marker_offset + container->as.list->padding,
                                             markdown_core_parser_last_child(container) != NULL ||
                                                 joining == container);
    }
    if (reader->blank) {
        if ((container->flags & MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK) && reader->indent == 0) {
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
                               markdown_core_node *node, markdown_core_reader *reader,
                               const markdown_core_node *joining, bool *taken) {
    (void)self;
    return markdown_core_list_continue(parser, node, reader, joining, taken);
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

static const markdown_core_open_ops LIST_OPENS = {
    .indent = 3,
    .scan = markdown_core_list_scan,
    /* A bullet, or an ordered marker: a numeral or `#`, closed or not. */
    .scan_needs = {.bytes = "*-+(#0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"},
};

static const markdown_core_line_ops LIST_LINES = {
    .blank_last = blank_line,
    .speculative = MARKDOWN_CORE_NODE__LIST_LAST_LINE_BLANK,
    .prefix = continue_container,
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

int markdown_core_block_consume_item_marker(markdown_core_reader *reader, int marker_width) {
    markdown_core_reader_advance(reader, reader->nonspace + marker_width - reader->offset, false);
    const markdown_core_reader_mark mark = markdown_core_reader_tell(reader);
    const bufsize_t column = reader->column;
    while (reader->column - column < 5 &&
           markdown_core_is_space_or_tab(markdown_core_reader_peek(reader, reader->offset))) {
        markdown_core_reader_advance(reader, 1, true);
    }
    int padding = reader->column - column;
    if (padding < 1 || padding >= 5 || markdown_core_is_line_end(markdown_core_reader_peek(reader, reader->offset))) {
        markdown_core_reader_back(reader, mark);
        if (padding > 0) {
            markdown_core_reader_advance(reader, 1, true);
        }
        padding = 1;
    }
    return marker_width + padding;
}

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
