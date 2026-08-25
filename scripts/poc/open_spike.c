/* Is __OPEN on a table cell a MID-STREAM artefact, or is it wrong in the
 * FINISHED tree too? make_block sets it (blocks.c:136) and finalize is the only
 * clearer (blocks.c:1036) -- and a cell is never on the open spine. */
#include <stdio.h>
#include <string.h>
#include <markdown_core.h>
#include "markdown-core.h"
#include "markdown-core-extensions.h"
#include "parser.h"
#include "node.h"

static void walk(markdown_core_node *n, int d) {
    markdown_core_node *c;
    printf("%*s%-16s flags=0x%02x %s\n", d * 2, "", markdown_core_node_get_type_string(n),
           (unsigned)n->flags, (n->flags & MARKDOWN_CORE_NODE__OPEN) ? "<-- __OPEN" : "");
    for (c = n->first_child; c; c = c->next) walk(c, d + 1);
}

int main(void) {
    const char *src = "| a | b |\n|---|---|\n| 1 | 2 |\n\npara\n";
    markdown_core_parser *p = markdown_core_parser_new(MARKDOWN_CORE_OPT_DEFAULT);
    markdown_core_node *doc;
    markdown_core_core_extensions_attach(p, MARKDOWN_CORE_CORE_EXTENSION_TABLE);
    markdown_core_parser_feed(p, src, strlen(src));
    doc = markdown_core_parser_finish(p);           /* FINISHED, not mid-stream */
    printf("=== the FINISHED tree ===\n");
    walk(doc, 0);
    markdown_core_node_free(doc);
    markdown_core_parser_free(p);
    return 0;
}
