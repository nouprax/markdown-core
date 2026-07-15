/* CLI AST dump suite.
 *
 * Runs the AST-only markdown-core CLI against every canonical fixture passed
 * on the command line and compares stdout byte-for-byte with
 * the reviewed `.ast` golden.
 *
 *   dump_cli_runner --program CLI --fixtures DIR NAME...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"

#if defined(_WIN32)
#define ts_popen _popen
#define ts_pclose _pclose
#else
#define ts_popen popen
#define ts_pclose pclose
#endif

static char *run_cli(const char *program, const char *markdown_path, size_t *output_length) {
    char command[2048];
    FILE *pipe;
    char *output = NULL;
    size_t capacity = 4096;
    size_t length = 0;

#if defined(_WIN32)
    /* cmd.exe /c strips the first and last quote from a quoted command. */
    snprintf(command, sizeof(command), "\"\"%s\" \"%s\"\"", program, markdown_path);
#else
    snprintf(command, sizeof(command), "\"%s\" \"%s\"", program, markdown_path);
#endif
    pipe = ts_popen(command, "r");
    if (!pipe)
        return NULL;
    output = (char *)malloc(capacity);
    if (!output) {
        ts_pclose(pipe);
        return NULL;
    }
    for (;;) {
        size_t bytes;
        if (length + 4096 + 1 > capacity) {
            char *grown;
            capacity *= 2;
            grown = (char *)realloc(output, capacity);
            if (!grown) {
                free(output);
                ts_pclose(pipe);
                return NULL;
            }
            output = grown;
        }
        bytes = fread(output + length, 1, 4096, pipe);
        length += bytes;
        if (bytes < 4096)
            break;
    }
    if (ts_pclose(pipe) != 0) {
        free(output);
        return NULL;
    }
    output[length] = 0;
    *output_length = length;
    return output;
}

int main(int argc, char **argv) {
    const char *program = NULL;
    const char *fixtures = NULL;
    int i;
    int first_fixture = 0;
    size_t failures = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--program") == 0 && i + 1 < argc) {
            program = argv[++i];
        } else if (strcmp(argv[i], "--fixtures") == 0 && i + 1 < argc) {
            fixtures = argv[++i];
        } else {
            first_fixture = i;
            break;
        }
    }

    if (!program || !fixtures || !first_fixture) {
        fputs("usage: dump_cli_runner --program CLI --fixtures DIR NAME...\n", stderr);
        return 2;
    }

    for (i = first_fixture; i < argc; i++) {
        char markdown_path[1024];
        char golden_path[1024];
        uint8_t *expected;
        size_t expected_length = 0;
        char *actual;
        size_t actual_length = 0;

        snprintf(markdown_path, sizeof(markdown_path), "%s/%s.md", fixtures, argv[i]);
        snprintf(golden_path, sizeof(golden_path), "%s/%s.ast", fixtures, argv[i]);
        expected = ts_read_file(golden_path, &expected_length);
        if (!expected) {
            fprintf(stderr, "%s: cannot read golden %s\n", argv[i], golden_path);
            failures++;
            continue;
        }
        actual = run_cli(program, markdown_path, &actual_length);
        if (!actual) {
            fprintf(stderr, "%s: CLI invocation failed\n", argv[i]);
            free(expected);
            failures++;
            continue;
        }
        if (actual_length != expected_length || memcmp(actual, expected, expected_length) != 0) {
            fprintf(stderr, "%s: CLI AST dump differs from reviewed golden\n", argv[i]);
            ts_print_line_diff(stderr, (const char *)expected, actual);
            failures++;
        }
        free(actual);
        free(expected);
    }

    if (failures) {
        fprintf(stderr, "%zu fixture(s) failed\n", failures);
        return 1;
    }
    printf("CLI AST dump matches all reviewed canonical fixtures\n");
    return 0;
}
