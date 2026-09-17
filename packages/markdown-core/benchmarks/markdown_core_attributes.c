/* Markdown Core's side of the attribute comparison.
 *
 * This is the one runner in `benchmarks/` that reaches past the public facade.
 * It has to: the grammar being compared is `elements/attributes.c`, and a
 * consumer reaches it only through a span, a heading or a link, each of which
 * brings a whole inline parse along. Measuring the attribute grammar through
 * one of those measures the inline parser, which is the mistake the stage
 * report already warns about -- `inline-span` reads 4.46x and the largest
 * single cost in it is `utf8proc_is_letter`, not the attribute scan.
 *
 * Reaching in is a property of THIS FILE and not of the product: the parser
 * has no measurement mode and no benchmark-only path, and the entry points
 * below are the same ones `link.c` and `heading.c` call.
 */
#include <stdlib.h>
#include <string.h>

#include "attributes.h"
#include "chunk.h"

#include "attribute_runner.h"

const char *bench_baseline_name(void) { return "markdown-core"; }

static void write_chunk(FILE *census, const markdown_core_chunk *chunk) {
    if (chunk->data && chunk->len > 0) {
        fwrite(chunk->data, 1, (size_t)chunk->len, census);
    }
}

int bench_parse_attributes(const char *source, size_t length, attribute_receipt *receipt, FILE *census) {
    /* One index over the whole buffer, exactly as an inline parse builds one
     * over a line: the index exists so overlapping failed candidates cannot
     * rescan an extent, and giving each list its own would measure a parser
     * this repository does not ship. */
    markdown_core_attribute_parser parser = {.data = (const unsigned char *)source, .length = (bufsize_t)length};
    bufsize_t at = 0;

    while (at < (bufsize_t)length) {
        markdown_core_attributes value = {0};
        bufsize_t end = 0;

        if (source[at] != '{' || !markdown_core_attributes_parse(&parser, at, &value, &end)) {
            at++;
            continue;
        }
        if (census) {
            size_t i;
            fprintf(census, "list %zu id=", receipt->lists);
            write_chunk(census, &value.anchor);
            fputs(" class=", census);
            for (i = 0; i < value.class_count; i++) {
                if (i) {
                    fputc(' ', census);
                }
                write_chunk(census, &value.classes[i]);
            }
            for (i = 0; i < value.record_count; i++) {
                fputc(' ', census);
                write_chunk(census, &value.records[i].name);
                fputc('=', census);
                write_chunk(census, &value.records[i].value);
            }
            fputc('\n', census);
        }
        receipt->lists++;
        /* The anchor and the class run each count once whether or not they are
         * present, so the count is a property of the input rather than of what
         * one baseline chose to represent. */
        receipt->values += 2 + value.record_count;
        markdown_core_attributes_free(&value);
        at = end;
    }
    /* Read before the free, because allocation failure is sticky and a
     * recovery that ran out of memory recovered less than it reports. */
    if (parser.oom) {
        markdown_core_attribute_parser_free(&parser);
        return 1;
    }
    markdown_core_attribute_parser_free(&parser);
    return 0;
}
