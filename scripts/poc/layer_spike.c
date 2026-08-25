/* Does RESOLUTION change more than the one node?
 *
 * If a failed bracket leaves its emphasis delimiters on the stack to be matched
 * ACROSS the bracket, while a resolved one bounds them, then resolution is not
 * a local substitution and a source-faithful inline layer cannot be independent
 * of the refmap. Same source, two refmaps. */
#include <stdio.h>
#include <string.h>
#include <markdown_core.h>
#include "markdown-core.h"
#include "parser.h"
#include "node.h"

static void walk(markdown_core_node *n, int d) {
    markdown_core_node *c;
    printf("%*s%s", d * 2, "", markdown_core_node_get_type_string(n));
    if (n->type == MARKDOWN_CORE_NODE_TEXT || n->type == MARKDOWN_CORE_NODE_CODE) {
        printf(" \"%.*s\"", (int)n->as.literal.len, n->as.literal.data);
    }
    putchar('\n');
    for (c = n->first_child; c; c = c->next) walk(c, d + 1);
}

static void run(const char *tag, const char *src) {
    markdown_core_parser *p = markdown_core_parser_new(MARKDOWN_CORE_OPT_DEFAULT);
    markdown_core_node *doc;
    markdown_core_parser_feed(p, src, strlen(src));
    doc = markdown_core_parser_finish(p);
    printf("=== %s ===\n%s---\n", tag, src);
    walk(doc, 0);
    putchar('\n');
    markdown_core_node_free(doc);
    markdown_core_parser_free(p);
}

int main(void) {
    run("A: emphasis crossing an UNRESOLVED bracket", "*foo [bar* baz]\n");
    run("B: same source, bracket RESOLVES", "*foo [bar* baz]\n\n[bar* baz]: /url\n");
    run("C: emphasis inside an UNRESOLVED shortcut", "[a *b] c*\n");
    run("D: same source, shortcut RESOLVES", "[a *b] c*\n\n[a *b]: /url\n");
    return 0;
}
