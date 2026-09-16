/* The attribute benchmark's process: load one buffer, recover once, report.
 *
 * Nothing here is measured. The driver reads its numbers out of the callgrind
 * call graph, from the edge into `bench_parse_attributes`, so this file only
 * has to make sure both baselines see the buffer they were given and that a
 * recovery which found nothing cannot be reported as a measurement.
 *
 *   <runner> --input PATH [--census]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "attribute_runner.h"

static char *load_input(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    char *buffer;
    long size;
    size_t read;

    if (!file) {
        fprintf(stderr, "attribute_runner: cannot open %s\n", path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "attribute_runner: cannot size %s\n", path);
        fclose(file);
        return NULL;
    }
    buffer = (char *)malloc((size_t)size + 1);
    if (!buffer) {
        fprintf(stderr, "attribute_runner: cannot allocate %ld bytes\n", size);
        fclose(file);
        return NULL;
    }
    read = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (read != (size_t)size) {
        fprintf(stderr, "attribute_runner: short read on %s\n", path);
        free(buffer);
        return NULL;
    }
    buffer[read] = 0;
    *length = read;
    return buffer;
}

int main(int argc, char **argv) {
    const char *input = NULL;
    attribute_receipt receipt;
    char *source;
    size_t length = 0;
    int census = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            input = argv[++i];
        } else if (strcmp(argv[i], "--census") == 0) {
            census = 1;
        } else {
            fputs("usage: <attribute runner> --input PATH [--census]\n", stderr);
            return 2;
        }
    }
    if (!input) {
        fputs("usage: <attribute runner> --input PATH [--census]\n", stderr);
        return 2;
    }

    source = load_input(input, &length);
    if (!source) {
        return 1;
    }

    receipt.lists = 0;
    receipt.values = 0;
    if (bench_parse_attributes(source, length, &receipt, census ? stdout : NULL) != 0) {
        fprintf(stderr, "attribute_runner: %s failed on %s\n", bench_baseline_name(), input);
        free(source);
        return 1;
    }
    free(source);

    /* An input that recovered nothing would make every number a measurement of
     * the empty case, which is exactly the failure a silent comparison hides. */
    if (receipt.lists == 0 || receipt.values == 0) {
        fprintf(stderr, "attribute_runner: %s recovered nothing from %s\n", bench_baseline_name(), input);
        return 1;
    }
    if (!census) {
        printf("attribute-runner baseline=%s lists=%zu values=%zu\n", bench_baseline_name(), receipt.lists,
               receipt.values);
    }
    return 0;
}
