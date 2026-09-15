#include "document.h"
#include "attributes.h"
#include "code_block.h"
#include "html_block.h"
#include "paragraph.h"
#include "heading.h"
#include "thematic_break.h"
#include "node.h"
#include "code.h"
#include "emphasis.h"
#include "html.h"
#include "line_break.h"
#include "text.h"
#include "embedded.h"
#include "link.h"
#include "definition_list.h"
#include "callout.h"
#include "specimen.h"
#include "list.h"
#include "citation.h"
#include "mark.h"
#include "insertion.h"
#include "footnote.h"
#include "superscript.h"
#include "subscript.h"
#include <stddef.h>
#include "element.h"

#include "markdown-core-elements.h"
#include "autolink.h"
#include "strikethrough.h"
#include "table.h"
#include "formula.h"
#include "directive.h"
#include "comment.h"
#include "cross_link.h"

/* Every parse attaches the same elements. Scanner precedence is
 * source ownership: footnote before superscript, double tilde before single
 * tilde, opaque scanners before bracket alternatives, and table last. */
static const markdown_core_element *const CORE_ELEMENTS[] = {&MARKDOWN_CORE_ELEMENT_DOCUMENT,
                                                             &MARKDOWN_CORE_ELEMENT_ATTRIBUTES,
                                                             &MARKDOWN_CORE_ELEMENT_CODE_BLOCK,
                                                             &MARKDOWN_CORE_ELEMENT_CALLOUT,
                                                             &MARKDOWN_CORE_ELEMENT_HEADING,
                                                             &MARKDOWN_CORE_ELEMENT_HTML_BLOCK,
                                                             &MARKDOWN_CORE_ELEMENT_THEMATIC_BREAK,
                                                             &MARKDOWN_CORE_ELEMENT_PARAGRAPH,
                                                             &MARKDOWN_CORE_ELEMENT_CODE,
                                                             &MARKDOWN_CORE_ELEMENT_EMPHASIS,
                                                             &MARKDOWN_CORE_ELEMENT_EMPHASIS_UNDERSCORE,
                                                             &MARKDOWN_CORE_ELEMENT_LINE_BREAK,
                                                             &MARKDOWN_CORE_ELEMENT_DEFINITION_LIST,

                                                             &MARKDOWN_CORE_ELEMENT_STRIKETHROUGH,
                                                             &MARKDOWN_CORE_ELEMENT_MARK,
                                                             &MARKDOWN_CORE_ELEMENT_INSERTION,
                                                             &MARKDOWN_CORE_ELEMENT_CITATION,
                                                             &MARKDOWN_CORE_ELEMENT_FOOTNOTE,
                                                             &MARKDOWN_CORE_ELEMENT_SPECIMEN,
                                                             &MARKDOWN_CORE_ELEMENT_LIST,
                                                             &MARKDOWN_CORE_ELEMENT_SUPERSCRIPT,
                                                             &MARKDOWN_CORE_ELEMENT_SUBSCRIPT,
                                                             &MARKDOWN_CORE_ELEMENT_AUTOLINK,
                                                             &MARKDOWN_CORE_ELEMENT_FORMULA,
                                                             &MARKDOWN_CORE_ELEMENT_COMMENT,
                                                             &MARKDOWN_CORE_ELEMENT_CROSS_LINK,
                                                             &MARKDOWN_CORE_ELEMENT_DIRECTIVE,
                                                             &MARKDOWN_CORE_ELEMENT_HTML,
                                                             &MARKDOWN_CORE_ELEMENT_LINK,
                                                             &MARKDOWN_CORE_ELEMENT_EMBEDDED,
                                                             &MARKDOWN_CORE_ELEMENT_TEXT,
                                                             &MARKDOWN_CORE_ELEMENT_TABLE};

#define CORE_ELEMENT_COUNT (sizeof(CORE_ELEMENTS) / sizeof(CORE_ELEMENTS[0]))

const markdown_core_element *const *markdown_core_core_elements(size_t *count) {
    *count = CORE_ELEMENT_COUNT;
    return CORE_ELEMENTS;
}

