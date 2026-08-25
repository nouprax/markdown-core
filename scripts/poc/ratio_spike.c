/* If the AST is DERIVED on demand and never stored, every derivation copies the
 * block skeleton and builds the inline children fresh. Building inlines is work
 * the engine already does; the SKELETON COPY is the new cost. Price it: what
 * fraction of a finished tree is blocks? */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <markdown_core.h>
#include "markdown-core.h"
#include "markdown-core-extensions.h"
#include "parser.h"
#include "node.h"

static void tally(markdown_core_node *n, long *blocks, long *inlines, long *bytes) {
    markdown_core_node *c;
    if ((n->type & MARKDOWN_CORE_NODE_TYPE_MASK) == MARKDOWN_CORE_NODE_TYPE_BLOCK) (*blocks)++; else (*inlines)++;
    *bytes += (long)n->content.size;
    for (c = n->first_child; c; c = c->next) tally(c, blocks, inlines, bytes);
}

int main(int argc, char **argv) {
    long tb = 0, ti = 0, tc = 0;
    int i;
    printf("%-46s %8s %8s %7s %9s\n", "file", "blocks", "inlines", "block%", "contentB");
    for (i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        char *buf; long n; markdown_core_parser *p; markdown_core_node *doc;
        long b = 0, in = 0, cb = 0;
        if (!f) continue;
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        buf = malloc((size_t)n + 1); if (!buf) { fclose(f); continue; }
        if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); continue; }
        fclose(f);
        p = markdown_core_parser_new(MARKDOWN_CORE_OPT_DEFAULT | MARKDOWN_CORE_OPT_FOOTNOTES);
        markdown_core_core_extensions_attach(p, 0x3fu);
        markdown_core_parser_feed(p, buf, (size_t)n);
        doc = markdown_core_parser_finish(p);
        if (doc) { tally(doc, &b, &in, &cb); markdown_core_node_free(doc); }
        markdown_core_parser_free(p);
        free(buf);
        printf("%-46s %8ld %8ld %6.1f%% %9ld\n", strrchr(argv[i],'/')?strrchr(argv[i],'/')+1:argv[i],
               b, in, 100.0 * (double)b / (double)(b + in ? b + in : 1), cb);
        tb += b; ti += in; tc += cb;
    }
    printf("%-46s %8ld %8ld %6.1f%% %9ld\n", "TOTAL", tb, ti, 100.0 * (double)tb / (double)(tb + ti), tc);
    return 0;
}
