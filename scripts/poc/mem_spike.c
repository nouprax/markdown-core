/* What does the concrete view ACTUALLY cost? Q24 says "~2.5-3x input resident".
 * Decompose it: normalized source, the line index, and the per-block content
 * buffers -- which exist whether or not anyone asks for a concrete view. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <markdown_core.h>
#include "markdown-core.h"
#include "markdown-core-extensions.h"
#include "ast_internal.h"
#include "parser.h"
#include "node.h"

static void sum_content(const markdown_core_node *n, long *used, long *alloc, long *nodes) {
    const markdown_core_node *c;
    (*nodes)++;
    *used += (long)n->content.size;
    *alloc += (long)n->content.asize;
    for (c = n->first_child; c; c = c->next) sum_content(c, used, alloc, nodes);
}

int main(int argc, char **argv) {
    int i;
    printf("%-26s %9s %8s %8s %8s %8s %8s\n", "file", "input", "src.siz", "src.aloc", "lineIdx", "blkCont", "nodes");
    for (i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb"); char *buf; long n;
        markdown_core_parse_options o; markdown_core_document *d; markdown_core_error *e = NULL;
        long cu = 0, ca = 0, nodes = 0; double src, idx, blk, nb;
        if (!f) continue;
        fseek(f,0,SEEK_END); n=ftell(f); fseek(f,0,SEEK_SET);
        buf=malloc((size_t)n+1); if(!buf){fclose(f);continue;}
        if (fread(buf,1,(size_t)n,f)!=(size_t)n){free(buf);fclose(f);continue;}
        fclose(f);
        markdown_core_parse_options_init(&o);
        d = markdown_core_document_parse((const uint8_t *)buf, (size_t)n, &o, &e);
        if (!d) { markdown_core_error_free(e); free(buf); continue; }
        sum_content(d->root, &cu, &ca, &nodes);
        src = (double)d->concrete.source.asize;
        idx = (double)d->concrete.line_starts_size * (double)sizeof(bufsize_t);
        blk = (double)ca;
        nb  = (double)nodes * (double)sizeof(markdown_core_node);
        printf("%-26s %9ld %7.2fx %7.2fx %7.3fx %7.2fx %7.2fx\n",
               strrchr(argv[i],'/')?strrchr(argv[i],'/')+1:argv[i], n,
               (double)d->concrete.source.size/(double)n, src/(double)n, idx/(double)n,
               blk/(double)n, nb/(double)n);
        markdown_core_document_free(d); free(buf);
    }
    printf("\n(src.aloc = the retained normalized source AS ALLOCATED; lineIdx = 4 bytes/line;\n"
           " blkCont = every block's content strbuf; nodes = sizeof(node) * count -- all as multiples of input)\n");
    return 0;
}
