/* Context oracle runner (docs/plans/2026-10-23-lsp-completion-engine.md,
 * section 8).
 *
 * The finished parse is the oracle for the context query. At every
 * position of every example of each spec fixture, the runner asks for the
 * context and checks each closed slot against the parse of edited text,
 * comparing canonical dumps with their scopes removed:
 *
 * - SUBSTITUTION. The slot's source, `before` and `after`, replaced with a
 *   sentinel, parses to the same tree, with the sentinel in properties of
 *   one node of the slot's owner kind and nothing else changed.
 * - BOUNDS. The sentinel inserted at the slot's start, and then at its end,
 *   lands at the start and at the end of the value each of those properties
 *   lost: the slot's source is exactly the value's, no more and no less.
 *   An empty slot instead parses to the same tree under two sentinels.
 *
 * At a position with no closed slot, the sentinel inserted at the cursor
 * must not grow a property of a construct a slot names with nothing else
 * changed: that is a field the cursor writes and the query missed.
 *
 *   oracle_runner [--exceptions FILE] [--generate] [FILE...]
 *
 * `--generate` also checks the inputs the runner makes: each of a list of
 * atoms inside each construct inside each construct (check_generated),
 * named `generated:N` in the reports.
 *
 * An exceptions file names the failures the oracle cannot judge, each with
 * its reason (read_exceptions); one that no longer occurs fails the run.
 *
 * The context fixture's cursor marks are dropped, so its examples are
 * documents like any other. */
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "samples.h"
#include "test_support.h"

static const char CURSOR[] = "\xe2\x80\xb8";
static const char SENTINEL[] = "qzxq";
static const char OTHER[] = "vkwv";
enum { SENTINEL_LENGTH = sizeof(SENTINEL) - 1 };

/* The PROPERTIES a slot completes, by the kind of node that holds them
 * (the slot model of scripts/tooling/generate-node-kinds.mjs): any node's
 * anchor and attributes, and the fields of the owners the model names. */
static const char *const FIELDS[][2] = {
    {"", "anchor"},        {"", "attributes"},      {"Link", "dest"},          {"Embedded", "dest"},
    {"Reference", "dest"}, {"CrossLink", "dest"},   {"CrossEmbedded", "dest"}, {"Reference", "label"},
    {"Footnote", "label"}, {"Specimen", "label"},   {"Citation", "referent"},  {"DirectiveBlock", "name"},
    {"Directive", "name"}, {"Topic", "name"},       {"Callout", "variant"},    {"CodeBlock", "language"},
    {"Metadata", "name"},  {"Metadata", "title"},   {"Metadata", "subtitle"},  {"Metadata", "time"},
    {"Metadata", "date"},  {"Metadata", "authors"}, {"Metadata", "keywords"},  {"Metadata", "abstract"},
    {"Metadata", "state"}, {"Metadata", "comment"},
};

/* The properties grammar's field names, a Metadata's fields. */
static const char *const NAMES[] = {"name",    "title",    "subtitle", "time",  "date",
                                    "authors", "keywords", "abstract", "state", "comment"};
enum { NAME_COUNT = sizeof(NAMES) / sizeof(NAMES[0]) };

/* `memmem` and `strdup` are POSIX, not C: the runner builds on Windows too. */
static const char *seek(const char *start, size_t length, const char *needle, size_t size) {
    for (size_t i = 0; size <= length && i <= length - size; i++) {
        if (!memcmp(start + i, needle, size)) {
            return start + i;
        }
    }
    return NULL;
}

static char *copy(const char *text) {
    const size_t size = strlen(text) + 1;
    char *out = malloc(size);
    if (out) {
        memcpy(out, text, size);
    }
    return out;
}

/* A canonical dump with its scopes removed, cut into lines. */
typedef struct tree {
    char *text;
    char **lines;
    size_t count;
} tree;

static void tree_free(tree *t) {
    free(t->text);
    free(t->lines);
    *t = (tree){0};
}

/* The brackets an unquoted value holds, which the line's words would read
 * as nesting: each stands in as a control byte of its own, `(` as 1. */
static const char BRACKETS[] = "()[]{}";

/* Removes every ` scope=...` from `text` in place, and the quotes the dump
 * puts around a class or an identifier that is not plain ASCII, so that a
 * value reads alike whatever it holds: `."a b"` is `.a b`. */
static void strip_scopes(char *text) {
    char *out = text;
    bool quoted = false;
    for (const char *in = text; *in;) {
        if (quoted || *in == '"') {
            /* A quoted string holds its bytes as they are. */
            if (quoted && *in == '\\' && in[1]) {
                *out++ = *in++;
            } else if (*in == '"' || *in == '\n') {
                quoted = !quoted && *in == '"';
            }
            *out++ = *in++;
            continue;
        }
        if (!strncmp(in, " scope=", 7)) {
            in += 7;
            while (*in && *in != ' ' && *in != '\n') {
                in++;
            }
            continue;
        }
        if ((*in == '.' || *in == '#') && in[1] == '"' && in > text && (in[-1] == '{' || in[-1] == ' ')) {
            *out++ = *in;
            in += 2;
            while (*in && *in != '"' && *in != '\n') {
                if (*in == '\\' && in[1]) {
                    *out++ = *in++;
                }
                const char *bracket = strchr(BRACKETS, *in);
                *out++ = bracket && *in ? (char)(1 + (bracket - BRACKETS)) : *in;
                in++;
            }
            if (*in == '"') {
                in++;
            }
            continue;
        }
        *out++ = *in++;
    }
    *out = 0;
}

static char *swapped(char *text, const char *from, const char *to);

/* `text` with a Metadata's scalars all read as text, in place of `text`: a
 * plain scalar's type follows from its bytes, which an edit changes, so
 * `2020` reads as `text("2020")` and an empty value or `null` as
 * `text("null")`. */
static char *scalars_as_text(char *text) {
    if (!text || !strstr(text, "Metadata ")) {
        return text;
    }
    text = swapped(text, "number(\"", "text(\"");
    text = swapped(text, "bool(true)", "text(\"true\")");
    text = swapped(text, "bool(false)", "text(\"false\")");
    return swapped(text, "scalar(null)", "scalar(text(\"null\"))");
}

static bool parse_tree(const uint8_t *source, size_t length, tree *out) {
    *out = (tree){0};
    markdown_core_document *document = NULL;
    if (markdown_core_document_parse(source, length, &document) != MARKDOWN_CORE_OK) {
        return false;
    }
    uint8_t *dump = NULL;
    size_t size = 0;
    const markdown_core_status status =
        markdown_core_document_dump(document, markdown_core_document_root(document), source, length, &dump, &size);
    markdown_core_document_free(document);
    if (status != MARKDOWN_CORE_OK) {
        return false;
    }
    out->text = malloc(size + 1);
    if (!out->text) {
        markdown_core_dump_free(dump);
        return false;
    }
    memcpy(out->text, dump, size);
    out->text[size] = 0;
    markdown_core_dump_free(dump);
    strip_scopes(out->text);
    if (!(out->text = scalars_as_text(out->text))) {
        return false;
    }
    size_t lines = 1;
    for (const char *c = out->text; *c; c++) {
        lines += *c == '\n';
    }
    out->lines = calloc(lines, sizeof(*out->lines));
    if (!out->lines) {
        tree_free(out);
        return false;
    }
    for (char *line = out->text; *line;) {
        char *end = strchr(line, '\n');
        out->lines[out->count++] = line;
        if (!end) {
            break;
        }
        *end = 0;
        line = end + 1;
    }
    return true;
}

/* `source` with `[from, to)` replaced by `with`. */
static uint8_t *splice(const uint8_t *source, size_t length, size_t from, size_t to, const char *with,
                       size_t *out_length) {
    const size_t size = strlen(with);
    *out_length = length - (to - from) + size;
    uint8_t *out = malloc(*out_length + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, source, from);
    memcpy(out + from, with, size);
    memcpy(out + from + size, source + to, length - to);
    out[*out_length] = 0;
    return out;
}

static bool parse_spliced(const uint8_t *source, size_t length, size_t from, size_t to, const char *with, tree *out) {
    size_t size;
    uint8_t *text = splice(source, length, from, to, with, &size);
    const bool ok = text && parse_tree(text, size, out);
    free(text);
    return ok;
}

/* A dump line's parts: the node's kind, and its properties as
 * `key=value` words, split at spaces outside quotes and brackets. */
enum { MAX_WORDS = 32 };
typedef struct word {
    const char *start;
    size_t length, key;
} word;
typedef struct line_parts {
    const char *kind;
    size_t kind_length, count;
    word words[MAX_WORDS];
} line_parts;

static void split_line(const char *line, line_parts *out) {
    const char *at = line, *mark;
    while ((mark = strstr(at, "\xe2\x94\x80\xe2\x94\x80 "))) {
        at = mark + 7;
    }
    while (*at == ' ') {
        at++;
    }
    out->kind = at;
    while (*at && *at != ' ') {
        at++;
    }
    out->kind_length = (size_t)(at - out->kind);
    out->count = 0;
    while (*at == ' ') {
        at++;
    }
    while (*at && out->count < MAX_WORDS) {
        const char *start = at;
        /* Only the outermost bracket's kind nests: an attribute's key may
         * hold a `(` that no `)` closes. */
        int depth = 0;
        char open = 0, close = 0;
        bool quoted = false;
        for (; *at && (quoted || depth || *at != ' '); at++) {
            if (quoted) {
                if (*at == '\\' && at[1]) {
                    at++;
                } else if (*at == '"') {
                    quoted = false;
                }
            } else if (*at == '"') {
                quoted = true;
            } else if (!depth && (*at == '(' || *at == '{' || *at == '[')) {
                open = *at;
                close = *at == '(' ? ')' : *at == '{' ? '}' : ']';
                depth = 1;
            } else if (depth && *at == open) {
                depth++;
            } else if (depth && *at == close) {
                depth--;
            }
        }
        const char *equals = memchr(start, '=', (size_t)(at - start));
        out->words[out->count++] =
            (word){start, (size_t)(at - start), equals ? (size_t)(equals - start) : (size_t)(at - start)};
        while (*at == ' ') {
            at++;
        }
    }
}

static bool same_kind(const line_parts *a, const char *kind, size_t length) {
    return a->kind_length == length && !memcmp(a->kind, kind, length);
}

/* One property that differs between two trees: line `line`, the word at
 * `index`, `was` before and `now` after. */
enum { MAX_CHANGES = 8 };
typedef struct change {
    size_t line, index;
    word was, now;
} change;
typedef struct changes {
    change at[MAX_CHANGES];
    size_t count;
    const char *why;
} changes;

/* The properties that differ between `a` and `b`, whose lines must agree
 * in number, kind and keys; false, with `why`, when they do not. */
static bool differ(const tree *a, const tree *b, changes *out) {
    out->count = 0;
    out->why = NULL;
    if (a->count != b->count) {
        out->why = "the tree changes shape";
        return false;
    }
    for (size_t i = 0; i < a->count; i++) {
        if (!strcmp(a->lines[i], b->lines[i])) {
            continue;
        }
        if (getenv("ORACLE_VERBOSE")) {
            fprintf(stderr, "  - %s\n  + %s\n", a->lines[i], b->lines[i]);
        }
        line_parts x, y;
        split_line(a->lines[i], &x);
        split_line(b->lines[i], &y);
        if (x.kind_length != y.kind_length || memcmp(x.kind, y.kind, x.kind_length) || x.count != y.count) {
            out->why = "a node changes kind";
            return false;
        }
        for (size_t k = 0; k < x.count; k++) {
            const word *p = &x.words[k], *q = &y.words[k];
            if (p->key != q->key || memcmp(p->start, q->start, p->key)) {
                out->why = "a node changes properties";
                return false;
            }
            if (p->length == q->length && !memcmp(p->start, q->start, p->length)) {
                continue;
            }
            if (out->count == MAX_CHANGES) {
                out->why = "too many properties change";
                return false;
            }
            out->at[out->count++] = (change){i, k, *p, *q};
        }
    }
    return true;
}