static const markdown_core_element *const BLOCK_STRUCTURE[] = {
    [MARKDOWN_CORE_NODE_TABLE & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_TABLE,
    [MARKDOWN_CORE_NODE_TABLE_ROW & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_TABLE,
    [MARKDOWN_CORE_NODE_TABLE_CELL & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_TABLE,
    [MARKDOWN_CORE_NODE_FORMULA_BLOCK & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_FORMULA,
    [MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_DIRECTIVE,

    [MARKDOWN_CORE_NODE_DOCUMENT & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_DOCUMENT,
    [MARKDOWN_CORE_NODE_CALLOUT & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_CALLOUT,
    [MARKDOWN_CORE_NODE_LIST & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_LIST,
    [MARKDOWN_CORE_NODE_LIST_ITEM & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_LIST,
    [MARKDOWN_CORE_NODE_CODE_BLOCK & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_CODE_BLOCK,
    [MARKDOWN_CORE_NODE_HTML_BLOCK & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_HTML_BLOCK,
    [MARKDOWN_CORE_NODE_PARAGRAPH & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_PARAGRAPH,
    [MARKDOWN_CORE_NODE_HEADING & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_HEADING,
    [MARKDOWN_CORE_NODE_THEMATIC_BREAK & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_THEMATIC_BREAK,
    [MARKDOWN_CORE_NODE_FOOTNOTE & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_FOOTNOTE,
    [MARKDOWN_CORE_NODE_SPECIMEN & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_SPECIMEN,
    [MARKDOWN_CORE_NODE_DEFINITION_LIST & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_DEFINITION_LIST,
    [MARKDOWN_CORE_NODE_DEFINITION & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_DEFINITION_LIST,
    [MARKDOWN_CORE_NODE_DEFINITION_BODY & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_DEFINITION_LIST,
    [MARKDOWN_CORE_NODE_COMMENT_BLOCK & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_COMMENT,
    [MARKDOWN_CORE_NODE_TABLE_CAPTION & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_TABLE,
};

static const markdown_core_element *const INLINE_STRUCTURE[] = {
    [MARKDOWN_CORE_NODE_CITE & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_CITATION,
    [MARKDOWN_CORE_NODE_CITATION & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_CITATION,
    [MARKDOWN_CORE_NODE_COMMENT & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_COMMENT,
    [MARKDOWN_CORE_NODE_STRIKETHROUGH & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_STRIKETHROUGH,
    [MARKDOWN_CORE_NODE_FORMULA & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_FORMULA,
    [MARKDOWN_CORE_NODE_DIRECTIVE & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_DIRECTIVE,
    [MARKDOWN_CORE_NODE_DIRECTIVE_LABEL & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_DIRECTIVE,
    [MARKDOWN_CORE_NODE_CROSS_LINK & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_CROSS_LINK,
    [MARKDOWN_CORE_NODE_CROSS_EMBEDDED & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_CROSS_LINK,
    [MARKDOWN_CORE_NODE_MARK & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_MARK,
    [MARKDOWN_CORE_NODE_INSERTION & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_INSERTION,
    [MARKDOWN_CORE_NODE_SUPERSCRIPT & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_SUPERSCRIPT,
    [MARKDOWN_CORE_NODE_SUBSCRIPT & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_SUBSCRIPT,

    [MARKDOWN_CORE_NODE_TEXT & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_TEXT,
    [MARKDOWN_CORE_NODE_SOFT_BREAK & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_LINE_BREAK,
    [MARKDOWN_CORE_NODE_LINE_BREAK & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_LINE_BREAK,
    [MARKDOWN_CORE_NODE_CODE & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_CODE,
    [MARKDOWN_CORE_NODE_HTML & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_HTML,
    [MARKDOWN_CORE_NODE_EMPHASIS & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_EMPHASIS,
    [MARKDOWN_CORE_NODE_STRONG & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_EMPHASIS,
    [MARKDOWN_CORE_NODE_LINK & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_LINK,
    [MARKDOWN_CORE_NODE_EMBEDDED & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_ELEMENT_EMBEDDED,
};

const markdown_core_element *markdown_core_structure_for_kind(markdown_core_node_type kind) {
    unsigned index = kind & MARKDOWN_CORE_NODE_VALUE_MASK;
    if (MARKDOWN_CORE_NODE_TYPE_INLINE_P(kind)) {
        return index < sizeof(INLINE_STRUCTURE) / sizeof(*INLINE_STRUCTURE) ? INLINE_STRUCTURE[index] : NULL;
    }
    return index < sizeof(BLOCK_STRUCTURE) / sizeof(*BLOCK_STRUCTURE) ? BLOCK_STRUCTURE[index] : NULL;
}
const markdown_core_element *markdown_core_node_structure(const markdown_core_node *node) {
    return node ? markdown_core_structure_for_kind(node->kind) : NULL;
}
