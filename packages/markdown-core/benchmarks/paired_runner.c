/* The paired PR benchmark loads both revisions with this identical driver.
 * Input loading, canonical validation and JSON output are outside the timer.
 * Each invocation owns its allocator history and reports every timed sample. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include <dlfcn.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include <markdown_core.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(1);
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static long rss_kib(void) {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        perror("getrusage");
        exit(1);
    }
#ifdef __APPLE__
    return usage.ru_maxrss / 1024;
#else
    return usage.ru_maxrss;
#endif
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: paired_runner LIBRARY INPUT measure|dump WARMUP REPEATS\n");
        return 2;
    }
    char *end;
    long warmup = strtol(argv[4], &end, 10);
    if (*end || warmup < 0 || warmup > 32) {
        return 2;
    }
    long repeats = strtol(argv[5], &end, 10);
    if (*end || repeats < 1 || repeats > 32 || (strcmp(argv[3], "measure") && strcmp(argv[3], "dump"))) {
        return 2;
    }
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        fprintf(stderr, "%s\n", dlerror());
        return 1;
    }
    markdown_core_document *(*parse_document)(const uint8_t *, size_t, markdown_core_error **) =
        (markdown_core_document * (*)(const uint8_t *, size_t, markdown_core_error **))
            dlsym(library, "markdown_core_document_parse");
    void (*free_document)(markdown_core_document *) =
        (void (*)(markdown_core_document *))dlsym(library, "markdown_core_document_free");
    bool (*dump_document)(const markdown_core_document *, uint8_t **, size_t *, markdown_core_error **) =
        (bool (*)(const markdown_core_document *, uint8_t **, size_t *, markdown_core_error **))dlsym(
            library, "markdown_core_document_dump");
    void (*free_dump)(uint8_t *) = (void (*)(uint8_t *))dlsym(library, "markdown_core_dump_free");
    if (!parse_document || !free_document || !dump_document || !free_dump) {
        fprintf(stderr, "library does not provide the benchmark's public API\n");
        return 1;
    }
    FILE *file = fopen(argv[2], "rb");
    if (!file || fseek(file, 0, SEEK_END) != 0) {
        return 1;
    }
    long size = ftell(file);
    if (size < 0 || size > 16 * 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 1;
    }
    uint8_t *input = malloc((size_t)size + 1);
    if (!input || fread(input, 1, (size_t)size, file) != (size_t)size) {
        free(input);
        fclose(file);
        return 1;
    }
    fclose(file);
    input[size] = 0;
    if (!strcmp(argv[3], "dump")) {
        markdown_core_document *document = parse_document(input, (size_t)size, NULL);
        uint8_t *dump = NULL;
        size_t length = 0;
        int ok = document && dump_document(document, &dump, &length, NULL);
        if (ok) {
            ok = fwrite(dump, 1, length, stdout) == length;
        }
        free_dump(dump);
        free_document(document);
        free(input);
        return ok ? 0 : 1;
    }
    uint64_t parse[32], release[32];
    for (long i = -warmup; i < repeats; i++) {
        uint64_t start = now_ns();
        markdown_core_document *document = parse_document(input, (size_t)size, NULL);
        uint64_t parsed = now_ns();
        if (!document) {
            free(input);
            return 1;
        }
        free_document(document);
        uint64_t freed = now_ns();
        if (i >= 0) {
            parse[i] = parsed - start;
            release[i] = freed - parsed;
        }
    }
    free(input);
    printf("{\"rssKiB\":%ld,\"parseNs\":[", rss_kib());
    for (long i = 0; i < repeats; i++) {
        printf("%s%" PRIu64, i ? "," : "", parse[i]);
    }
    printf("],\"freeNs\":[");
    for (long i = 0; i < repeats; i++) {
        printf("%s%" PRIu64, i ? "," : "", release[i]);
    }
    printf("]}\n");
    return ferror(stdout) ? 1 : 0;
}
