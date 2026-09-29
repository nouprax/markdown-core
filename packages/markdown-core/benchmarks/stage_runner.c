/* The stage benchmark's process: load one document, parse it once, report.
 *
 * Nothing here is measured. The driver reads its numbers out of the callgrind
 * call graph, from the edges into each engine's own stage entry, so this file
 * only has to make sure both engines see byte-identical input and report what
 * the parse produced. Each driver checks that receipt against what it expects
 * of its documents, so a parse which silently did nothing cannot be reported
 * as a measurement.
 *
 *   <runner> --document PATH
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stage_runner.h"

static char *load_document(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    char *buffer;
    long size;
    size_t read;

    if (!file) {
        fprintf(stderr, "stage_runner: cannot open %s\n", path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "stage_runner: cannot size %s\n", path);
        fclose(file);
        return NULL;
    }
    buffer = (char *)malloc((size_t)size + 1);
    if (!buffer) {
        fprintf(stderr, "stage_runner: cannot allocate %ld bytes\n", size);
        fclose(file);
        return NULL;
    }
    read = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (read != (size_t)size) {
        fprintf(stderr, "stage_runner: short read on %s\n", path);
        free(buffer);
        return NULL;
    }
    buffer[read] = 0;
    *length = read;
    return buffer;
}

int main(int argc, char **argv) {
    const char *document = NULL;
    bench_receipt receipt;
    char *source;
    size_t length = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--document") == 0 && i + 1 < argc) {
            document = argv[++i];
        } else {
            fputs("usage: <stage runner> --document PATH\n", stderr);
            return 2;
        }
    }
    if (!document) {
        fputs("usage: <stage runner> --document PATH\n", stderr);
        return 2;
    }

    source = load_document(document, &length);
    if (!source) {
        return 1;
    }

    receipt.bytes = 0;
    receipt.root_children = 0;
    if (bench_parse_document(source, length, &receipt) != 0) {
        fprintf(stderr, "stage_runner: %s failed to parse %s\n", bench_engine_name(), document);
        free(source);
        return 1;
    }
    free(source);
    printf("stage-runner engine=%s bytes=%zu root_children=%zu\n", bench_engine_name(), receipt.bytes,
           receipt.root_children);
    return 0;
}
