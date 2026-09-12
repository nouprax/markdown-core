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
#include "media.h"
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
#include "extension.h"

#include "markdown-core-extensions.h"
#include "autolink.h"
#include "strikethrough.h"
#include "table.h"
#include "formula.h"
#include "directive.h"
#include "comment.h"
#include "cross_link.h"

/* Every parse attaches the same element extensions. Scanner precedence is
 * source ownership: footnote before superscript, double tilde before single
 * tilde, opaque scanners before bracket alternatives, and table last. */
static const markdown_core_extension *const CORE_EXTENSIONS[] = {&MARKDOWN_CORE_EXTENSION_DOCUMENT,
                                                                 &MARKDOWN_CORE_EXTENSION_ATTRIBUTES,
                                                                 &MARKDOWN_CORE_EXTENSION_CODE_BLOCK,
                                                                 &MARKDOWN_CORE_EXTENSION_CALLOUT,
                                                                 &MARKDOWN_CORE_EXTENSION_HEADING,
                                                                 &MARKDOWN_CORE_EXTENSION_HTML_BLOCK,
                                                                 &MARKDOWN_CORE_EXTENSION_THEMATIC_BREAK,
                                                                 &MARKDOWN_CORE_EXTENSION_PARAGRAPH,
                                                                 &MARKDOWN_CORE_EXTENSION_CODE,
                                                                 &MARKDOWN_CORE_EXTENSION_EMPHASIS,
                                                                 &MARKDOWN_CORE_EXTENSION_EMPHASIS_UNDERSCORE,
                                                                 &MARKDOWN_CORE_EXTENSION_LINE_BREAK,
                                                                 &MARKDOWN_CORE_EXTENSION_DEFINITION_LIST,

                                                                 &MARKDOWN_CORE_EXTENSION_STRIKETHROUGH,
                                                                 &MARKDOWN_CORE_EXTENSION_MARK,
                                                                 &MARKDOWN_CORE_EXTENSION_INSERTION,
                                                                 &MARKDOWN_CORE_EXTENSION_CITATION,
                                                                 &MARKDOWN_CORE_EXTENSION_FOOTNOTE,
                                                                 &MARKDOWN_CORE_EXTENSION_SPECIMEN,
                                                                 &MARKDOWN_CORE_EXTENSION_LIST,
                                                                 &MARKDOWN_CORE_EXTENSION_SUPERSCRIPT,
                                                                 &MARKDOWN_CORE_EXTENSION_SUBSCRIPT,
                                                                 &MARKDOWN_CORE_EXTENSION_AUTOLINK,
                                                                 &MARKDOWN_CORE_EXTENSION_FORMULA,
                                                                 &MARKDOWN_CORE_EXTENSION_COMMENT,
                                                                 &MARKDOWN_CORE_EXTENSION_CROSS_LINK,
                                                                 &MARKDOWN_CORE_EXTENSION_DIRECTIVE,
                                                                 &MARKDOWN_CORE_EXTENSION_HTML,
                                                                 &MARKDOWN_CORE_EXTENSION_LINK,
                                                                 &MARKDOWN_CORE_EXTENSION_MEDIA,
                                                                 &MARKDOWN_CORE_EXTENSION_TEXT,
                                                                 &MARKDOWN_CORE_EXTENSION_TABLE};

#define CORE_EXTENSION_COUNT (sizeof(CORE_EXTENSIONS) / sizeof(CORE_EXTENSIONS[0]))

int markdown_core_core_extensions_attach(markdown_core_parser *parser) {
    size_t i;

    if (!parser) {
        return 0;
    }

    for (i = 0; i < CORE_EXTENSION_COUNT; i++) {
        if (!markdown_core_parser_attach_extension(parser, CORE_EXTENSIONS[i])) {
            return 0;
        }
    }

    return 1;
}