static bool contains(const char *data, size_t length, const char *needle) {
    const size_t size = strlen(needle);
    for (size_t i = 0; i + size <= length; i++) {
        if (!memcmp(data + i, needle, size)) {
            return true;
        }
    }
    return false;
}

/* Where `now` replaced a run of `was` with the sentinel: `[*from, *to)` in
 * `was`. False when it did not. */
static bool replaced(const word *was, const word *now, size_t *from, size_t *to) {
    size_t prefix = 0, suffix = 0;
    while (prefix < was->length && prefix < now->length && was->start[prefix] == now->start[prefix]) {
        prefix++;
    }
    while (suffix < was->length - prefix && suffix < now->length - prefix &&
           was->start[was->length - 1 - suffix] == now->start[now->length - 1 - suffix]) {
        suffix++;
    }
    /* The sentinel's letters may match the value's around it. */
    for (size_t back = 0; back <= SENTINEL_LENGTH && back <= prefix; back++) {
        const size_t at = prefix - back;
        if (at + SENTINEL_LENGTH <= now->length && !memcmp(now->start + at, SENTINEL, SENTINEL_LENGTH)) {
            const size_t rest = now->length - at - SENTINEL_LENGTH;
            if (rest <= was->length - at &&
                !memcmp(now->start + at + SENTINEL_LENGTH, was->start + was->length - rest, rest)) {
                *from = at;
                *to = was->length - rest;
                return true;
            }
        }
    }
    return false;
}

/* Whether `now` is `was` with the sentinel inserted at `at`. */
static bool inserted_at(const word *was, const word *now, size_t at) {
    return now->length == was->length + SENTINEL_LENGTH && at <= was->length && !memcmp(now->start, was->start, at) &&
           !memcmp(now->start + at, SENTINEL, SENTINEL_LENGTH) &&
           !memcmp(now->start + at + SENTINEL_LENGTH, was->start + at, was->length - at);
}

/* Whether `now` is `was` with the sentinel inserted somewhere. */
static bool inserted(const word *was, const word *now) {
    for (size_t at = 0; at <= was->length; at++) {
        if (inserted_at(was, now, at)) {
            return true;
        }
    }
    return false;
}

/* AN EXCEPTION: a failure the oracle cannot judge, by the fixture's file
 * name, the example's first line and what failed, with the reason the file
 * gives after `#`. An exception no failure meets is stale, and fails. */
typedef struct exception {
    char *file, *what;
    int line;
    size_t met;
} exception;

typedef struct report {
    const char *path;
    const ts_spec_case *example;
    size_t failures, checks, expected;
    exception *exceptions;
    size_t exception_count;
    /* The generated inputs checked so far. */
    size_t generated;
} report;

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* Whether the failure `what` of the current example is an exception. */
static bool excepted(report *r, const char *what) {
    for (size_t i = 0; i < r->exception_count; i++) {
        exception *e = &r->exceptions[i];
        if (e->line == r->example->start_line && !strcmp(e->what, what) && !strcmp(e->file, base_name(r->path))) {
            e->met++;
            return true;
        }
    }
    return false;
}

/* Reads the exceptions at `path`: one per line, `FILE:LINE WHAT # reason`,
 * and blank lines and lines starting with `#` between them. */
static bool read_exceptions(report *r, const char *path) {
    FILE *in = fopen(path, "r");
    if (!in) {
        return false;
    }
    char text[1024];
    while (fgets(text, sizeof(text), in)) {
        char *hash = strchr(text, '#');
        if (hash) {
            *hash = 0;
        }
        char *end = text + strlen(text);
        while (end > text && (end[-1] == ' ' || end[-1] == '\n' || end[-1] == '\r')) {
            *--end = 0;
        }
        char *colon = strchr(text, ':'), *space = colon ? strchr(colon, ' ') : NULL;
        if (!*text) {
            continue;
        }
        if (!colon || !space) {
            fclose(in);
            return false;
        }
        exception *grown = realloc(r->exceptions, (r->exception_count + 1) * sizeof(*grown));
        if (!grown) {
            fclose(in);
            return false;
        }
        r->exceptions = grown;
        *colon = *space = 0;
        exception *e = &r->exceptions[r->exception_count++];
        *e = (exception){copy(text), copy(space + 1), atoi(colon + 1), 0};
        if (!e->file || !e->what) {
            fclose(in);
            return false;
        }
    }
    fclose(in);
    return true;
}

/* Prints a failure with the cursor's line, the field `[from, to)` on it
 * between `[[` and `]]`, or the cursor alone when `from` is SIZE_MAX. */
static void fail_at(report *r, const uint8_t *source, size_t length, size_t cursor, size_t from, size_t to,
                    const char *what, const char *why) {
    if (excepted(r, what)) {
        r->expected++;
        if (!getenv("ORACLE_VERBOSE")) {
            return;
        }
        fputs("(excepted) ", stderr);
    } else {
        r->failures++;
    }
    if (r->failures > 100000) {
        return;
    }
    size_t line_start = cursor, line_end = cursor;
    while (line_start > 0 && source[line_start - 1] != '\n') {
        line_start--;
    }
    while (line_end < length && source[line_end] != '\n') {
        line_end++;
    }
    fprintf(stderr, "%s:%d: %s: %s\n    ", r->path, r->example->start_line, what, why);
    for (size_t at = line_start; at <= line_end; at++) {
        if (at == from) {
            fputs("[[", stderr);
        }
        if (at == cursor) {
            fputs(CURSOR, stderr);
        }
        if (at == to && from != SIZE_MAX) {
            fputs("]]", stderr);
        }
        if (at < line_end) {
            fputc(source[at], stderr);
        }
    }
    fputc('\n', stderr);
}

static void fail(report *r, const uint8_t *source, size_t length, size_t cursor, const char *what, const char *why) {
    fail_at(r, source, length, cursor, SIZE_MAX, SIZE_MAX, what, why);
}

/* Whether `w` is a property through which one node names another: an
 * anchor, which may be deduplicated or made from a heading's text, or a
 * citation's referent, which a specimen's label decides. */
static bool naming(const word *w) {
    return (w->key == 6 && !memcmp(w->start, "anchor", 6)) || (w->key == 8 && !memcmp(w->start, "referent", 8));
}

/* Whether `line` is a node of kind `owner` with the sentinel in a property
 * the slot writes: an anchor only for an identifier or a value, which may
 * be an `id=`, since a heading makes its anchor from its text. */
static bool takes_sentinel(const char *line, const char *owner, bool id) {
    line_parts parts;
    split_line(line, &parts);
    if (!same_kind(&parts, owner, strlen(owner))) {
        return false;
    }
    for (size_t i = 0; i < parts.count; i++) {
        const word *w = &parts.words[i];
        const bool anchor = w->key == 6 && !memcmp(w->start, "anchor", 6);
        if ((id || !anchor) && contains(w->start, w->length, SENTINEL)) {
            return true;
        }
    }
    return false;
}

/* How a slot's field is checked. A label is checked with a definition
 * appended that keeps the edited label resolved: a reference whose label
 * matches no definition is text. A record, a whole attribute member, may
 * change which properties the member sets, so only its node is checked. */
typedef enum check_mode {
    CHECK_PLAIN,
    CHECK_CALL,
    CHECK_DEFINITION,
    CHECK_NOTE_CALL,
    CHECK_NOTE_DEFINITION,
    CHECK_RECORD
} check_mode;

/* `source` with `[from, to)` replaced by `with`, then `tail` appended, parsed. */
static bool parse_edit(const uint8_t *source, size_t length, size_t from, size_t to, const char *with, const char *tail,
                       tree *out) {
    size_t size;
    *out = (tree){0};
    uint8_t *text = splice(source, length, from, to, with, &size);
    if (!text) {
        return false;
    }
    const size_t extra = strlen(tail);
    uint8_t *full = realloc(text, size + extra + 1);
    if (!full) {
        free(text);
        return false;
    }
    memcpy(full + size, tail, extra + 1);
    const bool ok = parse_tree(full, size + extra, out);
    free(full);
    return ok;
}

/* The definitions a check appends: when the field is a reference's label,
 * one for the edited label, `label`, and one for the original, so that the
 * reference resolves before and after the edit; when it is a definition's,
 * one for the original label, which keeps its references resolved. */
static void appended(check_mode mode, const char *label, const uint8_t *token, size_t token_length, char *out,
                     size_t size) {
    if (mode == CHECK_CALL) {
        snprintf(out, size, "\n\n[%s]: /o\n\n[%.*s]: /o\n", label, (int)token_length, (const char *)token);
    } else if (mode == CHECK_DEFINITION) {
        snprintf(out, size, "\n\n[%.*s]: /o\n", (int)token_length, (const char *)token);
    } else if (mode == CHECK_NOTE_CALL) {
        snprintf(out, size, "\n\n[^%s]: o\n\n[^%.*s]: o\n", label, (int)token_length, (const char *)token);
    } else if (mode == CHECK_NOTE_DEFINITION) {
        snprintf(out, size, "\n\n[^%.*s]: o\n", (int)token_length, (const char *)token);
    } else {
        *out = 0;
    }
}

/* The closers a slot's construct may still need when the line's end cuts
 * it short: the completion's edit writes them (plan 3.1, `spell`). */
static const char *const CLOSERS[] = {"]", "}", "\"}", "'}", ")", "]]", NULL};

/* Whether the field holds a byte the syntax around it may read: a quote, a
 * bracket, a pipe, a backslash or white space. Taken out, the field may
 * leave the text around it another construct. */
static bool syntactic(const uint8_t *token, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (token[i] < 0x80 && strchr("\"'()[]{}<>|\\ \t\r\n", token[i])) {
            return true;
        }
    }
    return false;
}

/* THE KEPT-BYTES CHECK of a field `syntactic` finds, for when taking it out
 * changes the tree: the sentinel inserted at its start and the other one at
 * its end, both at once, land in one property of one node of the owner's
 * kind, the first before the second, and nothing else changes but naming
 * properties. */
static const char *verify_kept(const uint8_t *source, size_t length, size_t from, size_t to, const char *owner,
                               check_mode mode) {
    tree base = {0}, both = {0};
    changes diff;
    char tail[256], label[160];
    const char *why = NULL;
    size_t size;
    snprintf(label, sizeof(label), "%s%.*s%s", SENTINEL, (int)(to - from), (const char *)source + from, OTHER);
    appended(mode, label, source + from, to - from, tail, sizeof(tail));
    uint8_t *ended = splice(source, length, to, to, OTHER, &size);
    if (!ended || !parse_edit(source, length, 0, 0, "", tail, &base) ||
        !parse_edit(ended, size, from, from, SENTINEL, tail, &both)) {
        why = "the edited text does not parse";
    } else if (!differ(&base, &both, &diff)) {
        why = diff.why;
    } else {
        size_t own = 0;
        for (size_t i = 0; i < diff.count && !why; i++) {
            const word *was = &diff.at[i].was, *now = &diff.at[i].now;
            line_parts parts;
            split_line(both.lines[diff.at[i].line], &parts);
            const char *first = seek(now->start, now->length, SENTINEL, SENTINEL_LENGTH);
            const char *second = seek(now->start, now->length, OTHER, SENTINEL_LENGTH);
            if (!first && !second) {
                why = naming(was) ? NULL : "another node changes";
                continue;
            }
            if (!first || !second || second < first || !same_kind(&parts, owner, strlen(owner)) ||
                (mode != CHECK_RECORD && now->length != was->length + 2 * SENTINEL_LENGTH)) {
                why = "the field's start and end land apart";
                continue;
            }
            /* The property is the old one with the two sentinels put in; a
             * record's member is rewritten whole, as a name the sentinel
             * starts. */
            own++;
            if (mode == CHECK_RECORD) {
                continue;
            }
            const size_t at = (size_t)(first - now->start), end = (size_t)(second - now->start);
            if (memcmp(now->start, was->start, at) ||
                memcmp(now->start + at + SENTINEL_LENGTH, was->start + at, end - at - SENTINEL_LENGTH) ||
                memcmp(now->start + end + SENTINEL_LENGTH, was->start + end - SENTINEL_LENGTH,
                       now->length - end - SENTINEL_LENGTH)) {
                why = "a property changes other than by the field's value";
            }
        }
        if (!why && own != 1) {
            why = "no node of the owner's kind holds the sentinel";
        }
    }
    free(ended);
    tree_free(&base);
    tree_free(&both);
    return why;
}

