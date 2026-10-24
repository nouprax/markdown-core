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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"

static const char CURSOR[] = "\xe2\x80\xb8";
static const char SENTINEL[] = "qzxq";
static const char OTHER[] = "vkwv";
enum { SENTINEL_LENGTH = sizeof(SENTINEL) - 1 };

/* The PROPERTIES a slot completes, by the kind of node that holds them
 * (the slot model of scripts/tooling/generate-node-kinds.mjs): any node's
 * anchor and attributes, and the fields of the owners the model names. A
 * Metadata's fields wait for its slots (plan step 2e). */
static const char *const FIELDS[][2] = {
    {"", "anchor"},        {"", "attributes"},    {"Link", "dest"},          {"Embedded", "dest"},
    {"Reference", "dest"}, {"CrossLink", "dest"}, {"CrossEmbedded", "dest"}, {"Reference", "label"},
    {"Footnote", "label"}, {"Specimen", "label"}, {"Citation", "referent"},  {"DirectiveBlock", "name"},
    {"Directive", "name"}, {"Topic", "name"},     {"Callout", "variant"},    {"CodeBlock", "language"},
};

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
        *e = (exception){strdup(text), strdup(space + 1), atoi(colon + 1), 0};
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
            const char *first = memmem(now->start, now->length, SENTINEL, SENTINEL_LENGTH);
            const char *second = memmem(now->start, now->length, OTHER, SENTINEL_LENGTH);
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
    if (!class && !value && !id) {
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
            const char *at = memmem(w->start, w->length, SENTINEL, SENTINEL_LENGTH);
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

/* The checks of one closed slot whose source is `[from, to)`. */
static void check_slot(report *r, const uint8_t *source, size_t length, size_t cursor, size_t from, size_t to,
                       const char *what, const char *kind, const char *owner, check_mode mode) {
    /* A field written in its own brackets or quotes holds its value
     * between them: a destination's `<...>`, a key's `{...}`, a quoted
     * attribute value. A cross link's anchor keeps the `^` that marks a
     * block identifier, which its value drops when the rest is one. */
    size_t starts[2] = {from, from}, ends[2] = {to, to};
    if (to - from >= 2) {
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
    if (same_kind(parts, "Metadata", 8)) {
        return false;
    }
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
            if (markdown_core_context_slot_at(context, i, &slot) != MARKDOWN_CORE_OK || !slot.closed) {
                continue;
            }
            closed = true;
            const size_t from =
                line_offset(source, length, slot.before.start.line) + (size_t)(slot.before.start.column - 1);
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
            markdown_core_slot_kind_name(slot.kind, &name);
            char what[96];
            snprintf(what, sizeof(what), "slot %s %s", name, kind_name(slot.owner));
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
        if (!closed) {
            check_gap(r, source, length, &original, at);
        }
        if (at < length && source[at] == '\n') {
            position.line++;
            line_start = at + 1;
        }
    }
    markdown_core_document_free(document);
    tree_free(&original);
}

/* THE GENERATED INPUTS: each atom inside each inner construct inside each
 * outer one, where the nesting and the escapes the fixtures write one at a
 * time meet. A construct is a template whose `@` the next one fills. */
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
};
static const char *const ATOMS[] = {
    "a", "a b", "\\]", "\\}", "&amp;", "\"q\"", "{.x}", "#t", "@k", "]", "}", "<u>", "`", "(", "|",
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

/* Runs every generated input, named `generated` and numbered from 1. */
static bool check_generated(report *r, size_t *examples) {
    ts_spec_case example = {0};
    r->path = "generated";
    r->example = &example;
    for (size_t outer = 0; outer < CONSTRUCT_COUNT; outer++) {
        for (size_t inner = 0; inner < CONSTRUCT_COUNT; inner++) {
            for (size_t atom = 0; atom < ATOM_COUNT; atom++) {
                char *in = fill(CONSTRUCTS[inner], ATOMS[atom]);
                char *source = in ? fill(CONSTRUCTS[outer], in) : NULL;
                free(in);
                if (!source) {
                    return false;
                }
                example.start_line = (int)++*examples;
                if (getenv("ORACLE_VERBOSE")) {
                    fprintf(stderr, "generated:%d: %s\n", example.start_line, source);
                }
                check_example(r, (const uint8_t *)source, strlen(source));
                free(source);
            }
        }
    }
    return true;
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
    const bool generate = argc > first && !strcmp(argv[first], "--generate");
    first += generate;
    if (argc <= first && !generate) {
        fputs("usage: oracle_runner [--exceptions FILE] [--generate] [FILE...]\n", stderr);
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
