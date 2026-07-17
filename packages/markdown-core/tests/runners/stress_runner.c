/* Deterministic correctness stress cases for the public parse/release facade.
 * These are deliberately separate from timed benchmark workloads. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <markdown_core.h>

static int parse_and_release(const uint8_t *source, size_t length) {
    markdown_core_error *error = NULL;
    markdown_core_document *document = markdown_core_document_parse(source, length, NULL, &error);
    if (!document) {
        markdown_core_string_view message = markdown_core_error_get_message(error);
        fprintf(
            stderr,
            "stress parse failed: %.*s\n",
            (int)message.length,
            message.data ? (const char *)message.data : "unknown"
        );
        markdown_core_error_free(error);
        return 1;
    }
    markdown_core_document_free(document);
    markdown_core_error_free(error);
    return 0;
}

static int large_document(void) {
    static const char unit[] = "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n";
    const size_t repeats = 10000;
    const size_t unit_length = sizeof(unit) - 1;
    uint8_t *source = (uint8_t *)malloc(unit_length * repeats);
    size_t index;
    int result;
    if (!source) {
        return 1;
    }
    for (index = 0; index < repeats; index++) {
        memcpy(source + index * unit_length, unit, unit_length);
    }
    result = parse_and_release(source, unit_length * repeats);
    free(source);
    return result;
}

static int deep_nesting(void) {
    const size_t depth = 2048;
    uint8_t *source = (uint8_t *)malloc(depth * 2 + 6);
    size_t index;
    int result;
    if (!source) {
        return 1;
    }
    for (index = 0; index < depth; index++) {
        source[index * 2] = '>';
        source[index * 2 + 1] = ' ';
    }
    memcpy(source + depth * 2, "leaf\n", 5);
    result = parse_and_release(source, depth * 2 + 5);
    free(source);
    return result;
}

static int repeated_release(void) {
    static const uint8_t source[] = "# Copy\n\n- [x] item 🚀\n";
    int iteration;
    for (iteration = 0; iteration < 5000; iteration++) {
        if (parse_and_release(source, sizeof(source) - 1) != 0) {
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *case_name;
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        puts("large_document");
        puts("deep_nesting");
        puts("repeated_release");
        return 0;
    }
    if (argc != 3 || strcmp(argv[1], "--case") != 0) {
        fputs("usage: stress_runner --list | --case NAME\n", stderr);
        return 2;
    }
    case_name = argv[2];
    if (strcmp(case_name, "large_document") == 0) {
        return large_document();
    }
    if (strcmp(case_name, "deep_nesting") == 0) {
        return deep_nesting();
    }
    if (strcmp(case_name, "repeated_release") == 0) {
        return repeated_release();
    }
    fprintf(stderr, "unknown stress case: %s\n", case_name);
    return 2;
}