/* Whether `[from, to)` of a Metadata's field word `w` is a whole string of
 * it, or a whole line of a literal's: a quote or an escaped line ending
 * before it and after it. */
static bool whole(const word *w, size_t from, size_t to) {
    const char *s = w->start;
    const bool starts = (from >= 1 && s[from - 1] == '"') || (from >= 2 && s[from - 2] == '\\' && s[from - 1] == 'n');
    const bool ends = to < w->length && (s[to] == '"' || (to + 1 < w->length && s[to] == '\\' && s[to + 1] == 'n'));
    return starts && ends;
}

static const char *verify(const uint8_t *source, size_t length, size_t from, size_t to, const char *owner, bool id,
                          check_mode mode, bool closing) {
    tree base = {0}, sub = {0}, other = {0}, edge = {0}, side_base = {0};
    changes diff;
    char tail[256], label[160];
    const char *why = NULL;
    const uint8_t *token = source + from;
    const size_t token_length = to - from;
    if (token_length > 64) {
        goto done;
    }
    appended(mode, SENTINEL, token, token_length, tail, sizeof(tail));
    if (!parse_edit(source, length, 0, 0, "", tail, &base) ||
        !parse_edit(source, length, from, to, SENTINEL, tail, &sub)) {
        why = ("the edited text does not parse");
        goto done;
    }
    bool owned = false;
    for (size_t i = 0; i < sub.count && !owned; i++) {
        owned = takes_sentinel(sub.lines[i], owner, id);
    }
    if (!owned) {
        /* A construct the line leaves open is checked as its closer would
         * leave it, written at the line's end. */
        size_t end = to;
        while (end < length && (source[end] == ' ' || source[end] == '\t')) {
            end++;
        }
        if (closing && (end == length || source[end] == '\n' || source[end] == '\r')) {
            for (const char *const *closer = CLOSERS; *closer; closer++) {
                size_t size;
                tree probe = {0};
                uint8_t *closed = splice(source, length, end, end, *closer, &size);
                bool takes = closed && parse_edit(closed, size, from, to, SENTINEL, tail, &probe);
                for (size_t i = 0; takes && i < probe.count; i++) {
                    if (takes_sentinel(probe.lines[i], owner, id)) {
                        break;
                    }
                    takes = i + 1 < probe.count;
                }
                tree_free(&probe);
                if (takes) {
                    why = verify(closed, size, from, to, owner, id, mode, false);
                    free(closed);
                    goto done;
                }
                free(closed);
            }
        }
        why = ("no node of the owner's kind holds the sentinel");
        goto kept;
    }
    if (from == to) {
        /* An empty field may be no construct before it is written: the
         * sentinels must read alike. */
        char other_tail[256];
        appended(mode, OTHER, token, token_length, other_tail, sizeof(other_tail));
        if (!parse_edit(source, length, from, to, OTHER, other_tail, &other)) {
            why = ("the edited text does not parse");
            goto done;
        }
        for (size_t i = 0; i < sub.count; i++) {
            for (char *c = sub.lines[i]; (c = strstr(c, SENTINEL)); c += SENTINEL_LENGTH) {
                memcpy(c, OTHER, SENTINEL_LENGTH);
            }
        }
        if (!differ(&sub, &other, &diff) || diff.count) {
            why = ("the tree depends on the field's text beyond its value");
        }
        goto done;
    }
    if (!differ(&base, &sub, &diff)) {
        why = (diff.why);
        goto kept;
    }
    /* The owner's node is the one that takes the sentinel. Another node
     * may change only where it names the owner's: a deduplicated anchor,
     * a heading's anchor made from its text, a citation's referent. */
    size_t own = SIZE_MAX;
    for (size_t i = 0; i < diff.count && own == SIZE_MAX; i++) {
        line_parts parts;
        split_line(sub.lines[diff.at[i].line], &parts);
        if (same_kind(&parts, owner, strlen(owner)) &&
            contains(diff.at[i].now.start, diff.at[i].now.length, SENTINEL)) {
            own = diff.at[i].line;
        }
    }
    if (own == SIZE_MAX) {
        why = ("no property of the owner's node takes the sentinel");
        goto done;
    }
    change mine[MAX_CHANGES];
    size_t count = 0, starts[MAX_CHANGES], ends[MAX_CHANGES];
    for (size_t i = 0; i < diff.count; i++) {
        if (diff.at[i].line == own) {
            mine[count++] = diff.at[i];
        } else if (!naming(&diff.at[i].was)) {
            why = ("another node changes");
            goto kept;
        }
    }
    for (size_t i = 0; i < count; i++) {
        if (mode != CHECK_RECORD && !naming(&mine[i].was) &&
            !replaced(&mine[i].was, &mine[i].now, &starts[i], &ends[i])) {
            why = ("a property changes other than by the field's value");
            goto done;
        }
        if (!strcmp(owner, "Metadata") && !whole(&mine[i].was, starts[i], ends[i])) {
            why = ("the field is part of a value");
            goto done;
        }
    }
    if (mode == CHECK_RECORD) {
        goto done;
    }
    const size_t places[2] = {from, to};
    for (int side = 0; side < 2; side++) {
        changes moved;
        tree_free(&edge);
        tree_free(&side_base);
        if (side) {
            snprintf(label, sizeof(label), "%.*s%s", (int)token_length, (const char *)token, SENTINEL);
        } else {
            snprintf(label, sizeof(label), "%s%.*s", SENTINEL, (int)token_length, (const char *)token);
        }
        appended(mode, label, token, token_length, tail, sizeof(tail));
        if (!parse_edit(source, length, 0, 0, "", tail, &side_base) ||
            !parse_edit(source, length, places[side], places[side], SENTINEL, tail, &edge)) {
            why = ("the edited text does not parse");
            goto done;
        }
        bool exact = differ(&side_base, &edge, &moved);
        size_t seen = 0;
        for (size_t i = 0; exact && i < moved.count; i++) {
            if (moved.at[i].line != own) {
                exact = naming(&moved.at[i].was);
                continue;
            }
            size_t k = 0;
            while (k < count && mine[k].index != moved.at[i].index) {
                k++;
            }
            exact = k < count && (naming(&mine[k].was) ||
                                  inserted_at(&moved.at[i].was, &moved.at[i].now, side ? ends[k] : starts[k]));
            seen++;
        }
        if (!exact || seen != count) {
            why = (side ? "the field's end is not its value's end" : "the field's start is not its value's start");
            goto done;
        }
    }
    goto done;
kept:
    /* Taking out a field that holds syntax may change the tree around it:
     * such a field is checked with its bytes kept. */
    if (syntactic(token, token_length)) {
        why = verify_kept(source, length, from, to, owner, mode);
    }
done:
    tree_free(&side_base);
    tree_free(&base);
    tree_free(&sub);
    tree_free(&other);
    tree_free(&edge);
    return why;
}

/* What the member of attribute word `w` that holds the byte at `at` is:
 * `.` a class, `=` a value, and 0 a key. */
