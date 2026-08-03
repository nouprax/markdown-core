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
    if (!pipe) {
        return NULL;
    }
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
        if (bytes < 4096) {
            break;
        }
    }
    if (ts_pclose(pipe) != 0) {
        free(output);
        return NULL;
    }
    output[length] = 0;
    *output_length = length;
    return output;
}

/* Runs the CLI over stdin with extra arguments and returns its stdout, with
 * the exit status in *status. */
static char *run_cli_stdin(const char *program, const char *extra, const char *input, int *status) {
    char command[2048];
    FILE *pipe;
    char *output;
    size_t capacity = 4096;
    size_t length = 0;

    snprintf(command, sizeof(command), "printf '%%s' '%s' | \"%s\" %s 2>/dev/null", input, program, extra);
    pipe = ts_popen(command, "r");
    if (!pipe) {
        return NULL;
    }
    output = (char *)malloc(capacity);
    if (!output) {
        ts_pclose(pipe);
        return NULL;
    }
    for (;;) {
        size_t got = fread(output + length, 1, capacity - length - 1, pipe);
        length += got;
        if (length + 1 < capacity) {
            break;
        }
        capacity *= 2;
        char *grown = (char *)realloc(output, capacity);
        if (!grown) {
            free(output);
            ts_pclose(pipe);
            return NULL;
        }
        output = grown;
    }
    output[length] = '\0';
    *status = ts_pclose(pipe);
    return output;
}

/* The CLI's option profiles decide which language it parses, and the
 * upstream-parity gate depends on `gfm` leaving this repository's own
 * extensions off. These assertions pin that: without them the profile could
 * silently start parsing a formula and the parity comparison would quietly
 * stop comparing the same language on both sides. */
static int check_profiles(const char *program) {
    static const struct {
        const char *name;
        const char *arguments;
        const char *input;
        const char *expect;
        int expect_success;
    } cases[] = {
        {"gfm leaves formulas as text", "--profile gfm", "$x$\\n", "Text", 1},
        {"default parses formulas", "--profile default", "$x$\\n", "Formula", 1},
        {"no profile equals default", "", "$x$\\n", "Formula", 1},
        {"gfm keeps the shared extensions", "--profile gfm", "~~x~~\\n", "Strikethrough", 1},
        {"gfm leaves cross links as text", "--profile gfm", "[[a]]\\n", "Text", 1},
        {"gfm-extended parses cross links", "--profile gfm-extended", "[[a]]\\n", "CrossLink", 1},
        {"gfm-extended leaves smart punctuation off", "--profile gfm-extended", "a...b\\n", "a...b", 1},
        {"gfm leaves smart punctuation off", "--profile gfm", "a...b\\n", "a...b", 1},
        /* U+2026 as UTF-8 bytes, so this source file stays ASCII. */
        {"gfm-smart turns smart punctuation on",
         "--profile gfm-smart",
         "a...b\\n",
         "a\xe2\x80\xa6"
         "b",
         1},
        {"gfm-smart leaves formulas as text", "--profile gfm-smart", "$x$\\n", "Text", 1},
        {"an unknown profile fails", "--profile nope", "x\\n", NULL, 0},
        {"a profile with no value fails", "--profile", "x\\n", NULL, 0}
    };
    size_t failures = 0;
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int status = -1;
        char *out = run_cli_stdin(program, cases[i].arguments, cases[i].input, &status);
        int succeeded;
        if (!out) {
            fprintf(stderr, "%s: CLI invocation failed\n", cases[i].name);
            failures++;
            continue;
        }
        succeeded = status == 0;
        if (succeeded != cases[i].expect_success) {
            fprintf(stderr, "%s: expected %s exit\n", cases[i].name, cases[i].expect_success ? "success" : "failure");
            failures++;
        } else if (cases[i].expect && !strstr(out, cases[i].expect)) {
            fprintf(stderr, "%s: expected %s in the dump, got:\n%s\n", cases[i].name, cases[i].expect, out);
            failures++;
        }
        free(out);
    }

    if (failures) {
        fprintf(stderr, "%zu profile case(s) failed\n", failures);
        return 1;
    }
    printf("CLI option profiles behave as specified\n");
    return 0;
}

int main(int argc, char **argv) {
    const char *program = NULL;
    const char *fixtures = NULL;
    int i;
    int first_fixture = 0;
    int profiles_only = 0;
    size_t failures = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--program") == 0 && i + 1 < argc) {
            program = argv[++i];
        } else if (strcmp(argv[i], "--fixtures") == 0 && i + 1 < argc) {
            fixtures = argv[++i];
        } else if (strcmp(argv[i], "--profiles") == 0) {
            profiles_only = 1;
        } else {
            first_fixture = i;
            break;
        }
    }

    if (profiles_only) {
        return program ? check_profiles(program) : 2;
    }

    if (!program || !fixtures || !first_fixture) {
        fputs("usage: dump_cli_runner --program CLI --fixtures DIR NAME...\n", stderr);
        fputs("       dump_cli_runner --program CLI --profiles\n", stderr);
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
