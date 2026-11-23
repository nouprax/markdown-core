#include "anchor.h"

const markdown_core_name_spelling MARKDOWN_CORE_ANCHOR_SPELLING = {
    .empty = {.ptr = (unsigned char *)"section", .size = 7},
    .separator = '-',
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_ANCHOR = {
    .name = "anchor",
    .spelling = &MARKDOWN_CORE_ANCHOR_SPELLING,
};
