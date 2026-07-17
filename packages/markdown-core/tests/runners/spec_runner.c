/* Data-driven spec fixture runner.
 *
 * Parses every example of a CommonMark-style spec fixture through the
 * read-only facade and compares the canonical AST dump byte-for-byte with
 * the expected block; each example is also dumped twice to assert dump
 * determinism.  No renderer is involved.
 *
 *   spec_runner --spec FILE [--option NAME]...
 *               [--list] [--example N] [--section TEXT]
 *
 * `--rewrite` is an explicit maintenance mode that regenerates the expected
 * blocks in place from the current parser.  The resulting fixture diff must
 * be human-reviewed before it is committed; it exists so intentional parser
 * changes produce reviewable golden diffs, never as part of a test run.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"

static void usage(FILE *stream) {
    fputs(
        "usage: spec_runner --spec FILE [--option NAME]...\n"
        "                   [--list] [--example N] [--section TEXT] [--rewrite]\n",
        stream
    );
}

static uint8_t *
dump_example(const ts_spec_case *test_case, const markdown_core_parse_options *base, size_t *dump_length) {
    markdown_core_parse_options options = *base;
    markdown_core_document *document;
    markdown_core_error *error = NULL;
    uint8_t *dump = NULL;
    size_t extension_index;

    for (extension_index = 0; extension_index < test_case->extension_count; extension_index++) {
        if (ts_ast_enable(&options, test_case->extensions[extension_index]) != 0) {
            fprintf(
                stderr,
                "example %d: unknown fixture tag %s\n",
                test_case->example,
                test_case->extensions[extension_index]
            );
            return NULL;
        }
    }
    document = ts_ast_parse((const uint8_t *)test_case->markdown, test_case->markdown_length, &options);
    if (!document) {
        return NULL;
    }
    if (!markdown_core_document_dump(document, &dump, dump_length, &error)) {
        fprintf(stderr, "example %d: dump failed\n", test_case->example);
        markdown_core_error_free(error);
        markdown_core_document_free(document);
        return NULL;
    }
    /* Dump determinism: a second dump of the same document must be
     * byte-identical. */
    {
        uint8_t *second = NULL;
        size_t second_length = 0;
        if (!markdown_core_document_dump(document, &second, &second_length, &error) || second_length != *dump_length ||
            memcmp(dump, second, second_length) != 0) {
            fprintf(stderr, "example %d: dump is not deterministic\n", test_case->example);
            markdown_core_dump_free(second);
            markdown_core_dump_free(dump);
            markdown_core_error_free(error);
            markdown_core_document_free(document);
            return NULL;
        }
        markdown_core_dump_free(second);
    }
    markdown_core_document_free(document);
    return dump;
}

/* Checks for the `disabled` tag within the fence-open line only (the line is
 * a view into the whole file and is not NUL-terminated). */
