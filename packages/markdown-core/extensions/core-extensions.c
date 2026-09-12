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
static const markdown_core_extension *const CORE_EXTENSIONS[] = {&MARKDOWN_CORE_EXTENSION_DEFINITION_LIST,
                                                                 &MARKDOWN_CORE_EXTENSION_CALLOUT,
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
