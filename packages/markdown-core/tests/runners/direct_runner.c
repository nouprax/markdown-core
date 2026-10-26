/* Direct reader suite (elements/properties.c, THE DIRECT READER).
 *
 * The parse reads the members of the common shapes with a direct reader
 * and leaves the rest to the YAML parser. The YAML parser is the oracle:
 * every envelope of every example of each spec fixture, and every
 * generated one, is read both ways, and the two readings must set the same
 * fields to the same values. The generated envelopes put each probe value
 * in each member template, with each line ending, between two members.
 *
 *   direct_runner [FILE...]
 *
 * The run fails when a reading differs, or when the direct reader read no
 * member, which would leave it untested. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "properties.h"
#include "test_support.h"

/* A member template: `@` stands for the probe value. */
static const char *const MEMBERS[] = {
    "title: @\n",
    "title:@\n",
    "title:  @  \n",
    "title: @ # note\n",
    "title: \"@\"\n",
    "title: '@'\n",
    "title: \"@\" # note\n",
    "time: @\n",
    "title: @\n\n  \n",
    "title: @\n  more\n",
    "title : @\n",
    "\"title\": @\n",
    "  title: @\n",
    "authors: [@]\n",
    "authors: [a, @, b]\n",
    "authors: [@,]\n",
    "authors: [ @ ] # note\n",
    "authors: [\"@\", '@']\n",
    "authors: [@\n",
    "authors:\n  - @\n",
    "abstract: |\n  @\n",
    "abstract: |\n  x\n  @\n",
    "abstract: |\n\n  @\n\n",
    "abstract: |\n    @\n  x\n",
    "abstract: |\n  @\n    deeper\n\n  last\n",
    "abstract: |\n     \n  @\n",
    "abstract: |\n  @\n     \n",
    "abstract: | # note\n  @\n",
    "abstract: |-\n  @\n",
    "abstract: >\n  @\n",
    "abstract: |\n\t@\n",
    "state: |\n  @\n",
    "comment: @\n",
};

/* The probe values: plain text, the indicators, the separators, the
 * resolved words, the line breaks and the characters YAML does not print
 * or reads as line breaks. */
static const char *const PROBES[] = {
    "",
    "a",
    "a b",
    "a  b",
    "a #b",
    "a#b",
    "a: b",
    "a:b",
    "a:",
    "a:?b",
    "-1",
    "-1.5e+2",
    "-",
    "- a",
    "-a",
    "?a",
    ":a",
    "? a",
    "true",
    "false",
    "null",
    "Null",
    "~",
    "0",
    "01",
    "1e3",
    "0x1",
    "a,b",
    "[a]",
    "{a}",
    "a]",
    "a[",
    "a}",
    "a{",
    "'a'",
    "\"a\"",
    "a'b",
    "a''b",
    "a\"b",
    "a\\b",
    "a\\\"b",
    "\\",
    "#",
    " #a",
    "a ",
    " a",
    "|",
    ">",
    "&a",
    "*a",
    "!a",
    "%a",
    "@a",
    "`a",
    "...",
    "---",
    "a\tb",
    "\t",
    "a\rb",
    "a\n b",
    "a\nb",
    "\x01",
    "\x7f",
    "\xc2\x85",
    "\xc2\xa0",
    "\xc3\xa9",
    "\xe4\xb8\xad",
    "\xf0\xa0\x80\x80",
    "\xe2\x80\xa8",
    "\xe2\x80\xa9",
    "\xef\xbb\xbf",
    "\xef\xbf\xbe",
    "\xef\xbf\xbf",
    "\xed\xa0\x80",
    "\xff",
    "\xc3",
    "\xe0\x80\xaf",
    "a,",
    ",a",
    "a, b",
    "a ,b",
    "a ]",
    "a # b",
    "  ",
    "a\n\nb",
};

static const char *const ENDINGS[] = {"\n", "\r\n", "\r"};

typedef struct {
    size_t envelopes, taken, failures;
} tally;

static bool same_string(markdown_core_string a, markdown_core_string b) {
    return a.length == b.length && (!a.length || !memcmp(a.data, b.data, a.length));
}

static bool same_value(const markdown_core_metadata_value *a, const markdown_core_metadata_value *b) {
    if (a->kind != b->kind) {
        return false;
    }
    if (a->kind == MARKDOWN_CORE_METADATA_SCALAR) {
        const markdown_core_metadata_scalar *x = &a->as.scalar, *y = &b->as.scalar;
        return x->kind == y->kind &&
               (x->kind == MARKDOWN_CORE_METADATA_NULL ||
                (x->kind == MARKDOWN_CORE_METADATA_BOOL ? x->value.boolean == y->value.boolean
                                                        : same_string(x->value.string, y->value.string)));
    }
    if (a->kind == MARKDOWN_CORE_METADATA_LIST) {
        if (a->as.list.count != b->as.list.count) {
            return false;
        }
        for (size_t i = 0; i < a->as.list.count; i++) {
            if (a->as.list.items[i].kind != b->as.list.items[i].kind ||
                !same_string(a->as.list.items[i].value, b->as.list.items[i].value)) {
                return false;
            }
        }
    }
    return true;
}