static char member_at(const word *w, size_t at) {
    char kind = 0;
    bool quoted = false;
    for (size_t i = w->key + 2; i < at && i < w->length; i++) {
        const char c = w->start[i];
        if (quoted) {
            if (c == '\\') {
                i++;
            } else if (c == '"') {
                quoted = false;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == ' ') {
            kind = 0;
        } else if (c == '=' || (c == '.' && w->start[i - 1] == ' ') || (c == '.' && i == w->key + 2)) {
            kind = c == '=' ? '=' : (kind ? kind : '.');
        }
    }
    return kind;
}

/* THE MEMBER CHECK of a class, identifier or value slot: the sentinel
 * written at the field's start lands in a member of that kind of a node of
 * the owner's kind, a class in a class, a value in a value, in the classes
 * a `class=` sets or in the anchor an `id=` sets, an identifier in the
 * anchor; not in another member that holds the field's bytes as text. */
static const char *member_lands(const uint8_t *source, size_t length, size_t from, size_t to, const char *owner,
                                const char *kind, bool closing) {
    const bool class = !strcmp(kind, "class"), value = !strcmp(kind, "value"), id = !strcmp(kind, "id");
    /* A Metadata's member has one value, no attribute's members. */
    if ((!class && !value && !id) || !strcmp(owner, "Metadata")) {
        return NULL;
    }
    tree edited = {0};
    if (!parse_edit(source, length, from, from, SENTINEL, "", &edited)) {
        return "the edited text does not parse";
    }
    bool lands = false, seen = false;
    for (size_t i = 0; i < edited.count && !lands; i++) {
        line_parts parts;
        split_line(edited.lines[i], &parts);
        if (!same_kind(&parts, owner, strlen(owner))) {
            continue;
        }
        for (size_t k = 0; k < parts.count && !lands; k++) {
            const word *w = &parts.words[k];
            const char *at = seek(w->start, w->length, SENTINEL, SENTINEL_LENGTH);
            if (!at) {
                continue;
            }
            seen = true;
            if (w->key == 6 && !memcmp(w->start, "anchor", 6)) {
                lands = id || value;
            } else if (w->key == 10 && !memcmp(w->start, "attributes", 10)) {
                const char member = member_at(w, (size_t)(at - w->start));
                lands = member == '.' ? class || value : value && member == '=';
            }
        }
    }
    tree_free(&edited);
    if (!seen || lands) {
        return NULL;
    }
    /* A block the line leaves open is checked as its closer would leave
     * it, written at the line's end. */
    size_t end = to;
    while (closing && end < length && source[end] != '\n' && source[end] != '\r') {
        end++;
    }
    for (const char *const *closer = CLOSERS; closing && *closer; closer++) {
        size_t size;
        uint8_t *closed = splice(source, length, end, end, *closer, &size);
        const bool taken = closed && !member_lands(closed, size, from, to, owner, kind, false);
        free(closed);
        if (taken) {
            return NULL;
        }
    }
    return "the sentinel lands in another member";
}

/* The Metadata line of `t` split into parts, or false. */
static bool metadata_of(const tree *t, line_parts *parts, size_t *line) {
    for (size_t i = 0; i < t->count; i++) {
        split_line(t->lines[i], parts);
        if (same_kind(parts, "Metadata", 8)) {
            *line = i;
            return true;
        }
    }
    return false;
}

/* The value of the Metadata's field `name` in `parts`, or NULL. */
static const word *field_of(const line_parts *parts, const char *name) {
    for (size_t i = 0; i < parts->count; i++) {
        if (parts->words[i].key == strlen(name) && !memcmp(parts->words[i].start, name, parts->words[i].key)) {
            return &parts->words[i];
        }
    }
    return NULL;
}

/* Whether field word `w` is unset, `=null`. */
static bool unset(const word *w) { return w && w->length - w->key == 5 && !memcmp(w->start + w->key, "=null", 5); }

static bool same_word(const word *a, const word *b) {
    return a && b && a->length - a->key == b->length - b->key &&
           !memcmp(a->start + a->key, b->start + b->key, a->length - a->key);
}

/* THE NAME CHECK of a Metadata's `field` slot: the sentinel names no field,
 * so a name that sets no field, `with`, is written over the slot's source,
 * `[from, to)`, instead, which must move the member's value there: `with`
 * reads the value one field read, which reads null instead, or the value of
 * a later member that names it, the first is set, and nothing else changes.
 * A slot that holds less or more than the name names no field. */
static const char *verify_name(const uint8_t *source, size_t length, size_t from, size_t to, const char *with) {
    tree base = {0}, moved = {0};
    line_parts was, now;
    size_t line, other;
    const char *why = NULL;
    if (!parse_tree(source, length, &base) || !parse_spliced(source, length, from, to, with, &moved) ||
        !metadata_of(&base, &was, &line) || !metadata_of(&moved, &now, &other) || line != other ||
        base.count != moved.count || was.count != now.count) {
        why = "the edited text does not parse to a Metadata";
        goto done;
    }
    const word *old = NULL, *taken = field_of(&now, with);
    for (size_t i = 0; i < was.count && !why; i++) {
        const word *w = &was.words[i], *v = &now.words[i];
        if (same_word(w, v) || v == taken) {
            continue;
        }
        if (old || unset(w)) {
            why = "another field changes";
        }
        old = w;
    }
    if (!why && (!old || !same_word(old, taken))) {
        why = "another name does not move the value";
    }
    for (size_t i = 0; i < base.count && !why; i++) {
        if (i != line && strcmp(base.lines[i], moved.lines[i])) {
            why = "another node changes";
        }
    }
done:
    tree_free(&base);
    tree_free(&moved);
    return why;
}

/* Whether the Metadata of `source` leaves field `name` unset. */
static bool leaves(const uint8_t *source, size_t length, const char *name) {
    tree t = {0};
    line_parts parts;
    size_t line;
    const bool found =
        parse_tree(source, length, &t) && metadata_of(&t, &parts, &line) && unset(field_of(&parts, name));
    tree_free(&t);
    return found;
}

/* THE MEMBER CHECK of an open Metadata `field` slot: the slot names no
 * field yet, so a member written there, `with` and a value, begins a member
 * of its own: the Metadata reads the value in field `with`, which it left
 * unset, and every other field and node as it did. A slot whose line goes
 * on past it is judged by the name check alone. */
static const char *verify_member(const uint8_t *source, size_t length, size_t from, size_t to, const char *with) {
    char member[64];
    snprintf(member, sizeof(member), "%s: %s", with, SENTINEL);
    tree base = {0}, grown = {0};
    line_parts was, now;
    size_t line, other;
    const char *why = NULL;
    if (!parse_tree(source, length, &base) || !parse_spliced(source, length, from, to, member, &grown) ||
        !metadata_of(&base, &was, &line) || !metadata_of(&grown, &now, &other) || line != other ||
        base.count != grown.count || was.count != now.count) {
        why = "the edited text does not parse to a Metadata";
        goto done;
    }
    const word *taken = field_of(&now, with);
    if (!taken || unset(taken) || !seek(taken->start, taken->length, SENTINEL, strlen(SENTINEL))) {
        why = "the member written in the slot sets no field";
    }
    for (size_t i = 0; i < was.count && !why; i++) {
        if (&now.words[i] != taken && !same_word(&was.words[i], &now.words[i])) {
            why = "the member written in the slot changes another field";
        }
    }
    for (size_t i = 0; i < base.count && !why; i++) {
        if (i != line && strcmp(base.lines[i], grown.lines[i])) {
            why = "another node changes";
        }
    }
done:
    tree_free(&base);
    tree_free(&grown);
    return why;
}

/* The checks of one closed slot whose source is `[from, to)`. */
static void check_slot(report *r, const uint8_t *source, size_t length, size_t cursor, size_t from, size_t to,
                       const char *what, const char *kind, const char *owner, check_mode mode) {
    /* A field written in its own brackets or quotes holds its value
     * between them: a destination's `<...>`, a key's `{...}`, a quoted
     * attribute value. A cross link's anchor keeps the `^` that marks a
     * block identifier, which its value drops when the rest is one. A
     * Metadata's slot is inside its scalar's quotes already. */
    size_t starts[2] = {from, from}, ends[2] = {to, to};
    if (to - from >= 2 && strcmp(owner, "Metadata")) {
        const uint8_t first = source[from], last = source[to - 1];
        if ((!strcmp(kind, "dest") && first == '<' && last == '>') ||
            ((!strcmp(kind, "bib") || !strcmp(kind, "specimen")) && first == '{' && last == '}') ||
            (!strcmp(kind, "value") && (first == '"' || first == '\'') && last == first)) {
            starts[0] = starts[1] = from + 1;
            ends[0] = ends[1] = to - 1;
        } else if (!strcmp(kind, "value") && (first == '"' || first == '\'')) {
            starts[0] = starts[1] = from + 1;
        } else if (!strcmp(kind, "value") && (last == '"' || last == '\'')) {
            /* A quoted value that began on an earlier line: its slot keeps
             * to the cursor's line, up to the closing quote. */
            ends[0] = ends[1] = to - 1;
        } else if (!strcmp(kind, "anchor") && first == '^') {
            starts[1] = from + 1;
        }
    }
    r->checks++;
    if (!strcmp(owner, "Metadata") && !strcmp(kind, "field")) {
        /* A literal moves only to a prose field, which the Metadata may
         * set already: then no name is checked. */
        size_t after = to + (to < length && (source[to] == '"' || source[to] == '\''));
        while (after < length && (source[after] == ' ' || source[after] == '\t' || source[after] == ':')) {
            after++;
        }
        const bool literal = after < length && source[after] == '|';
        const char *why = NULL;
        for (size_t i = 0; i < NAME_COUNT; i++) {
            const bool prose = !strcmp(NAMES[i], "abstract") || !strcmp(NAMES[i], "comment");
            if ((prose || !literal) && leaves(source, length, NAMES[i]) &&
                !(why = verify_name(source, length, from, to, NAMES[i]))) {
                break;
            }
        }
        if (why) {
            fail_at(r, source, length, cursor, from, to, what, why);
        }
        return;
    }
    const bool id = !strcmp(kind, "id") || !strcmp(kind, "value");
    const char *why = verify(source, length, starts[0], ends[0], owner, id, mode, true);
    if (why && (starts[1] != starts[0] || ends[1] != ends[0]) &&
        !verify(source, length, starts[1], ends[1], owner, id, mode, true)) {
        why = NULL;
    }
    if (!why) {
        why = member_lands(source, length, starts[1], ends[1], owner, kind, true);
    }
    if (why) {
        fail_at(r, source, length, cursor, from, to, what, why);
    }
}

/* Whether `w`, a property of a node of kind `parts`, is a field a slot
 * completes. */
static bool completable(const line_parts *parts, const word *w) {
    for (size_t i = 0; i < sizeof(FIELDS) / sizeof(FIELDS[0]); i++) {
        if (strlen(FIELDS[i][1]) == w->key && !memcmp(w->start, FIELDS[i][1], w->key) &&
            (!*FIELDS[i][0] || same_kind(parts, FIELDS[i][0], strlen(FIELDS[i][0])))) {
            return true;
        }
    }
    return false;
}

/* Where the 1-based line `line` of `source` starts. */
static size_t line_offset(const uint8_t *source, size_t length, int32_t line) {
    size_t at = 0;
    for (int32_t n = 1; n < line && at < length; at++) {
        if (source[at] == '\n' || (source[at] == '\r' && (at + 1 >= length || source[at + 1] != '\n'))) {
            n++;
            if (n == line) {
                return at + 1;
            }
        }
    }
    return at;
}

/* The check of a position with no closed slot. */
static void check_gap(report *r, const uint8_t *source, size_t length, const tree *original, size_t cursor) {
    tree grown = {0};
    changes diff;
    r->checks++;
    if (!parse_spliced(source, length, cursor, cursor, SENTINEL, &grown)) {
        fail(r, source, length, cursor, "gap", "the edited text does not parse");
        return;
    }
    bool grows = differ(original, &grown, &diff) && diff.count;
    size_t field = SIZE_MAX;
    line_parts parts = {0};
    if (grows) {
        split_line(grown.lines[diff.at[0].line], &parts);
    }
    for (size_t i = 0; grows && i < diff.count; i++) {
        grows = diff.at[i].line == diff.at[0].line && inserted(&diff.at[i].was, &diff.at[i].now);
        if (field == SIZE_MAX && completable(&parts, &diff.at[i].was)) {
            field = i;
        }
    }
    if (grows && field != SIZE_MAX) {
        char what[96];
        snprintf(what, sizeof(what), "missed %.*s.%.*s", (int)parts.kind_length, parts.kind,
                 (int)diff.at[field].was.key, diff.at[field].was.start);
        fail(r, source, length, cursor, what, "the cursor writes this field and the query reports no slot");
    }
    tree_free(&grown);
}

static const char *kind_name(markdown_core_node_kind kind) {
    const char *name = "?";
    markdown_core_node_kind_name(kind, &name);
    return name;
}

/* THE SPELLING CHECK (plan 3.1, `spell`; plan 8, oracle in_parens trip). Each
 * value is spelled in the slot, the spell_mode written over the slot's source
 * and the text parsed; so is the sentinel, the witness. The two trees must
 * agree but for the value where the witness holds the sentinel: the value
 * lands in the field the sentinel does, whole, and nothing else changes. A
 * slot that the document leaves open is checked too, since the spell_mode
 * writes its closer. A value the slot refuses must be one its grammar
 * cannot hold (refusable). */
static bool spell_mode;

static const char *const VALUES[] = {
    "qz xq",        "qz|xq",     "qz]xq",
    "qz[xq",        "qz]]xq",    "qz)xq",
    "qz(xq",        "qz\\xq",    "qzxq\\",
    "qz#xq",        "qz}xq",     "qz{xq",
    "qz\"xq",       "qz'xq",     "qz<xq",
    "qz>xq",        "qz&amp;xq", "qz&xq",
    "qz*xq",        "qz`xq",     "qz=xq",
    "qz.xq",        "qz-xq",     "qz_xq",
    "qz@xq",        "qz^xq",     "qz:xq",
    "qz/xq",        "qz%xq",     "qz%%xq",
    "qz$xq",        "qz!xq",     "qz\xe4\xb8\xadxq",
    "qz\xc3\xa9xq", "qz\txq",    "",
    " qzxq",        "qzxq ",     "^qzxq",
    "#qzxq",        "qz##xq",    "<qzxq>",
    "qz\\|xq",      "qz~xq",     "qz+xq",
    "qz==xq",       "qz;xq",     "qz,xq",
    "qz?xq",        "qz\\]xq",   "{qzxq}",
    "qz{x}q",       "-qzxq",     "qzxq-",
    "qz--xq",       "qz&#35;xq", "qz\nxq",
    "qz:?xq",       "qz: xq",    "qzxq:",
    "qz #xq",       "qz 'xq",    "qz \"xq",
};
enum { VALUE_COUNT = sizeof(VALUES) / sizeof(VALUES[0]) };

/* Whether the source byte `c` is one of `set`: a NUL, which the parse
 * reads as U+FFFD, is none. */
static bool among(unsigned char c, const char *set) { return c && strchr(set, c); }

static bool holds(const char *value, const char *set) {
    for (const char *c = value; *c; c++) {
        if (strchr(set, *c)) {
            return true;
        }
    }
    return false;
}

static bool ascii_word(const char *value, const char *also) {
    for (const char *c = value; *c; c++) {
        const unsigned char u = (unsigned char)*c;
        if (!(u >= 0x80 || (u >= '0' && u <= '9') || ((u | 0x20) >= 'a' && (u | 0x20) <= 'z') || strchr(also, u))) {
            return false;
        }
    }
    return *value != 0;
}

/* Whether `value` holds a byte that opens a literal token: a code span,
 * math, an autolink or HTML, or a comment. A raw field cannot escape it,
 * and text after the slot may close it. */
static bool literal(const char *value) { return holds(value, "`$<>") || strstr(value, "%%"); }

/* Whether the grammar holds `value` in the slot in no spell_mode, by the
 * dialect's own words (docs/specs/dialect): a refusal of any other value
 * fails the check. `row` is whether the slot is in a table's cell, and
 * `wrap` the byte before the slot. */
/* Whether the line before `from` leaves an `open` no `close` matches. */
static bool opened(const uint8_t *source, size_t from, char open, char close) {
    size_t start = from, depth = 0;
    while (start > 0 && source[start - 1] != '\n') {
        start--;
    }
    for (size_t i = start; i < from; i++) {
        if (source[i] == '\\') {
            i++;
        } else if (source[i] == open) {
            depth++;
        } else if (source[i] == close && depth) {
            depth--;
        }
    }
    return depth > 0;
}

/* What the earlier lines of the paragraph leave open at the start of the
 * line that holds `from`: a directive's label, `:name[`, which any `]`
 * ends, and the quote of a value in braces. */
static void carried(const uint8_t *source, size_t from, bool *label, char *quote) {
    size_t line = from;
    while (line > 0 && source[line - 1] != '\n') {
        line--;
    }
    size_t start = line;
    while (start > 0) {
        size_t before = start - 1;
        while (before > 0 && source[before - 1] != '\n') {
            before--;
        }
        size_t at = before;
        while (at < start - 1 && (source[at] == ' ' || source[at] == '\t')) {
            at++;
        }
        if (at == start - 1) {
            break;
        }
        start = before;
    }
    size_t labels = 0, braces = 0;
    *quote = 0;
    for (size_t i = start; i < line; i++) {
        const uint8_t c = source[i];
        if (c == '\\') {
            i++;
        } else if (*quote) {
            *quote = c == (uint8_t)*quote ? 0 : *quote;
        } else if (braces && (c == '"' || c == '\'')) {
            *quote = (char)c;
        } else if (c == '{') {
            braces++;
        } else if (c == '}' && braces) {
            braces--;
        } else if (c == '[') {
            size_t name = i;
            while (name > start && !among(source[name - 1], " \t\r\n[{:")) {
                name--;
            }
            labels += name < i && name > start && source[name - 1] == ':';
        } else if (c == ']' && labels) {
            labels--;
        }
    }
    *label = labels > 0;
    *quote = braces ? *quote : 0;
}

/* Whether the value would end a construct the line opens before it: a
 * bracket or a parenthesis it does not match, or a brace or an `=`, which
 * a name's or a bare value's scan reads escaped or not; or one an earlier
 * line leaves open, a directive's label or a quoted value. */
static bool ends(const char *value, const uint8_t *source, size_t from) {
    bool label;
    char quote;
    carried(source, from, &label, &quote);
    const bool in_brackets = label || opened(source, from, '[', ']'), in_parens = opened(source, from, '(', ')'),
               in_braces = opened(source, from, '{', '}');
    long brackets = 0, parens = 0;
    for (const char *c = value; *c; c++) {
        if ((in_braces && (*c == '}' || *c == '=')) || (quote && *c == quote)) {
            return true;
        }
        if (*c == '\\' && c[1]) {
            c++;
            continue;
        }
        if ((*c == ']' && !brackets && in_brackets) || (*c == ')' && !parens && in_parens)) {
            return true;
        }
        brackets += *c == '[' ? 1 : *c == ']' ? -1 : 0;
        parens += *c == '(' ? 1 : *c == ')' ? -1 : 0;
    }
    return (brackets && in_brackets) || (parens && in_parens);
}

/* Whether a `:` before or in the value begins a directive whose name
 * runs on through it, and past the slot, to a label or braces. */
static bool directs(const char *value, const uint8_t *source, size_t length, size_t from, size_t to, bool own) {
    bool run = false;
    for (size_t i = own ? 0 : from; i > 0 && source[i - 1] != '\n'; i--) {
        if (among(source[i - 1], " \t[{:")) {
            run = source[i - 1] == ':';
            break;
        }
    }
    for (const char *c = value; *c; c++) {
        if ((*c == '[' || *c == '{') && run) {
            return true;
        }
        run = *c == ':' || (run && *c != ' ' && *c != '\t');
    }
    for (size_t i = to; i < length && run; i++) {
        if (among(source[i], " \t\r\n[{:")) {
            return source[i] == '[' || source[i] == '{';
        }
    }
    return false;
}

/* Whether a delimiter run or a citation's `@` begins at `at`. */
static bool delimits(const uint8_t *source, size_t length, size_t at) {
    return at < length &&
           (among(source[at], "*_~^@") || (source[at] == '=' && at + 1 < length && source[at + 1] == '='));
}

/* Whether the line before `from` leaves a cross link, or an autolink or a
 * tag, open. */
static bool crossed(const uint8_t *source, size_t from) {
    size_t start = from, cross = 0;
    while (start > 0 && source[start - 1] != '\n') {
        start--;
    }
    for (size_t i = start; i + 1 < from; i++) {
        if (source[i] == source[i + 1] && (source[i] == '[' || source[i] == ']')) {
            cross = source[i] == '[' ? cross + 1 : cross ? cross - 1 : 0;
            i++;
        }
    }
    return cross > 0;
}

static bool marked_up(const uint8_t *source, size_t from, unsigned char wrap) {
    size_t start = from;
    while (start > 0 && source[start - 1] != '\n') {
        start--;
    }
    bool in_markup = false;
    for (size_t i = start; i < from - (wrap == '<'); i++) {
        const unsigned char next = source[i + 1];
        if (source[i] == '<' && (strchr("/!?", next) || isalpha(next))) {
            in_markup = true;
        } else if (source[i] == '>') {
            in_markup = false;
        }
    }
    return in_markup;
}

static int flank(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || !c ? 0 : ispunct(c) ? 1 : 2; }

/* Whether `from` is on a line of a literal's text, after its indentation:
 * the member line before it, the last line indented less than `from`,
 * ends with the literal's `|`; `*first` whether no line of text comes
 * before it. */
static bool literal_line(const uint8_t *source, size_t from, bool *first) {
    size_t line = from;
    while (line > 0 && source[line - 1] != '\n') {
        line--;
    }
    for (size_t i = line; i < from; i++) {
        if (source[i] != ' ') {
            return false;
        }
    }
    const size_t column = from - line;
    *first = true;
    while (line > 0) {
        size_t start = line - 1;
        while (start > 0 && source[start - 1] != '\n') {
            start--;
        }
        size_t end = line - 1;
        while (end > start && (source[end - 1] == ' ' || source[end - 1] == '\t' || source[end - 1] == '\r')) {
            end--;
        }
        size_t indent = start;
        while (indent < end && source[indent] == ' ') {
            indent++;
        }
        if (indent < end && indent - start < column) {
            return source[end - 1] == '|';
        }
        *first = *first && end == start;
        line = start;
    }
    return false;
}

static bool refusable(const char *kind, const char *owner, const char *value, bool row, unsigned char wrap,
                      const uint8_t *source, size_t length, size_t from, size_t to) {
    if (strchr(value, '\n')) {
        return true;
    }
    if (!strcmp(owner, "Metadata")) {
        /* A literal's line holds text, and its first a value that adds no
         * indentation; a quoted or bare value holds any. */
        bool first;
        const bool blank = !value[strspn(value, " \t")];
        return !strcmp(kind, "value") && literal_line(source, from, &first) && (blank || (first && value[0] == ' '));
    }
    const size_t size = strlen(value);
    /* A code span's backtick, a formula's `$` or a comment's `%%` closes
     * one the text before opens, escaped or not. */
    if (holds(value, "`$") || strstr(value, "%%")) {
        return true;
    }
    /* A value that changes the byte class before a delimiter run or a
     * citation's `@` changes what the run or the `@` opens; a key in
     * braces or a quoted value ends in punctuation. */
    if (delimits(source, length, to)) {
        const unsigned char was = to > 0 ? source[to - 1] : 0;
        const unsigned char now = size ? (unsigned char)value[size - 1] : from > 0 ? source[from - 1] : 0;
        if (flank(was) != flank(now) || !strcmp(kind, "bib") || !strcmp(kind, "value")) {
            return true;
        }
    }
    if (strcmp(kind, "info") &&
        (ends(value, source, from) ||
         directs(value, source, length, from, to, !strcmp(kind, "name") && strcmp(owner, "Topic")))) {
        return true;
    }
    if (strcmp(kind, "info") && ((crossed(source, from) && strchr(value, '|')) ||
                                 (marked_up(source, from, wrap) && (!size || holds(value, " \t\"'<>"))))) {
        return true;
    }
    /* A key that needs braces writes a `}`. */
    if (!strcmp(kind, "bib") && opened(source, from, '{', '}') && !ascii_word(value, "")) {
        return true;
    }
    const bool cross = !strcmp(owner, "CrossLink") || !strcmp(owner, "CrossEmbedded");
    const bool brace = wrap == '{' && from >= 2 && source[from - 2] == '@';
    if (!strcmp(kind, "dest") && cross) {
        return !size || literal(value) || holds(value, "|[]#") || value[size - 1] == '\\';
    }
    if (!strcmp(kind, "anchor") && cross) {
        return !size || literal(value) || holds(value, "|[]") || value[0] == '#' || value[size - 1] == '#' ||
               strstr(value, "##") || value[0] == '^' || value[size - 1] == '\\';
    }
    if (!strcmp(kind, "dest") || !strcmp(kind, "anchor")) {
        /* A fragment after a bare path holds no space, nor does a
         * destination that goes on after the slot, since `<...>` is a whole
         * destination; a destination holds every other value. */
        const bool spaced = !size || holds(value, " \t") || value[0] == '<';
        const bool goes = to < length && !among(source[to], ") \t\r\n");
        return wrap != '<' && spaced && (!strcmp(kind, "anchor") || goes);
    }
    if (!strcmp(kind, "label") || !strcmp(kind, "footnote")) {
        bool odd = false, bracket = false;
        for (const char *c = value; *c; c++) {
            bracket = bracket || (!odd && (*c == '[' || *c == ']'));
            odd = *c == '\\' && !odd;
        }
        bool blank = true;
        for (const char *c = value; *c; c++) {
            blank = blank && (*c == ' ' || *c == '\t');
        }
        /* A footnote's label holds no bracket, even escaped: the
         * definition would be a link reference's. */
        return blank || bracket || odd || literal(value) || (row && strchr(value, '|')) ||
               (!strcmp(kind, "label") && value[0] == '^') || (!strcmp(kind, "footnote") && holds(value, " \t[]"));
    }
    if (!strcmp(kind, "bib") || !strcmp(kind, "specimen")) {
        int depth = 0;
        bool unbalanced = false;
        for (const char *c = value; *c; c++) {
            depth += *c == '{' ? 1 : *c == '}' ? -1 : 0;
            unbalanced = unbalanced || depth < 0;
        }
        if (!size || literal(value) || holds(value, " \t") || unbalanced || depth || (row && strchr(value, '|'))) {
            return true;
        }
        if (!strcmp(kind, "specimen")) {
            return !ascii_word(value, "-_") || value[0] == '-' || value[size - 1] == '-' || strstr(value, "--");
        }
        (void)brace;
        return false;
    }
    if (!strcmp(kind, "name") && !strcmp(owner, "Topic")) {
        return !ascii_word(value, "_-/") || value[0] == '/' || value[size - 1] == '/' || strstr(value, "//");
    }
    if (!strcmp(kind, "name")) {
        return !size || literal(value) || value[size - 1] == '\\' || holds(value, " \t[{:") ||
               (row && strchr(value, '|'));
    }
    if (!strcmp(kind, "id") && (!strcmp(owner, "Paragraph") || !strcmp(owner, "ListItem") || !strcmp(owner, "List") ||
                                !strcmp(owner, "Callout") || !strcmp(owner, "Table"))) {
        return !ascii_word(value, "-") || holds(value, "\xc3\xe4");
    }
    if (!strcmp(kind, "class") && !strcmp(owner, "DirectiveBlock") && holds(value, ":{")) {
        /* A nameless container's class word is a word. */
        return true;
    }
    if (!strcmp(kind, "id") || !strcmp(kind, "class") || !strcmp(kind, "record")) {
        /* A shorthand or a name keeps its bytes, so a final backslash
         * would escape the closer after it. */
        return !size || literal(value) || value[size - 1] == '\\' || holds(value, " \t=}\"'{") ||
               (row && strchr(value, '|')) ||
               (!strcmp(kind, "record") && (value[0] == '#' || value[0] == '.' || !strcmp(value, "-")));
    }
    if (!strcmp(kind, "value")) {
        /* A backtick fence's info string holds no backtick. */
        size_t start = from;
        while (start > 0 && source[start - 1] != '\n') {
            start--;
        }
        while (start < from && (source[start] == ' ' || source[start] == '>')) {
            start++;
        }
        return !strcmp(owner, "CodeBlock") && source[start] == '`' && strchr(value, '`');
    }
    if (!strcmp(kind, "variant")) {
        return !ascii_word(value, "_-") || holds(value, "\xc3\xe4");
    }
    if (!strcmp(kind, "info")) {
        /* The slot is the info string's first word. */
        return !size || holds(value, " \t`");
    }
    return true;
}

/* The value as the dump writes a string, between its quotes. */
static char *dumped(const char *value) {
    char *out = malloc(strlen(value) * 6 + 1), *at = out;
    if (!out) {
        return NULL;
    }
    for (const unsigned char *c = (const unsigned char *)value; *c; c++) {
        if (*c == '"' || *c == '\\') {
            *at++ = '\\';
            *at++ = (char)*c;
        } else if (*c == '\b' || *c == '\t' || *c == '\n' || *c == '\f' || *c == '\r') {
            *at++ = '\\';
            *at++ = *c == '\b' ? 'b' : *c == '\t' ? 't' : *c == '\n' ? 'n' : *c == '\f' ? 'f' : 'r';
        } else if (*c < 0x20) {
            at += sprintf(at, "\\u%04x", *c);
        } else {
            *at++ = (char)*c;
        }
    }
    *at = 0;
    return out;
}

/* `text` with every `from` replaced by `to`, in place of `text`. */
static char *swapped(char *text, const char *from, const char *to) {
    const size_t a = strlen(from), b = strlen(to);
    if (!a || !text) {
        return text;
    }
    size_t count = 0;
    for (const char *at = text; (at = strstr(at, from)); at += a) {
        count++;
    }
    if (!count) {
        return text;
    }
    char *out = malloc(strlen(text) + count * (b > a ? b - a : 0) + 1), *put = out;
    if (!out) {
        free(text);
        return NULL;
    }
    const char *at = text, *hit;
    while ((hit = strstr(at, from))) {
        memcpy(put, at, (size_t)(hit - at));
        put += hit - at;
        memcpy(put, to, b);
        put += b;
        at = hit + a;
    }
    strcpy(put, at);
    free(text);
    return out;
}

/* Whether the dump line holding offset `at` is a node of kind `owner`. */
static bool line_of(const uint8_t *dump, size_t size, size_t at, const char *owner) {
    size_t start = at;
    while (start > 0 && dump[start - 1] != '\n') {
        start--;
    }
    size_t end = start;
    while (end < size && dump[end] != '\n') {
        end++;
    }
    char *line = malloc(end - start + 1);
    if (!line) {
        return false;
    }
    memcpy(line, dump + start, end - start);
    line[end - start] = 0;
    line_parts parts;
    split_line(line, &parts);
    const bool same = same_kind(&parts, owner, strlen(owner));
    free(line);
    return same;
}

/* The dump of `source` with its scopes, an info string's derived language
 * when `info`, and the anchor of every node not of kind `owner` removed:
 * a heading's anchor is made from its text, which a field in it is part
 * of. NULL when it does not parse. */
static char *plain_dump(const uint8_t *source, size_t length, bool info, const char *owner) {
    markdown_core_document *document = NULL;
    if (markdown_core_document_parse(source, length, &document) != MARKDOWN_CORE_OK) {
        return NULL;
    }
    uint8_t *dump = NULL;
    size_t size = 0;
    const markdown_core_status status =
        markdown_core_document_dump(document, markdown_core_document_root(document), source, length, &dump, &size);
    markdown_core_document_free(document);
    if (status != MARKDOWN_CORE_OK) {
        return NULL;
    }
    char *out = malloc(size + 1), *put = out;
    if (!out) {
        markdown_core_dump_free(dump);
        return NULL;
    }
    for (size_t i = 0; i < size;) {
        const char *rest = (const char *)dump + i;
        const bool scope = size - i > 7 && !memcmp(rest, " scope=", 7);
        const bool language = info && size - i > 10 && !memcmp(rest, " language=", 10);
        const bool anchor = size - i > 8 && !memcmp(rest, " anchor=", 8) && !line_of(dump, size, i, owner);
        if (scope || language || anchor) {
            i += scope ? 7 : language ? 10 : 8;
            bool quoted = false;
            for (; i < size && (quoted || (dump[i] != ' ' && dump[i] != '\n')); i++) {
                if (quoted && dump[i] == '\\') {
                    i++;
                } else if (dump[i] == '"') {
                    quoted = !quoted;
                }
            }
            continue;
        }
        *put++ = (char)dump[i++];
    }
    *put = 0;
    markdown_core_dump_free(dump);
    return out;
}

/* A label as the definitions match it: trimmed, its white space runs one
 * space, ASCII letters lowercase. */
static char *normalized(const char *value) {
    char *out = malloc(strlen(value) + 1), *put = out;
    if (!out) {
        return NULL;
    }
    bool space = false;
    for (const char *c = value; *c; c++) {
        if (*c == ' ' || *c == '\t') {
            space = put != out;
            continue;
        }
        if (space) {
            *put++ = ' ';
            space = false;
        }
        *put++ = (char)((*c >= 'A' && *c <= 'Z') ? *c + 32 : *c);
    }
    *put = 0;
    return out;
}

/* The source with the slot's `[from, to)` replaced by `text`, `later` at
 * the end of its line, and the definition a reference needs to resolve,
 * spelled `label`, appended. */
static uint8_t *spelled_source(const uint8_t *source, size_t length, size_t from, size_t to, const char *text,
                               size_t text_size, const char *later, const char *kind, const char *owner,
                               const char *label, size_t label_size, size_t *out_size) {
    char tail[2048] = "";
    const bool call = !strcmp(owner, "Link") || !strcmp(owner, "Embedded");
    if (!strcmp(kind, "label") && call) {
        snprintf(tail, sizeof(tail), "\n\n[%.*s]: /o\n", (int)label_size, label);
    } else if (!strcmp(kind, "footnote") && !strcmp(owner, "Citation")) {
        snprintf(tail, sizeof(tail), "\n\n[^%.*s]: o\n", (int)label_size, label);
    } else if (!strcmp(kind, "specimen") && !strcmp(owner, "Citation")) {
        snprintf(tail, sizeof(tail), "\n\n(@%.*s) o\n", (int)label_size, label);
    }
    size_t end = to;
    while (end < length && source[end] != '\n' && source[end] != '\r') {
        end++;
    }
    const size_t extra = strlen(tail), more = strlen(later);
    *out_size = length - (to - from) + text_size + more + extra;
    uint8_t *out = malloc(*out_size + 1), *put = out;
    if (!out) {
        return NULL;
    }
    memcpy(put, source, from);
    put += from;
    memcpy(put, text, text_size);
    put += text_size;
    memcpy(put, source + to, end - to);
    put += end - to;
    memcpy(put, later, more);
    put += more;
    memcpy(put, source + end, length - end);
    put += length - end;
    memcpy(put, tail, extra);
    out[*out_size] = 0;
    return out;
}

/* The tree of the slot's source spelled `made`, with a definition for its
 * label; NULL when it does not parse. */
static char *spelled_tree(const uint8_t *source, size_t length, size_t from, size_t to,
                          const markdown_core_insertion *made, const char *later, const char *kind, const char *owner) {
    size_t size = 0;
    uint8_t *text = spelled_source(source, length, from, to, (const char *)made->text, made->size, later, kind, owner,
                                   (const char *)made->text, made->cursor, &size);
    char *tree = text ? plain_dump(text, size, !strcmp(kind, "info"), owner) : NULL;
    if (tree && getenv("ORACLE_VERBOSE")) {
        fprintf(stderr, "  spelled: %s\n", (const char *)text);
    }
    free(text);
    return tree;
}

/* The offset of a dump line's node name, past the tree's drawing. */
static size_t indent(const char *line) {
    size_t at = 0;
    while (line[at] && !isalpha((unsigned char)line[at])) {
        at++;
    }
    return at;
}

/* `tree` without the children of each node whose line holds `needle`. */
static void pruned(char *tree, const char *needle) {
    if (!tree) {
        return;
    }
    char *out = tree;
    const char *line = tree;
    size_t depth = SIZE_MAX;
    while (*line) {
        const char *next = strchr(line, '\n');
        next = next ? next + 1 : line + strlen(line);
        const size_t at = indent(line);
        if (depth != SIZE_MAX && at <= depth) {
            depth = SIZE_MAX;
        }
        if (depth == SIZE_MAX) {
            size_t size = (size_t)(next - line);
            const char *found = seek(line, size, needle, strlen(needle));
            memmove(out, line, size);
            if (found) {
                /* Its count of children goes with them. */
                depth = at;
                char *count = (char *)seek(out, size, " children=", 10);
                if (count) {
                    size_t width = 10;
                    while (isdigit((unsigned char)count[width])) {
                        width++;
                    }
                    memmove(count, count + width, size - (size_t)(count - out) - width);
                    size -= width;
                }
            }
            out += size;
        }
        line = next;
    }
    *out = 0;
}

/* Whether a line of `tree` is a node of kind `owner` with the sentinel in a
 * field the slot writes. */
static bool lands_in(const char *tree, const char *owner, bool id) {
    char *lines = copy(tree);
    bool lands = false;
    for (char *line = lines; line && *line && !lands;) {
        char *end = strchr(line, '\n');
        if (end) {
            *end = 0;
        }
        lands = takes_sentinel(line, owner, id);
        line = end ? end + 1 : NULL;
    }
    free(lines);
    return lands;
}

/* Whether the slot holds no value at all: a label that follows `@{`, which
 * no braces write; a key or a topic's segment that the text after it goes
 * on, so that no value is read whole, as a value the text after it
 * continues; or a value that goes on to another
 * line, which a spell_mode on the cursor's line cannot replace. A quote that
 * the line does not pair tells the last. */
static bool unheld(const char *kind, const char *owner, const uint8_t *source, size_t length, size_t from, size_t to,
                   bool closed) {
    const unsigned char next = to < length ? source[to] : 0;
    /* A delimiter run after the slot that the sentinel's letters would
     * keep from opening, or a destination that goes on after it. */
    if (delimits(source, length, to) && flank(to > 0 ? source[to - 1] : 0) != flank('q')) {
        return true;
    }
    if ((!strcmp(kind, "dest") || !strcmp(kind, "anchor")) && strcmp(owner, "CrossLink") &&
        strcmp(owner, "CrossEmbedded") && next && !strchr(") \t\r\n", next)) {
        return true;
    }
    /* A label that goes on to the next line, or a slot whose `|` keeps
     * its line from a table's row. */
    size_t end = to;
    while (end < length && source[end] != '\n') {
        end++;
    }
    size_t start = from;
    while (start > 0 && source[start - 1] != '\n') {
        start--;
    }
    if ((!strcmp(kind, "label") || !strcmp(kind, "footnote")) &&
        ((closed && !memchr(source + to, ']', end - to)) || !memchr(source + start, '[', from - start))) {
        return true;
    }
    if (memchr(source + from, '|', to - from)) {
        return true;
    }
    /* Text that reads as a tag but for the quotes in the slot reads as
     * one once a value replaces them. */
    for (size_t i = from; i < to && marked_up(source, from, 0); i++) {
        if (source[i] == '"' || source[i] == '\'') {
            return true;
        }
    }
    if (!strcmp(kind, "specimen")) {
        return (from >= 2 && source[from - 1] == '{' && source[from - 2] == '@') ||
               (next && (strchr(".:-_/", next) || isalnum(next)));
    }
    if (!strcmp(kind, "name") && !strcmp(owner, "Topic")) {
        return next == '/';
    }
    if (!strcmp(kind, "value") && next && !strchr(" \t}\r\n", next)) {
        return true;
    }
    /* A value that begins its line began on an earlier one, in quotes the
     * line does not open: no insertion on the line replaces it whole. */
    if (!strcmp(kind, "value") && from == start) {
        return true;
    }
    if (!strcmp(kind, "value")) {
        size_t quotes = 0;
        for (size_t i = start; i < end; i++) {
            quotes += source[i] == '"' && (i == start || source[i - 1] != '\\');
        }
        return quotes % 2 == 1;
    }
    return false;
}

/* Whether the line at `at` is a grid table's border or a simple table's
 * rule: lines whose columns their bytes' positions set. */
static bool ruled(const uint8_t *source, size_t length, size_t at) {
    size_t dashes = 0, i = at;
    for (; i < length && source[i] != '\n'; i++) {
        if (source[i] == '-' || source[i] == '=') {
            dashes++;
        } else if (!among(source[i], " :+|")) {
            return false;
        }
    }
    return dashes >= 3 && (source[at] == '+' || !memchr(source + at, '|', i - at));
}

/* Whether the cursor's line is a row of a table its layout sets: a value
 * of another size moves its columns. */
static bool laid(const uint8_t *source, size_t length, size_t from) {
    size_t start = from, end = from;
    while (start > 0 && source[start - 1] != '\n') {
        start--;
    }
    while (end < length && source[end] != '\n') {
        end++;
    }
    size_t before = start;
    if (before > 0) {
        before--;
        while (before > 0 && source[before - 1] != '\n') {
            before--;
        }
    }
    return (start > 0 && ruled(source, length, before)) || (end + 1 < length && ruled(source, length, end + 1));
}

static void check_spell(report *r, const uint8_t *source, size_t length, size_t cursor, size_t from, size_t to,
                        const markdown_core_context *context, size_t index, const markdown_core_slot *slot, bool row,
                        const char *what, const char *kind, const char *owner) {
    markdown_core_insertion *witness = NULL;
    const bool laid_out = laid(source, length, from);
    if (laid_out && to - from != SENTINEL_LENGTH) {
        return;
    }
    /* A `|` in the slot may keep its line from a table's row, and a quote
     * in it may end a title or a value the line opens before it: the
     * slot's text is what the reading around it stands on. */
    size_t quotes = 0, start = from;
    while (start > 0 && source[start - 1] != '\n') {
        start--;
    }
    for (size_t i = start; i < from; i++) {
        quotes += source[i] == '"';
    }
    if (memchr(source + from, '|', to - from) || (quotes % 2 && memchr(source + from, '"', to - from))) {
        return;
    }
    r->checks++;
    if (markdown_core_slot_spell(context, index, (const uint8_t *)SENTINEL, SENTINEL_LENGTH, &witness) !=
            MARKDOWN_CORE_OK ||
        !witness) {
        if (!witness && unheld(kind, owner, source, length, from, to, slot->closed)) {
            return;
        }
        fail_at(r, source, length, cursor, from, to, what, "the slot refuses the sentinel");
        markdown_core_insertion_free(witness);
        return;
    }
    /* A construct whose field goes on after the slot, as a path before its
     * fragment, closes after the field: the user writes that closer. */
    static const char *const LATER[] = {"", ">)", ">", ")", "]]", "]", "}", NULL};
    const bool id = !strcmp(kind, "id") || !strcmp(kind, "value");
    /* A block's slot reads as closed while its attribute tail or variant
     * is not (step 2d tells them apart), so its closer may be written. */
    const bool block = !strcmp(owner, "Heading") || !strcmp(owner, "CodeBlock") || !strcmp(owner, "DirectiveBlock") ||
                       !strcmp(owner, "Callout");
    const bool closed = slot->closed && !block;
    const char *later = "";
    char *expected = NULL;
    for (size_t i = 0; LATER[i] && (i == 0 || !closed); i++) {
        free(expected);
        later = LATER[i];
        expected = spelled_tree(source, length, from, to, witness, later, kind, owner);
        if (expected && lands_in(expected, owner, id)) {
            break;
        }
    }
    markdown_core_insertion_free(witness);
    /* An open construct the text after the slot keeps from holding, with
     * any closer, is text whatever the slot holds: no value is judged. */
    /* So is a slot whose text keeps a construct around it from holding,
     * as a space or a quote in a destination it encloses, which any value
     * in its place lets hold. */
    bool breaks = false;
    for (size_t i = from; i < to && !breaks; i++) {
        breaks = strchr(" \t\"'(){}[]\\|", source[i]) != NULL;
    }
    if (expected && !lands_in(expected, owner, id) && (!slot->closed || breaks)) {
        free(expected);
        return;
    }
    if (expected && breaks) {
        /* Where the witness lands but the nodes around it change, the
         * slot's text kept them from it. */
        char *was = plain_dump(source, length, !strcmp(kind, "info"), owner);
        size_t lines[2] = {0, 0};
        for (const char *c = was ? was : ""; *c; c++) {
            lines[0] += *c == '\n';
        }
        for (const char *c = expected; *c; c++) {
            lines[1] += *c == '\n';
        }
        free(was);
        if (lines[0] != lines[1]) {
            free(expected);
            return;
        }
    }
    if (!expected || !lands_in(expected, owner, id)) {
        fail_at(r, source, length, cursor, from, to, what,
                expected ? "the spelled sentinel lands in no field of the owner" : "the spelled text does not parse");
        free(expected);
        return;
    }
    const unsigned char wrap = from > 0 ? source[from - 1] : 0;
    /* An `id` or `class` value is the node's identifier or classes, which
     * drop an empty one and split one at its spaces: those are not
     * judged. */
    size_t key = from;
    key -= key > 0 && (source[key - 1] == '"' || source[key - 1] == '\'');
    const bool special =
        !strcmp(kind, "value") &&
        ((key >= 4 && !memcmp(source + key - 4, " id=", 4)) || (key >= 4 && !memcmp(source + key - 4, "{id=", 4)) ||
         (key >= 7 && (!memcmp(source + key - 7, " class=", 7) || !memcmp(source + key - 7, "{class=", 7))));
    for (size_t v = 0; v < VALUE_COUNT; v++) {
        const char *value = VALUES[v];
        if ((special && (!*value || holds(value, " \t"))) || (laid_out && strlen(value) != SENTINEL_LENGTH)) {
            continue;
        }
        markdown_core_insertion *made = NULL;
        r->checks++;
        if (markdown_core_slot_spell(context, index, (const uint8_t *)value, strlen(value), &made) !=
            MARKDOWN_CORE_OK) {
            fail_at(r, source, length, cursor, from, to, what, "spell fails");
            continue;
        }
        char why[256];
        char *desc = dumped(value);
        if (!made || strchr(value, '\n')) {
            if (made) {
                snprintf(why, sizeof(why), "spells \"%s\", which no field holds", desc);
                fail_at(r, source, length, cursor, from, to, what, why);
            } else if (!refusable(kind, owner, value, row, wrap, source, length, from, to)) {
                snprintf(why, sizeof(why), "refuses \"%s\"", desc);
                fail_at(r, source, length, cursor, from, to, what, why);
            }
            free(desc);
            markdown_core_insertion_free(made);
            continue;
        }
        /* The caret stands after the value, before the closer. */
        bool placed = made->cursor <= made->size && (!closed || made->cursor == made->size);
        for (size_t i = made->cursor; placed && i < made->size; i++) {
            placed = strchr("[](){}>#\"'", made->text[i]) != NULL;
        }
        char *result = spelled_tree(source, length, from, to, made, later, kind, owner);
        /* The value's forms in the dump, each the sentinel in the witness:
         * a quoted string, a label as definitions match it, the same inside
         * a longer string, as a destination before its fragment, and the
         * value bare, as a class or a record's name. Each is marked first,
         * so that no replacement reads another's. */
        char *norm = normalized(value), *form = norm ? dumped(norm) : NULL;
        char quoted[2][1024];
        snprintf(quoted[0], sizeof(quoted[0]), "\"%s\"", desc);
        snprintf(quoted[1], sizeof(quoted[1]), "\"%s\"", form ? form : "");
        const char *forms[] = {quoted[0], quoted[1], desc, form, value};
        const char *marks[] = {"\"\x01\"", "\"\x01\"", "\x01", "\x01", "\x01"};
        for (size_t f = 0; f < 5 && result && *value; f++) {
            if (forms[f] && *forms[f]) {
                result = swapped(result, forms[f], marks[f]);
            }
        }
        result = swapped(result, ".\"\x01\"", ".\x01");
        result = swapped(result, "\x01", "qzxq");
        /* The empty value is checked the other way: the witness with its
         * sentinel taken out. */
        char *want = *value ? copy(expected) : swapped(copy(expected), "qzxq", "");
        if (!*value) {
            free(result);
            result = spelled_tree(source, length, from, to, made, later, kind, owner);
        }
        if (!strcmp(kind, "label")) {
            /* A label that is also the link's text reads as inlines
             * there, which the label does not write. */
            pruned(result, "reference(\"qzxq\")");
            pruned(want, "reference(\"qzxq\")");
        }
        const bool same = result && want && !strcmp(result, want);
        free(want);
        if (!placed || !same) {
            snprintf(why, sizeof(why), "%s \"%s\" as \"%.*s\"", placed ? "misspells" : "misplaces the caret in", desc,
                     (int)made->size, (const char *)made->text);
            fail_at(r, source, length, cursor, from, to, what, why);
            if (getenv("ORACLE_VERBOSE") && result) {
                ts_print_line_diff(stderr, expected, result);
            }
        }
        free(norm);
        free(form);
        free(desc);
        free(result);
        markdown_core_insertion_free(made);
    }
    free(expected);
}

/* THE NAME SPELLING CHECK of a Metadata's `field` slot: a member's name is
 * one of the grammar's field names, so a name is written as it is, ASCII
 * letters, numerals, `_` and `-` after the first, and any other value is
 * refused. The name check (verify_name) judges where a name lands, and
 * the member check (verify_member) where a member written in an open slot
 * does. */
static void check_name_spell(report *r, const uint8_t *source, size_t length, size_t cursor, size_t from, size_t to,
                             const markdown_core_context *context, size_t index, bool closed, const char *what) {
    size_t rest = to;
    while (rest < length && (source[rest] == ' ' || source[rest] == '\t')) {
        rest++;
    }
    if (!closed && (rest == length || source[rest] == '\n' || source[rest] == '\r')) {
        const char *why = NULL;
        r->checks++;
        for (size_t i = 0; i < NAME_COUNT; i++) {
            if (leaves(source, length, NAMES[i]) && !(why = verify_member(source, length, from, to, NAMES[i]))) {
                break;
            }
        }
        if (why) {
            fail_at(r, source, length, cursor, from, to, what, why);
        }
    }
    for (size_t v = 0; v < VALUE_COUNT + NAME_COUNT; v++) {
        const char *value = v < VALUE_COUNT ? VALUES[v] : NAMES[v - VALUE_COUNT];
        const size_t size = strlen(value);
        bool name = size && value[0] != '-';
        for (size_t i = 0; i < size && name; i++) {
            name = isalnum((unsigned char)value[i]) || value[i] == '_' || value[i] == '-';
        }
        markdown_core_insertion *made = NULL;
        char why[256];
        r->checks++;
        if (markdown_core_slot_spell(context, index, (const uint8_t *)value, size, &made) != MARKDOWN_CORE_OK) {
            fail_at(r, source, length, cursor, from, to, what, "spell fails");
            continue;
        }
        char *desc = dumped(value);
        if (!name && made) {
            snprintf(why, sizeof(why), "spells \"%s\", which no name holds", desc);
            fail_at(r, source, length, cursor, from, to, what, why);
        } else if (name && (!made || made->size != size || made->cursor != size || memcmp(made->text, value, size))) {
            snprintf(why, sizeof(why), "%s \"%s\"", made ? "misspells" : "refuses", desc);
            fail_at(r, source, length, cursor, from, to, what, why);
        }
        free(desc);
        markdown_core_insertion_free(made);
    }
}

/* Whether `position` is in a table's cell, whose `|` a field cannot hold
 * bare. */
static bool in_row(const markdown_core_document *document, const uint8_t *source, size_t length,
                   markdown_core_position position) {
    const markdown_core_node **nodes = NULL;
    size_t count = 0;
    bool row = false;
    if (position.column > 1) {
        position.column--;
    }
    if (markdown_core_document_path(document, position, source, length, &nodes, &count) == MARKDOWN_CORE_OK) {
        for (size_t i = 0; i < count && !row; i++) {
            row = markdown_core_node_get_kind(nodes[i]) == MARKDOWN_CORE_KIND_TABLE_CELL;
        }
        markdown_core_path_free(nodes);
    }
    return row;
}

/* THE CUES (markdown_core_slot_cues): the bytes before a slot that begins
 * at the cursor, each of which must be a cue, and every cue must be met. A
 * quote's `>` before a field's later line is a container's prefix, not an
 * opener. */
static bool cue_met[256];

static void check_cue(report *r, const uint8_t *source, size_t length, size_t line_start, size_t at, size_t from,
                      const char *what) {
    if (from != at || !at) {
        return;
    }
    const unsigned char c = source[at - 1];
    size_t prefix = line_start;
    while (prefix < at && (source[prefix] == '>' || source[prefix] == ' ' || source[prefix] == '\t')) {
        prefix++;
    }
    if (c == '>' && prefix == at) {
        return;
    }
    const markdown_core_string cues = markdown_core_slot_cues();
    r->checks++;
    if (!memchr(cues.data, c, cues.length)) {
        char why[64];
        snprintf(why, sizeof(why), "opens after 0x%02x, which is not a cue", c);
        fail(r, source, length, at, what, why);
    }
    cue_met[c] = true;
}

/* The checks at every position of `source`. */
static void check_example(report *r, const uint8_t *source, size_t length) {
    tree original;
    markdown_core_document *document = NULL;
    if (!parse_tree(source, length, &original) ||
        markdown_core_document_parse(source, length, &document) != MARKDOWN_CORE_OK) {
        fail(r, source, length, 0, "parse", "the example does not parse");
        tree_free(&original);
        return;
    }
    /* A slot the context reports at several positions is checked once. */
    size_t seen[64][4], seen_count = 0;
    size_t line_start = 0;
    markdown_core_position position = {1, 1};
    for (size_t at = 0; at <= length; at++) {
        if (at < length && (source[at] & 0xc0) == 0x80) {
            continue;
        }
        position.column = (int32_t)(at - line_start) + 1;
        markdown_core_context *context = NULL;
        if (markdown_core_document_context(document, source, length, position, &context) != MARKDOWN_CORE_OK) {
            fail(r, source, length, at, "query", "the context query fails");
            break;
        }
        bool closed = false;
        for (size_t i = 0; i < markdown_core_context_slot_count(context); i++) {
            markdown_core_slot slot;
            const char *name = "?";
            if (markdown_core_context_slot_at(context, i, &slot) != MARKDOWN_CORE_OK) {
                continue;
            }
            const size_t from =
                line_offset(source, length, slot.before.start.line) + (size_t)(slot.before.start.column - 1);
            markdown_core_slot_kind_name(slot.kind, &name);
            if (!spell_mode) {
                check_cue(r, source, length, line_start, at, from, name);
            }
            if (!slot.closed && !spell_mode) {
                continue;
            }
            closed = closed || slot.closed;
            const size_t to = line_offset(source, length, slot.after.end.line) + (size_t)slot.after.end.column;
            bool known = false;
            for (size_t k = 0; k < seen_count && !known; k++) {
                known = seen[k][0] == from && seen[k][1] == to && seen[k][2] == slot.kind && seen[k][3] == slot.owner;
            }
            if (known) {
                continue;
            }
            if (seen_count < 64) {
                seen[seen_count][0] = from;
                seen[seen_count][1] = to;
                seen[seen_count][2] = slot.kind;
                seen[seen_count++][3] = slot.owner;
            }
            char what[96];
            snprintf(what, sizeof(what), "%s %s %s", spell_mode ? "spell" : "slot", name, kind_name(slot.owner));
            if (spell_mode && slot.owner == MARKDOWN_CORE_KIND_METADATA && slot.kind == MARKDOWN_CORE_SLOT_FIELD) {
                check_name_spell(r, source, length, at, from, to, context, i, slot.closed, what);
                continue;
            }
            if (spell_mode) {
                check_spell(r, source, length, at, from, to, context, i, &slot,
                            in_row(document, source, length, position), what, name, kind_name(slot.owner));
                continue;
            }
            const bool call = slot.owner == MARKDOWN_CORE_KIND_LINK || slot.owner == MARKDOWN_CORE_KIND_EMBEDDED;
            const check_mode mode = !strcmp(name, "record")          ? CHECK_RECORD
                                    : !strcmp(name, "label") && call ? CHECK_CALL
                                    : !strcmp(name, "label")         ? CHECK_DEFINITION
                                    : !strcmp(name, "footnote") && slot.owner == MARKDOWN_CORE_KIND_CITATION
                                        ? CHECK_NOTE_CALL
                                    : !strcmp(name, "footnote") ? CHECK_NOTE_DEFINITION
                                                                : CHECK_PLAIN;
            check_slot(r, source, length, at, from, to, what, name, kind_name(slot.owner), mode);
        }
        markdown_core_context_free(context);
        if (!closed && !spell_mode) {
            check_gap(r, source, length, &original, at);
        }
        if (at < length &&
            (source[at] == '\n' || (source[at] == '\r' && (at + 1 == length || source[at + 1] != '\n')))) {
            position.line++;
            line_start = at + 1;
        }
    }
    markdown_core_document_free(document);
    tree_free(&original);
}

/* Checks one generated input, named `generated` and numbered as
 * samples_each numbers it. */
static void check_one(void *context, const char *source, size_t length, int number) {
    report *r = context;
    ts_spec_case *example = (ts_spec_case *)r->example;
    example->start_line = number;
    r->generated++;
    check_example(r, (const uint8_t *)source, length);
}

static bool check_generated(report *r, size_t *examples) {
    ts_spec_case example = {0};
    r->path = "generated";
    r->example = &example;
    r->generated = 0;
    const bool ran = samples_each(check_one, r);
    *examples += r->generated;
    return ran;
}

int main(int argc, char **argv) {
    report r = {0};
    int first = 1;
    if (argc > 2 && !strcmp(argv[1], "--exceptions")) {
        if (!read_exceptions(&r, argv[2])) {
            fprintf(stderr, "%s: cannot read exceptions\n", argv[2]);
            return 1;
        }
        first = 3;
    }
    spell_mode = argc > first && !strcmp(argv[first], "--spell");
    first += spell_mode;
    const bool generate = argc > first && !strcmp(argv[first], "--generate");
    first += generate;
    if (argc <= first && !generate) {
        fputs("usage: oracle_runner [--exceptions FILE] [--spell] [--generate] [FILE...]\n", stderr);
        return 2;
    }
    size_t examples = 0;
    if (generate && !check_generated(&r, &examples)) {
        return 1;
    }
    for (int i = first; i < argc; i++) {
        ts_spec_file spec;
        if (ts_spec_load(argv[i], &spec) != 0) {
            fprintf(stderr, "%s: cannot load\n", argv[i]);
            return 1;
        }
        r.path = argv[i];
        for (size_t j = 0; j < spec.count; j++) {
            const ts_spec_case *example = &spec.cases[j];
            r.example = example;
            /* Drop the context fixture's cursor mark. */
            uint8_t *source = malloc(example->markdown_length + 1);
            if (!source) {
                return 1;
            }
            size_t length = 0;
            for (size_t k = 0; k < example->markdown_length;) {
                if (k + 3 <= example->markdown_length && !memcmp(example->markdown + k, CURSOR, 3)) {
                    k += 3;
                    continue;
                }
                source[length++] = (uint8_t)example->markdown[k++];
            }
            source[length] = 0;
            check_example(&r, source, length);
            free(source);
            examples++;
        }
        ts_spec_free(&spec);
    }
    /* Only the whole corpus, which the generated inputs are part of, meets
     * every cue. */
    const markdown_core_string cues = markdown_core_slot_cues();
    for (size_t i = 0; generate && !spell_mode && i < cues.length; i++) {
        if (!cue_met[cues.data[i]]) {
            fprintf(stderr, "cue 0x%02x: no slot opens after it\n", cues.data[i]);
            r.failures++;
        }
    }
    size_t stale = 0;
    for (size_t i = 0; i < r.exception_count; i++) {
        if (!r.exceptions[i].met) {
            fprintf(stderr, "%s:%d: %s: stale exception, no failure meets it\n", r.exceptions[i].file,
                    r.exceptions[i].line, r.exceptions[i].what);
            stale++;
        }
        free(r.exceptions[i].file);
        free(r.exceptions[i].what);
    }
    free(r.exceptions);
    printf("%zu examples, %zu checks, %zu failures, %zu excepted, %zu stale exceptions\n", examples, r.checks,
           r.failures, r.expected, stale);
    return r.failures || stale ? 1 : 0;
}
