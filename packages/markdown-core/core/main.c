/* The installed command-line adapter: Markdown in, the canonical AST dump out.
 *
 * It parses the one Markdown Core dialect through the public facade and
 * nothing else. There is no `--profile`, no `-e`, and no `--smart`: the
 * dialect has no switches, so the executable has no flags that would name
 * one, and the test tree has no other parser to run either -- the oracle
 * gates and position audits run this executable.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <markdown_core.h>

#include "markdown-core-version.h"

#if defined(__OpenBSD__)
#include <sys/param.h>
#if OpenBSD >= 201605
#define USE_PLEDGE
#include <unistd.h>
#endif
#endif

#if defined(_WIN32) && !defined(__CYGWIN__)
#include <fcntl.h>
#include <io.h>
#endif

static void print_usage(FILE *stream) {
    fputs("Usage:   markdown-core [FILE*]\n"
          "Parses the files, or standard input, as one Markdown Core document and\n"
          "prints its canonical AST dump.\n"
          "Options:\n"
          "  --help, -h       Print usage information\n"
          "  --version        Print version\n",
          stream);
}

typedef struct source_buffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
} source_buffer;

/* The parser refuses a source longer than this (`blocks.c`), so the adapter
 * never buffers more. The bound also keeps the doubling below inside `size_t`
 * on a 32-bit host, where a capacity that reached 2 GiB would wrap to zero on
 * the next doubling and the loop would never end. */
#define SOURCE_LIMIT ((size_t)(INT32_MAX / 2))

/* Reads `input` to its end. Returns NULL, or the reason it stopped. */
static const char *read_all(source_buffer *source, FILE *input) {
    uint8_t chunk[4096];
    size_t bytes;

    while ((bytes = fread(chunk, 1, sizeof(chunk), input)) > 0) {
        if (bytes > SOURCE_LIMIT - source->size) {
            return "the input is longer than the parser accepts";
        }
        if (source->size + bytes > source->capacity) {
            size_t capacity = source->capacity ? source->capacity : 4096;
            uint8_t *grown;
            /* `size + bytes` is at most SOURCE_LIMIT, so the last doubling
             * stops at 2^30 and cannot wrap. */
            while (capacity < source->size + bytes) {
                capacity *= 2;
            }
            grown = (uint8_t *)realloc(source->data, capacity);
            if (!grown) {
                return "out of memory";
            }
            source->data = grown;
            source->capacity = capacity;
        }
        memcpy(source->data + source->size, chunk, bytes);
        source->size += bytes;
    }
    return ferror(input) ? strerror(errno) : NULL;
}

static bool print_document(const markdown_core_document *document) {
    markdown_core_error *error = NULL;
    uint8_t *dump = NULL;
    size_t length = 0;
    markdown_core_string message;

    if (!markdown_core_document_dump(document, &dump, &length, &error)) {
        message = markdown_core_error_get_message(error);
        fprintf(stderr, "AST dump failed: %.*s\n", (int)message.length,
                message.data ? (const char *)message.data : "unknown error");
        markdown_core_error_free(error);
        return false;
    }
    fwrite(dump, 1, length, stdout);
    markdown_core_dump_free(dump);
    return true;
}

int main(int argc, char *argv[]) {
    source_buffer source = {NULL, 0, 0};
    markdown_core_document *document = NULL;
    markdown_core_error *error = NULL;
    markdown_core_string message;
    const char *failure;
    int i;
    int file_count = 0;
    int result = 1;

#ifdef USE_PLEDGE
    if (pledge("stdio rpath", NULL) != 0) {
        perror("pledge");
        return 1;
    }
#endif

#if defined(_WIN32) && !defined(__CYGWIN__)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("markdown-core %s\n", MARKDOWN_CORE_VERSION_STRING);
            result = 0;
            goto done;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(stdout);
            result = 0;
            goto done;
        } else if (argv[i][0] == '-' && argv[i][1] != 0) {
            /* Every former switch -- `--profile`, `-e`, `--smart`, and the
             * rest -- is refused, not ignored: an invocation that asks for a
             * language other than the dialect must not silently get the
             * dialect. */
            fprintf(stderr, "markdown-core: unknown option %s; the parser takes no options\n", argv[i]);
            print_usage(stderr);
            goto done;
        } else {
            FILE *file = fopen(argv[i], "rb");
            if (!file) {
                fprintf(stderr, "Error opening file %s: %s\n", argv[i], strerror(errno));
                goto done;
            }
            failure = read_all(&source, file);
            if (failure) {
                fprintf(stderr, "Error reading file %s: %s\n", argv[i], failure);
                fclose(file);
                goto done;
            }
            fclose(file);
            file_count++;
        }
    }

    if (file_count == 0) {
        failure = read_all(&source, stdin);
        if (failure) {
            fprintf(stderr, "Error reading standard input: %s\n", failure);
            goto done;
        }
    }

#ifdef USE_PLEDGE
    if (pledge("stdio", NULL) != 0) {
        perror("pledge");
        goto done;
    }
#endif

    document = markdown_core_document_parse(source.data, source.size, &error);
    if (!document) {
        message = markdown_core_error_get_message(error);
        fprintf(stderr, "Parse failed: %.*s\n", (int)message.length,
                message.data ? (const char *)message.data : "unknown error");
        markdown_core_error_free(error);
        goto done;
    }
    if (print_document(document)) {
        result = 0;
    }

done:
    markdown_core_document_free(document);
    free(source.data);
    return result;
}