static bool same_fields(const markdown_core_metadata_fields *a, const markdown_core_metadata_fields *b) {
    return same_value(&a->name, &b->name) && same_value(&a->title, &b->title) &&
           same_value(&a->subtitle, &b->subtitle) && same_value(&a->time, &b->time) && same_value(&a->date, &b->date) &&
           same_value(&a->authors, &b->authors) && same_value(&a->keywords, &b->keywords) &&
           same_value(&a->abstract, &b->abstract) && same_value(&a->state, &b->state) &&
           same_value(&a->comment, &b->comment);
}

static void show(const char *label, const uint8_t *text, size_t length) {
    fprintf(stderr, "  %s: \"", label);
    for (size_t i = 0; i < length; i++) {
        const uint8_t c = text[i];
        if (c == '\n') {
            fputs("\\n", stderr);
        } else if (c == '\r') {
            fputs("\\r", stderr);
        } else if (c == '\t') {
            fputs("\\t", stderr);
        } else if (c == '"' || c == '\\') {
            fprintf(stderr, "\\%c", c);
        } else if (c < 0x20 || c >= 0x7f) {
            fprintf(stderr, "\\x%02x", c);
        } else {
            fputc(c, stderr);
        }
    }
    fputs("\"\n", stderr);
}

/* Reads the envelope `text`, `length` bytes through its closing fence,
 * both ways. */
static void check(tally *t, const char *where, const uint8_t *text, size_t length) {
    markdown_core_metadata_fields direct = {0}, oracle = {0};
    size_t taken = 0, none = 0;
    const bool read = markdown_core_metadata_fields_read(text, length, true, &direct, &taken);
    const bool known = markdown_core_metadata_fields_read(text, length, false, &oracle, &none);
    t->envelopes++;
    t->taken += taken;
    if (!read || !known || !same_fields(&direct, &oracle)) {
        t->failures++;
        fprintf(stderr, "%s: the direct reading differs from the YAML parser's\n", where);
        show("envelope", text, length);
    }
    markdown_core_metadata_fields_free(&direct);
    markdown_core_metadata_fields_free(&oracle);
}

/* The envelope an example opens with: through its closing fence. */
static size_t envelope(const uint8_t *text, size_t length) {
    if (length < 4 || memcmp(text, "---", 3) || (text[3] != '\n' && text[3] != '\r')) {
        return 0;
    }
    for (size_t at = 4; at + 3 <= length; at++) {
        if ((text[at - 1] == '\n' || text[at - 1] == '\r') && !memcmp(text + at, "---", 3) &&
            (at + 3 == length || text[at + 3] == '\n' || text[at + 3] == '\r')) {
            return at + 3;
        }
    }
    return 0;
}

static bool fixture(tally *t, const char *path) {
    ts_spec_file file;
    if (ts_spec_load(path, &file)) {
        fprintf(stderr, "%s: cannot read the fixture\n", path);
        return false;
    }
    for (size_t i = 0; i < file.count; i++) {
        const ts_spec_case *c = &file.cases[i];
        const size_t length = envelope((const uint8_t *)c->markdown, c->markdown_length);
        if (length) {
            char where[512];
            snprintf(where, sizeof(where), "%s:%d", path, c->start_line);
            check(t, where, (const uint8_t *)c->markdown, length);
        }
    }
    ts_spec_free(&file);
    return true;
}

/* Writes `piece` into `out` at `*at`, each line feed as `ending`. */
static void put(char *out, size_t *at, const char *piece, const char *ending) {
    for (const char *c = piece; *c; c++) {
        if (*c == '\n') {
            for (const char *e = ending; *e; e++) {
                out[(*at)++] = *e;
            }
        } else {
            out[(*at)++] = *c;
        }
    }
}

static void generate(tally *t) {
    char text[1024];
    for (size_t m = 0; m < sizeof(MEMBERS) / sizeof(*MEMBERS); m++) {
        for (size_t v = 0; v < sizeof(PROBES) / sizeof(*PROBES); v++) {
            for (size_t e = 0; e < sizeof(ENDINGS) / sizeof(*ENDINGS); e++) {
                size_t at = 0;
                put(text, &at, "---\nname: before\n", ENDINGS[e]);
                for (const char *c = MEMBERS[m]; *c; c++) {
                    if (*c == '@') {
                        for (const char *p = PROBES[v]; *p; p++) {
                            text[at++] = *p;
                        }
                    } else if (*c == '\n') {
                        put(text, &at, "\n", ENDINGS[e]);
                    } else {
                        text[at++] = *c;
                    }
                }
                put(text, &at, "state: after\n---", ENDINGS[e]);
                char where[64];
                snprintf(where, sizeof(where), "member %zu, probe %zu, ending %zu", m, v, e);
                check(t, where, (const uint8_t *)text, at);
            }
        }
    }
}

int main(int argc, char **argv) {
    tally t = {0};
    for (int i = 1; i < argc; i++) {
        if (!fixture(&t, argv[i])) {
            return 1;
        }
    }
    generate(&t);
    printf("%zu envelopes, %zu members read directly, %zu failures\n", t.envelopes, t.taken, t.failures);
    if (!t.taken) {
        fprintf(stderr, "the direct reader read no member\n");
        return 1;
    }
    return t.failures ? 1 : 0;
}