static const markdown_core_extension *const BLOCK_SYNTAX[] = {
    [MARKDOWN_CORE_NODE_TABLE & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_TABLE,
    [MARKDOWN_CORE_NODE_TABLE_ROW & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_TABLE,
    [MARKDOWN_CORE_NODE_TABLE_CELL & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_TABLE,
    [MARKDOWN_CORE_NODE_FORMULA_BLOCK & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_FORMULA,
    [MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_DIRECTIVE,

    [MARKDOWN_CORE_NODE_DOCUMENT & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_DOCUMENT,
    [MARKDOWN_CORE_NODE_CALLOUT & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_CALLOUT,
    [MARKDOWN_CORE_NODE_LIST & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_LIST,
    [MARKDOWN_CORE_NODE_LIST_ITEM & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_LIST,
    [MARKDOWN_CORE_NODE_CODE_BLOCK & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_CODE_BLOCK,
    [MARKDOWN_CORE_NODE_HTML_BLOCK & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_HTML_BLOCK,
    [MARKDOWN_CORE_NODE_PARAGRAPH & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_PARAGRAPH,
    [MARKDOWN_CORE_NODE_HEADING & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_HEADING,
    [MARKDOWN_CORE_NODE_THEMATIC_BREAK & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_THEMATIC_BREAK,
    [MARKDOWN_CORE_NODE_FOOTNOTE & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_FOOTNOTE,
    [MARKDOWN_CORE_NODE_SPECIMEN & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_SPECIMEN,
    [MARKDOWN_CORE_NODE_DEFINITION_LIST & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_DEFINITION_LIST,
    [MARKDOWN_CORE_NODE_DEFINITION & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_DEFINITION_LIST,
    [MARKDOWN_CORE_NODE_DEFINITION_BODY & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_DEFINITION_LIST,
    [MARKDOWN_CORE_NODE_COMMENT_BLOCK & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_COMMENT,
    [MARKDOWN_CORE_NODE_TABLE_CAPTION & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_TABLE,
};

static const markdown_core_extension *const INLINE_SYNTAX[] = {
    [MARKDOWN_CORE_NODE_CITE & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_CITATION,
    [MARKDOWN_CORE_NODE_CITATION & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_CITATION,
    [MARKDOWN_CORE_NODE_COMMENT & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_COMMENT,
    [MARKDOWN_CORE_NODE_STRIKETHROUGH & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_STRIKETHROUGH,
    [MARKDOWN_CORE_NODE_FORMULA & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_FORMULA,
    [MARKDOWN_CORE_NODE_DIRECTIVE & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_DIRECTIVE,
    [MARKDOWN_CORE_NODE_DIRECTIVE_LABEL & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_DIRECTIVE,
    [MARKDOWN_CORE_NODE_CROSS_LINK & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_CROSS_LINK,
    [MARKDOWN_CORE_NODE_CROSS_EMBEDDED & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_CROSS_LINK,
    [MARKDOWN_CORE_NODE_MARK & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_MARK,
    [MARKDOWN_CORE_NODE_INSERTION & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_INSERTION,
    [MARKDOWN_CORE_NODE_SUPERSCRIPT & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_SUPERSCRIPT,
    [MARKDOWN_CORE_NODE_SUBSCRIPT & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_SUBSCRIPT,

    [MARKDOWN_CORE_NODE_TEXT & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_TEXT,
    [MARKDOWN_CORE_NODE_SOFT_BREAK & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_LINE_BREAK,
    [MARKDOWN_CORE_NODE_LINE_BREAK & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_LINE_BREAK,
    [MARKDOWN_CORE_NODE_CODE & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_CODE,
    [MARKDOWN_CORE_NODE_HTML & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_HTML,
    [MARKDOWN_CORE_NODE_EMPHASIS & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_EMPHASIS,
    [MARKDOWN_CORE_NODE_STRONG & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_EMPHASIS,
    [MARKDOWN_CORE_NODE_LINK & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_LINK,
    [MARKDOWN_CORE_NODE_MEDIA & MARKDOWN_CORE_NODE_VALUE_MASK] = &MARKDOWN_CORE_EXTENSION_MEDIA,
};

const markdown_core_extension *markdown_core_syntax_for_kind(markdown_core_node_type kind) {
    unsigned index = kind & MARKDOWN_CORE_NODE_VALUE_MASK;
    if (MARKDOWN_CORE_NODE_TYPE_INLINE_P(kind)) {
        return index < sizeof(INLINE_SYNTAX) / sizeof(*INLINE_SYNTAX) ? INLINE_SYNTAX[index] : NULL;
    }
    return index < sizeof(BLOCK_SYNTAX) / sizeof(*BLOCK_SYNTAX) ? BLOCK_SYNTAX[index] : NULL;
}
const markdown_core_extension *markdown_core_node_syntax(const markdown_core_node *node) {
    return node ? markdown_core_syntax_for_kind(node->kind) : NULL;
}