static int line_has_disabled_tag(const char *line, size_t line_length) {
    const char *cursor = line + 40;
    const char *end = line + line_length;
    while (cursor < end) {
        const char *word;
        while (cursor < end && *cursor == ' ') {
            cursor++;
        }
        word = cursor;
        while (cursor < end && *cursor != ' ') {
            cursor++;
        }
        if (cursor - word == 8 && strncmp(word, "disabled", 8) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Maintenance mode: regenerates every enabled example's expected block from
 * the current parser, preserving all prose, fences, tags, and disabled
 * examples byte-for-byte. */
static int rewrite_fixture(const char *path, const markdown_core_parse_options *base) {
    size_t length = 0;
    uint8_t *bytes = ts_read_file(path, &length);
    ts_spec_file spec;
    FILE *output;
    size_t line_start = 0;
    int state = 0; /* 0 prose, 1 markdown, 2 skipping old expected */
    int disabled = 0;
    int example_number = 0;
    size_t case_index = 0;
    int result = -1;

    if (!bytes) {
        return -1;
    }
    if (ts_spec_load(path, &spec) != 0) {
        free(bytes);
        return -1;
    }
    output = fopen(path, "wb");
    if (!output) {
        ts_spec_free(&spec);
        free(bytes);
        return -1;
    }

    while (line_start <= length) {
        size_t line_end = line_start;
        const char *line = (const char *)bytes + line_start;
        size_t raw_length;
        size_t content_end;
        size_t line_length;
        while (line_end < length && bytes[line_end] != '\n') {
            line_end++;
        }
        if (line_start == length && line_end == length && line_start != 0 && bytes[line_start - 1] == '\n') {
            break;
        }
        raw_length = line_end - line_start;
        content_end = line_end;
        while (content_end > line_start &&
               (bytes[content_end - 1] == '\r' || bytes[content_end - 1] == ' ' || bytes[content_end - 1] == '\t')) {
            content_end--;
        }
        line_length = content_end - line_start;

        if (line_length >= 40 && strncmp(line, "````````````````````````````````", 32) == 0 &&
            strncmp(line + 32, " example", 8) == 0) {
            state = 1;
            disabled = line_has_disabled_tag(line, line_length);
            fwrite(line, 1, raw_length, output);
            fputc('\n', output);
        } else if (line_length == 32 && strncmp(line, "````````````````````````````````", 32) == 0 && state != 0) {
            example_number++;
            fwrite(line, 1, raw_length, output);
            fputc('\n', output);
            state = 0;
        } else if (state == 1 && line_length == 1 && line[0] == '.') {
            fwrite(line, 1, raw_length, output);
            fputc('\n', output);
            if (disabled) {
                /* keep the stored expected block verbatim; state 1 lines fall
                 * through to the verbatim branch below */
            } else {
                size_t dump_length = 0;
                uint8_t *dump;
                if (case_index >= spec.count || spec.cases[case_index].example != example_number + 1) {
                    fprintf(
                        stderr,
                        "rewrite lost sync at example %d (case_index %zu of %zu, holds "
                        "example %d)\n",
                        example_number + 1,
                        case_index,
                        spec.count,
                        case_index < spec.count ? spec.cases[case_index].example : -1
                    );
                    goto done;
                }
                dump = dump_example(&spec.cases[case_index], base, &dump_length);
                if (!dump) {
                    goto done;
                }
                fwrite(dump, 1, dump_length, output);
                markdown_core_dump_free(dump);
                case_index++;
                state = 2;
            }
        } else if (state == 2) {
            /* skip the old expected block */
        } else {
            fwrite(line, 1, raw_length, output);
            fputc('\n', output);
        }

        if (line_end >= length) {
            break;
        }
        line_start = line_end + 1;
    }

    result = 0;
done:
    fclose(output);
    ts_spec_free(&spec);
    free(bytes);
    return result;
}

int main(int argc, char **argv) {
    const char *spec_path = NULL;
    const char *section_filter = NULL;
    markdown_core_parse_options base;
    ts_spec_file spec;
    int list_only = 0;
    int rewrite = 0;
    int example_filter = 0;
    int i;
    size_t case_index;
    size_t passed = 0, failed = 0, errored = 0, skipped = 0;

    ts_ast_options_none(&base);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--spec") == 0 && i + 1 < argc) {
            spec_path = argv[++i];
        } else if (strcmp(argv[i], "--option") == 0 && i + 1 < argc) {
            if (ts_ast_enable(&base, argv[++i]) != 0) {
                fprintf(stderr, "unknown option: %s\n", argv[i]);
                return 2;
            }
        } else if (strcmp(argv[i], "--list") == 0) {
            list_only = 1;
        } else if (strcmp(argv[i], "--rewrite") == 0) {
            rewrite = 1;
        } else if (strcmp(argv[i], "--example") == 0 && i + 1 < argc) {
            example_filter = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--section") == 0 && i + 1 < argc) {
            section_filter = argv[++i];
        } else {
            usage(stderr);
            return 2;
        }
    }

    if (!spec_path) {
        usage(stderr);
        return 2;
    }
    if (rewrite) {
        if (rewrite_fixture(spec_path, &base) != 0) {
            fprintf(stderr, "failed to rewrite %s\n", spec_path);
            return 1;
        }
        printf("rewrote expected AST dumps in %s; review the diff before committing\n", spec_path);
        return 0;
    }
    if (ts_spec_load(spec_path, &spec) != 0) {
        fprintf(stderr, "cannot load spec fixture: %s\n", spec_path);
        return 2;
    }

    for (case_index = 0; case_index < spec.count; case_index++) {
        const ts_spec_case *test_case = &spec.cases[case_index];
        uint8_t *dump = NULL;
        size_t dump_length = 0;

        if (example_filter && test_case->example != example_filter) {
            skipped++;
            continue;
        }
        if (section_filter && !strstr(test_case->section, section_filter)) {
            skipped++;
            continue;
        }
        if (list_only) {
            printf(
                "example %d (lines %d-%d) %s\n",
                test_case->example,
                test_case->start_line,
                test_case->end_line,
                test_case->section
            );
            continue;
        }

        dump = dump_example(test_case, &base, &dump_length);
        if (!dump) {
            fprintf(
                stderr,
                "example %d (lines %d-%d) %s: conversion failed\n",
                test_case->example,
                test_case->start_line,
                test_case->end_line,
                test_case->section
            );
            errored++;
            continue;
        }

        if (strlen(test_case->expected) == dump_length && memcmp(test_case->expected, dump, dump_length) == 0) {
            passed++;
        } else {
            fprintf(
                stderr,
                "FAILED example %d (lines %d-%d) %s\n",
                test_case->example,
                test_case->start_line,
                test_case->end_line,
                test_case->section
            );
            fputs(test_case->markdown, stderr);
            ts_print_line_diff(stderr, test_case->expected, (const char *)dump);
            fputc('\n', stderr);
            failed++;
        }
        markdown_core_dump_free(dump);
    }

    ts_spec_free(&spec);
    if (list_only) {
        return 0;
    }
    printf("%zu passed, %zu failed, %zu errored, %zu skipped\n", passed, failed, errored, skipped);
    return (failed + errored) ? 1 : 0;
}
