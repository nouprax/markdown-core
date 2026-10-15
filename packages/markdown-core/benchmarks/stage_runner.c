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

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "stage_runner.h"

/* Read with read(2) rather than stdio. A stdio stream allocates a buffer and
 * frees it at fclose, just before the measured parse, and the engine's first
 * allocations would then be carved from that hole: the stages' allocator work
 * would depend on this file's I/O. Read this way, the parse starts on a heap
 * that holds the document and nothing the runner has freed. */
static char *load_document(const char *path, size_t *length) {
    int file = open(path, O_RDONLY);
    struct stat facts;
    char *buffer;
    size_t done = 0;

    if (file < 0) {
        fprintf(stderr, "stage_runner: cannot open %s\n", path);
        return NULL;
    }
    if (fstat(file, &facts) != 0 || facts.st_size < 0) {
        fprintf(stderr, "stage_runner: cannot size %s\n", path);
        close(file);
        return NULL;
    }
    buffer = (char *)malloc((size_t)facts.st_size + 1);
    if (!buffer) {
        fprintf(stderr, "stage_runner: cannot allocate %lld bytes\n", (long long)facts.st_size);
        close(file);
        return NULL;
    }
    while (done < (size_t)facts.st_size) {
        ssize_t count = read(file, buffer + done, (size_t)facts.st_size - done);
        if (count <= 0) {
            break;
        }
        done += (size_t)count;
    }
    close(file);
    if (done != (size_t)facts.st_size) {
        fprintf(stderr, "stage_runner: short read on %s\n", path);
        free(buffer);
        return NULL;
    }
    buffer[done] = 0;
    *length = done;
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
