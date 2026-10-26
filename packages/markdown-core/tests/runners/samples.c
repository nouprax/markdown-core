#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "samples.h"

/* THE GENERATED INPUTS: each atom inside each inner construct inside each
 * outer one, where the nesting and the escapes the fixtures write one at a
 * time meet. A construct is a template whose `@` the next one fills. An
 * atom's \001 stands for NUL, which a C string cannot hold and which the
 * parser reads as U+FFFD. */
static const char *const CONSTRUCTS[] = {
    "[@](u)",
    "[x](@)",
    "[x](u \"@\")",
    "[@]{.c}",
    "[x]{k=\"@\"}",
    "[x]{@=1}",
    "[x]{.@}",
    "*@*",
    "`@`",
    ":d[@]{.a}",
    ":d{k=@}",
    "[@k; @]",
    "@{@}",
    "[[@]]",
    "[[a|@]]",
    "![[@]]",
    "[^@]",
    "^[@]",
    "#@",
    "# @ {.c}",
    "> @",
    "- @",
    "| @ |\n| - |",
    "[r]: @",
    "> [!@]",
    "<i t=\"@\">",
    "\\@",
    "x @ y",
    "[@][r]\n\n[r]: u",
    "x @ y #i#",
    "$@$",
    "$$$@$$$",
    "%%@%%",
    "<!--@-->",
    "```\n@\n```",
};
static const char *const ATOMS[] = {
    "a",   "a b", "\\]", "\\}", "&amp;", "\"q\"",    "{.x}",   "#t",  "@k",  "]",      "}",
    "<u>", "`",   "(",   "|",   "#t#",   "\\&#35;t", "&#35;t", "a#b", "\\#", "a\001b",
};
enum {
    CONSTRUCT_COUNT = sizeof(CONSTRUCTS) / sizeof(CONSTRUCTS[0]),
    ATOM_COUNT = sizeof(ATOMS) / sizeof(ATOMS[0]),
};

/* `template` with its first `@` filled with `with`, in a new string. */
static char *fill(const char *template, const char *with) {
    const char *hole = strchr(template, '@');
    const size_t head = (size_t)(hole - template), size = strlen(template) + strlen(with);
    char *out = malloc(size);
    if (out) {
        memcpy(out, template, head);
        strcpy(out + head, with);
        strcat(out, hole + 1);
    }
    return out;
}

bool samples_each(void (*check)(void *context, const char *source, size_t length, int number), void *context) {
    for (size_t outer = 0; outer < CONSTRUCT_COUNT; outer++) {
        for (size_t inner = 0; inner < CONSTRUCT_COUNT; inner++) {
            for (size_t atom = 0; atom < ATOM_COUNT; atom++) {
                char *in = fill(CONSTRUCTS[inner], ATOMS[atom]);
                char *source = in ? fill(CONSTRUCTS[outer], in) : NULL;
                free(in);
                if (!source) {
                    return false;
                }
                /* Numbered by place, not by count, so a construct or an atom
                 * added at a list's end leaves every other's number. */
                const int number = (int)(outer * 10000 + inner * 100 + atom + 1);
                if (getenv("ORACLE_VERBOSE")) {
                    fprintf(stderr, "generated:%d: %s\n", number, source);
                }
                const size_t length = strlen(source);
                for (char *at = source; (at = strchr(at, '\x01')) != NULL;) {
                    *at = '\0';
                }
                check(context, source, length, number);
                free(source);
            }
        }
    }
    return true;
}
