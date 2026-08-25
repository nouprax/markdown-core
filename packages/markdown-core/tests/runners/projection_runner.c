/* The CST/AST split's own gates (Stage 1, §12).
 *
 * Every case here drives the raw parser -- feed, finish, and the parser's
 * internal state -- so, like stream_runner, this links the static engine
 * rather than the facade. The corpus is the same fixture set the goldens pin,
 * so every example is a case for free.
 *
 *   projection_runner --case NAME --spec FILE
 *
 * closed_after_finish: a FINISHED tree contains no block still carrying
 * MARKDOWN_CORE_NODE__OPEN. §12.8 Q3 measured the violation: every table cell
 * and the header row shipped open, because `make_block` sets the flag,
 * `finalize` is the only clearer, and neither is ever on the open spine. The
 * flag is the closed signal Stage 1 schedules projections on, so an open block
 * in a finished tree is a lie about completeness.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"

#include "markdown-core.h"
#include "markdown-core-extensions.h"

#include "iterator.h"
#include "node.h"
#include "parser.h"

/* Every extension and the footnote option, because the point is coverage of
 * every node KIND a parse can put in a tree, not agreement with any golden. */
static markdown_core_parser *pr_parser_new(void) {
    markdown_core_parser *parser = markdown_core_parser_new(MARKDOWN_CORE_OPT_DEFAULT | MARKDOWN_CORE_OPT_FOOTNOTES);
    if (!parser) {
        return NULL;
    }
    if (!markdown_core_core_extensions_attach(
            parser, MARKDOWN_CORE_CORE_EXTENSION_TABLE | MARKDOWN_CORE_CORE_EXTENSION_STRIKETHROUGH |
                        MARKDOWN_CORE_CORE_EXTENSION_AUTOLINK | MARKDOWN_CORE_CORE_EXTENSION_TASKLIST |
                        MARKDOWN_CORE_CORE_EXTENSION_FORMULA | MARKDOWN_CORE_CORE_EXTENSION_DIRECTIVE)) {
        markdown_core_parser_free(parser);
        return NULL;
    }
    return parser;
}

static markdown_core_node *pr_parse(const char *text, size_t length) {
    markdown_core_parser *parser = pr_parser_new();
    markdown_core_node *root;
    if (!parser) {
        return NULL;
    }
    markdown_core_parser_feed(parser, text, length);
    root = markdown_core_parser_finish(parser);
    markdown_core_parser_free(parser);
    return root;
}

static int pr_count_open_blocks(markdown_core_node *root, int example, size_t *nodes_seen) {
    markdown_core_iter *iter = markdown_core_iter_new(root);
    markdown_core_event_type ev_type;
    int open_nodes = 0;
    if (!iter) {
        return -1;
    }
    while ((ev_type = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = markdown_core_iter_get_node(iter);
        if (ev_type != MARKDOWN_CORE_EVENT_ENTER) {
            continue;
        }
        (*nodes_seen)++;
        if (node->flags & MARKDOWN_CORE_NODE__OPEN) {
            fprintf(stderr, "example %d: %s at %d:%d is still open after finish\n", example,
                    markdown_core_node_get_type_string(node), node->start_line, node->start_column);
            open_nodes++;
        }
    }
    markdown_core_iter_free(iter);
    return open_nodes;
}

static int case_closed_after_finish(const ts_spec_file *file) {
    size_t index;
    size_t nodes_seen = 0;
    int failures = 0;
    for (index = 0; index < file->count; index++) {
        const ts_spec_case *test_case = &file->cases[index];
        markdown_core_node *root = pr_parse(test_case->markdown, test_case->markdown_length);
        int open_nodes;
        if (!root) {
            fprintf(stderr, "example %d: parse failed\n", test_case->example);
            failures++;
            continue;
        }
        open_nodes = pr_count_open_blocks(root, test_case->example, &nodes_seen);
        if (open_nodes != 0) {
            failures++;
        }
        markdown_core_node_free(root);
    }
    printf("closed after finish: %zu/%zu examples clean, %zu nodes\n", file->count - (size_t)failures, file->count,
           nodes_seen);
    return failures ? -1 : 0;
}

typedef struct pr_case_entry {
    const char *name;
    int (*run)(const ts_spec_file *file);
} pr_case_entry;

static const pr_case_entry PR_CASES[] = {
    {"closed_after_finish", case_closed_after_finish},
};

int main(int argc, char **argv) {
    const char *case_name = NULL;
    const char *spec = NULL;
    ts_spec_file file;
    size_t i;
    int arg;

    for (arg = 1; arg < argc; arg++) {
        if (strcmp(argv[arg], "--list") == 0) {
            for (i = 0; i < sizeof(PR_CASES) / sizeof(PR_CASES[0]); i++) {
                puts(PR_CASES[i].name);
            }
            return 0;
        } else if (strcmp(argv[arg], "--case") == 0 && arg + 1 < argc) {
            case_name = argv[++arg];
        } else if (strcmp(argv[arg], "--spec") == 0 && arg + 1 < argc) {
            spec = argv[++arg];
        } else {
            fputs("usage: projection_runner [--list] --case NAME --spec FILE\n", stderr);
            return 2;
        }
    }
    if (!case_name || !spec) {
        fputs("usage: projection_runner [--list] --case NAME --spec FILE\n", stderr);
        return 2;
    }
    if (ts_spec_load(spec, &file) != 0) {
        fprintf(stderr, "projection_runner: cannot load %s\n", spec);
        return 2;
    }
    for (i = 0; i < sizeof(PR_CASES) / sizeof(PR_CASES[0]); i++) {
        if (strcmp(PR_CASES[i].name, case_name) == 0) {
            int failed = PR_CASES[i].run(&file) != 0;
            ts_spec_free(&file);
            printf("%s %s\n", case_name, failed ? "[FAILED]" : "[PASSED]");
            return failed;
        }
    }
    ts_spec_free(&file);
    fprintf(stderr, "unknown case: %s\n", case_name);
    return 2;
}
