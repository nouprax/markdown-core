/* Field and marker oracle runner (docs/plans/2026-10-23-lsp-completion-engine.md,
 * sections 3.3 and 8).
 *
 * The finished parse is the oracle for the field and marker queries. For
 * every node of every example of each spec fixture, and of the generated
 * inputs, the runner reads its markers and its fields of every slot kind
 * and checks them against the parse, comparing canonical dumps with their
 * scopes removed:
 *
 * - MARKERS. A marker is syntax: punctuation, spaces and line endings, and
 *   the digits and the task box of a list item's marker. A letter in one is
 *   a field the reader missed.
 * - PARTITION. No two fields of a node share a byte, and no field shares
 *   one with a marker.
 * - EXACTNESS. A field written without escapes or references is the value
 *   the node's dump line shows, byte for byte.
 * - SUBSTITUTION. A field replaced with a sentinel parses to the same tree,
 *   with the sentinel in the line of the field's node and nothing else
 *   changed. A label a definition declares or a use names is renamed at
 *   every field that writes it, which must leave every resolution as it
 *   was. A field whose bytes a construct around it reads too is judged by
 *   INSERTION instead: the sentinel written before the field.
 *
 *   field_runner [--exceptions FILE] [--generate] [FILE...]
 *
 * An exceptions file names the failures the oracle cannot judge, each with
 * its reason; one that no longer occurs fails the run. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "samples.h"
#include "test_support.h"

static const char CURSOR[] = "\xe2\x80\xb8";
static const char SENTINEL[] = "qzxq";

typedef struct exception {
    char *file, *what;
    int line;
    size_t met;
} exception;

typedef struct report {
    const char *path;
    int line;
    size_t failures, checks, expected, examples;
    exception *exceptions;
    size_t exception_count;
} report;

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/'), *back = strrchr(path, '\\');
    const char *last = slash > back ? slash : back;
    return last ? last + 1 : path;
}

static bool excepted(report *r, const char *what) {
    for (size_t i = 0; i < r->exception_count; i++) {
        exception *e = &r->exceptions[i];
        if (e->line == r->line && !strcmp(e->file, base_name(r->path)) && !strcmp(e->what, what)) {
            e->met++;
            return true;
        }
    }
    return false;
}

/* Reads `FILE:LINE: WHAT # REASON` lines; `#` lines and blank lines are
 * comments. */
static bool read_exceptions(report *r, const char *path) {
    size_t length;
    uint8_t *data = ts_read_file(path, &length);
    if (!data) {
        return false;
    }
    for (char *line = (char *)data; line && *line;) {
        char *next = strchr(line, '\n');
        if (next) {
            *next++ = 0;
        }
        char *hash = strstr(line, " # ");
        if (*line && *line != '#' && hash) {
            *hash = 0;
            char *colon = strchr(line, ':');
            char *what = colon ? strstr(colon + 1, ": ") : NULL;
            if (!colon || !what) {
                free(data);
                return false;
            }
            *colon = 0;
            *what = 0;
            exception *grown = realloc(r->exceptions, (r->exception_count + 1) * sizeof(*grown));
            char *file = malloc(strlen(line) + 1), *text = malloc(strlen(what + 2) + 1);
            if (!grown || !file || !text) {
                free(file);
                free(text);
                free(data);
                return false;
            }
            r->exceptions = grown;
            strcpy(file, line);
            strcpy(text, what + 2);
            r->exceptions[r->exception_count++] = (exception){file, text, atoi(colon + 1), 0};
        }
        line = next;
    }
    free(data);
    return true;
}

static void fail(report *r, const uint8_t *source, size_t length, const char *what, const char *why) {
    r->checks++;
    if (excepted(r, what)) {
        r->expected++;
        return;
    }
    r->failures++;
    if (r->failures > 200 && !getenv("FIELD_ALL")) {
        return;
    }
    fprintf(stderr, "%s:%d: %s: %s\n  source: ", base_name(r->path), r->line, what, why);
    for (size_t i = 0; i < length; i++) {
        const unsigned char c = source[i];
        if (c == '\n') {
            fputs("\\n", stderr);
        } else if (c == '\t') {
            fputs("\\t", stderr);
        } else {
            fputc(c, stderr);
        }
    }
    fputc('\n', stderr);
}

/* THE NODES OF A DOCUMENT that hold a byte, each once, in the order the
 * walk meets them, with their dump lines: a node with no byte of its own
 * names no field and no marker. */
typedef struct {
    const markdown_core_node **items;
    size_t count, capacity;
} nodes;

static bool add_node(nodes *list, const markdown_core_node *node) {
    for (size_t i = 0; i < list->count; i++) {
        if (list->items[i] == node) {
            return true;
        }
    }
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 64;
        const markdown_core_node **grown = realloc((void *)list->items, capacity * sizeof(*grown));
        if (!grown) {
            return false;
        }
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count++] = node;
    return true;
}

static bool collect(const markdown_core_document *document, const uint8_t *source, size_t length, nodes *out) {
    int32_t line = 1, column = 1;
    *out = (nodes){0};
    for (size_t i = 0; i < length; i++) {
        const markdown_core_node **path;
        size_t count;
        if (markdown_core_document_path(document, (markdown_core_position){line, column}, source, length, &path,
                                        &count) == MARKDOWN_CORE_OK) {
            for (size_t k = 0; k < count; k++) {
                if (!add_node(out, path[k])) {
                    markdown_core_path_free(path);
                    return false;
                }
            }
            markdown_core_path_free(path);
        }
        if (source[i] == '\n' || (source[i] == '\r' && !(i + 1 < length && source[i + 1] == '\n'))) {
            line++;
            column = 1;
        } else if ((source[i] & 0xC0) != 0x80) {
            column++;
        }
    }
    return true;
}

/* The byte offset of a position, a line and a UTF-8 column. */
static size_t offset_of(const uint8_t *source, size_t length, markdown_core_position at) {
    size_t offset = 0;
    for (int32_t line = 1; line < at.line && offset < length; offset++) {
        if (source[offset] == '\n' ||
            (source[offset] == '\r' && !(offset + 1 < length && source[offset + 1] == '\n'))) {
            line++;
        }
    }
    return offset + (size_t)at.column - 1;
}

/* A scope as a byte range: its start, and the byte past its end. */
static void extent(const uint8_t *source, size_t length, markdown_core_scope scope, size_t *from, size_t *to) {
    *from = offset_of(source, length, scope.start);
    *to = offset_of(source, length, (markdown_core_position){scope.end.line, scope.end.column + 1});
}

/* The canonical dump, with its scopes when `scoped`, one line per node. */
static char *dumped(const markdown_core_document *document, const uint8_t *source, size_t length, bool scoped) {
    uint8_t *dump;
    size_t size;
    if (markdown_core_document_dump(document, markdown_core_document_root(document), source, length, &dump, &size) !=
        MARKDOWN_CORE_OK) {
        return NULL;
    }
    char *out = malloc(size + 1);
    if (out) {
        size_t n = 0;
        bool quoted = false;
        for (size_t i = 0; i < size;) {
            if (!scoped && !quoted && i + 7 <= size && !memcmp(dump + i, " scope=", 7)) {
                i += 7;
                while (i < size && dump[i] != ' ' && dump[i] != '\n') {
                    i++;
                }
                continue;
            }
            if (quoted && dump[i] == '\\' && i + 1 < size) {
                out[n++] = (char)dump[i++];
            } else if (dump[i] == '"') {
                quoted = !quoted;
            } else if (dump[i] == '\n') {
                quoted = false;
            }
            out[n++] = (char)dump[i++];
        }
        out[n] = 0;
    }
    markdown_core_dump_free(dump);
    return out;
}

/* The `index`th line of `text`, counted from 0, as a range. */
static bool line_at(const char *text, size_t index, const char **start, size_t *size) {
    const char *at = text;
    for (size_t i = 0; i < index; i++) {
        at = strchr(at, '\n');
        if (!at) {
            return false;
        }
        at++;
    }
    const char *end = strchr(at, '\n');
    *start = at;
    *size = end ? (size_t)(end - at) : strlen(at);
    return *size || end;
}

static size_t line_count(const char *text) {
    size_t count = 0;
    for (const char *at = text; *at; at++) {
        count += *at == '\n';
    }
    return count;
}

/* The line of `node`'s dump in `raw`, the dump with its scopes: the line
 * of its kind whose scope is the node's. */
/* The dump's scope key of `node`: its kind and its runs, joined by commas. */
static bool line_tag(const markdown_core_document *document, const markdown_core_node *node, const uint8_t *source,
                     size_t length, char *key, size_t capacity, markdown_core_position *start) {
    markdown_core_scope *scopes;
    size_t count;
    const char *kind = "?";
    markdown_core_node_kind_name(markdown_core_node_get_kind(node), &kind);
    if (markdown_core_document_scope(document, node, source, length, &scopes, &count) != MARKDOWN_CORE_OK) {
        return false;
    }
    int size = snprintf(key, capacity, " %s scope=", kind);
    for (size_t i = 0; i < count && size < (int)capacity - 64; i++) {
        size += snprintf(key + size, capacity - (size_t)size, "%s%d:%d..%d:%d", i ? "," : "", scopes[i].start.line,
                         scopes[i].start.column, scopes[i].end.line, scopes[i].end.column);
    }
    snprintf(key + size, capacity - (size_t)size, " ");
    if (start && count) {
        *start = scopes[0].start;
    }
    markdown_core_scopes_free(scopes);
    return true;
}

/* The line of `node` in the scoped dump `raw`. Nodes of one kind with the
 * same runs, one holding the other (an inline note's citation and the one
 * its note holds), show the same key: the node is the one at its depth. */
static bool node_line(const markdown_core_document *document, const char *raw, const markdown_core_node *node,
                      const uint8_t *source, size_t length, size_t *line) {
    char key[1024], other[1024];
    markdown_core_position start = {1, 1};
    if (!line_tag(document, node, source, length, key, sizeof(key), &start)) {
        return false;
    }
    size_t outer = 0;
    const markdown_core_node **path;
    size_t count;
    if (markdown_core_document_path(document, start, source, length, &path, &count) == MARKDOWN_CORE_OK) {
        for (size_t i = 0; i < count && path[i] != node; i++) {
            outer += line_tag(document, path[i], source, length, other, sizeof(other), NULL) && !strcmp(key, other);
        }
        markdown_core_path_free(path);
    }
    const char *found = strstr(raw, key);
    for (; found && outer; outer--) {
        found = strstr(found + 1, key);
    }
    if (!found) {
        /* The root's line starts the dump. */
        if (strncmp(raw, key + 1, strlen(key) - 1)) {
            return false;
        }
        found = raw;
    }
    *line = 0;
    for (const char *at = raw; at < found; at++) {
        *line += *at == '\n';
    }
    return true;
}

/* What a marker may hold: punctuation, spaces and line endings. */
static bool syntax(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           (c > ' ' && c < 0x7F && !((c | 0x20) >= 'a' && (c | 0x20) <= 'z') && !(c >= '0' && c <= '9'));
}

/* Whether `size` bytes at `text` are a list item's marker that writes its
 * ordinal, `1.`, `a)` or `(iv)`, with the spaces around it: a marker
 * that holds letters and digits. */
static bool ordinal(const uint8_t *text, size_t size) {
    size_t at = 0;
    while (at < size && (text[at] == ' ' || text[at] == '\t')) {
        at++;
    }
    at += at < size && text[at] == '(';
    const size_t first = at;
    while (at < size &&
           ((text[at] >= '0' && text[at] <= '9') || ((text[at] | 0x20) >= 'a' && (text[at] | 0x20) <= 'z'))) {
        at++;
    }
    if (at == first || at >= size || (text[at] != '.' && text[at] != ')')) {
        return false;
    }
    for (at++; at < size; at++) {
        if (!syntax(text[at])) {
            return false;
        }
    }
    return true;
}

/* Whether `size` bytes at `text` are the keyword `word`, in either case,
 * with the spaces around it. */
static bool heads(const uint8_t *text, size_t size, const char *word) {
    size_t at = 0, n = strlen(word);
    while (at < size && (text[at] == ' ' || text[at] == '\t')) {
        at++;
    }
    for (size_t i = 0; i < n; i++, at++) {
        if (at >= size || (text[at] | 0x20) != word[i]) {
            return false;
        }
    }
    for (; at < size; at++) {
        if (!syntax(text[at])) {
            return false;
        }
    }
    return true;
}

/* A field as the runner keeps it: its node, slot kind and byte range. */
typedef struct {
    const markdown_core_node *node;
    size_t index;
    markdown_core_slot_kind slot;
    size_t from, to;
    bool split;
} field;

typedef struct {
    field *items;
    size_t count, capacity;
} fields;

static bool add_field(fields *list, field f) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 64;
        field *grown = realloc(list->items, capacity * sizeof(*grown));
        if (!grown) {
            return false;
        }
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count++] = f;
    return true;
}

static const char *kind_of(const markdown_core_node *node) {
    const char *name = "?";
    markdown_core_node_kind_name(markdown_core_node_get_kind(node), &name);
    return name;
}

static const char *slot_of(markdown_core_slot_kind slot) {
    const char *name = "none";
    if (slot) {
        markdown_core_slot_kind_name(slot, &name);
    }
    return name;
}

/* The source with the ranges of `list` that `pick` names replaced by the
 * sentinel, in a new string. */
static uint8_t *replaced(const uint8_t *source, size_t length, const fields *list, const bool *pick, bool keep,
                         size_t *size) {
    uint8_t *out = malloc(length + list->count * sizeof(SENTINEL) + 1);
    if (!out) {
        return NULL;
    }
    size_t n = 0, at = 0, floor = 0;
    for (;;) {
        size_t next = SIZE_MAX, which = 0;
        for (size_t i = 0; i < list->count; i++) {
            if (pick[i] && list->items[i].from >= floor && list->items[i].from < next) {
                next = list->items[i].from;
                which = i;
            }
        }
        if (next == SIZE_MAX) {
            break;
        }
        memcpy(out + n, source + at, next - at);
        n += next - at;
        memcpy(out + n, SENTINEL, sizeof(SENTINEL) - 1);
        n += sizeof(SENTINEL) - 1;
        at = keep ? next : list->items[which].to;
        floor = keep ? next + 1 : at;
    }
    memcpy(out + n, source + at, length - at);
    n += length - at;
    out[n] = 0;
    *size = n;
    return out;
}

/* Whether a label `a` names what `b` names: alike but for case and runs of
 * whitespace. */
static bool same_label(const uint8_t *a, size_t a_size, const uint8_t *b, size_t b_size) {
    size_t i = 0, j = 0;
    while (i < a_size || j < b_size) {
        const bool space_a = i < a_size && (a[i] == ' ' || a[i] == '\t' || a[i] == '\n');
        const bool space_b = j < b_size && (b[j] == ' ' || b[j] == '\t' || b[j] == '\n');
        if (space_a && space_b) {
            while (i < a_size && (a[i] == ' ' || a[i] == '\t' || a[i] == '\n')) {
                i++;
            }
            while (j < b_size && (b[j] == ' ' || b[j] == '\t' || b[j] == '\n')) {
                j++;
            }
            continue;
        }
        if (i >= a_size || j >= b_size) {
            return false;
        }
        unsigned char x = a[i++], y = b[j++];
        if (x >= 'A' && x <= 'Z') {
            x |= 0x20;
        }
        if (y >= 'A' && y <= 'Z') {
            y |= 0x20;
        }
        if (x != y) {
            return false;
        }
    }
    return true;
}

/* A slot kind whose value a definition declares and a use names. */
static bool naming(markdown_core_slot_kind slot) {
    return slot == MARKDOWN_CORE_SLOT_LABEL || slot == MARKDOWN_CORE_SLOT_FOOTNOTE ||
           slot == MARKDOWN_CORE_SLOT_SPECIMEN;
}

/* SUBSTITUTION of the fields `pick` names: the edited source parses to the
 * same tree but for the lines of their nodes, each of which holds the
 * sentinel. */
/* Whether lines `a` and `b` are alike but for the anchors they show: an
 * identifier substituted in one node frees or takes the name another
 * node's computed anchor is made unique against. */
static bool alike(const char *a, size_t a_size, const char *b, size_t b_size) {
    size_t i = 0, j = 0;
    while (i < a_size && j < b_size) {
        if (a_size - i >= 8 && b_size - j >= 8 && !memcmp(a + i, "anchor=\"", 8) && !memcmp(b + j, "anchor=\"", 8)) {
            for (i += 8; i < a_size && a[i] != '"'; i += a[i] == '\\' ? 2 : 1) {
            }
            for (j += 8; j < b_size && b[j] != '"'; j += b[j] == '\\' ? 2 : 1) {
            }
            continue;
        }
        if (a[i++] != b[j++]) {
            return false;
        }
    }
    return i == a_size && j == b_size;
}

/* SUBSTITUTION of the fields `pick` names, as the comment at the top says.
 * A field whose value a later member of its node overrides, `overridable`,
 * may leave its node's line as it was: whether every line of the picked
 * fields' nodes is. */
/* Why the edited dump `after` fails the original `dump`, whose `owned`
 * lines hold the edited fields, or NULL when it does not; `*unchanged`
 * false when an owned line changed. */
static const char *judged(const char *dump, const char *after, const bool *owned, bool overridable, bool *unchanged,
                          char *why, size_t capacity) {
    const size_t lines = line_count(dump);
    *unchanged = true;
    if (lines != line_count(after)) {
        return "the edit changes the tree's shape";
    }
    for (size_t i = 0; i < lines; i++) {
        const char *a, *b;
        size_t a_size, b_size;
        line_at(dump, i, &a, &a_size);
        line_at(after, i, &b, &b_size);
        const bool same = a_size == b_size && !memcmp(a, b, a_size);
        bool holds = false;
        for (size_t k = 0; k + sizeof(SENTINEL) - 1 <= b_size && !holds; k++) {
            holds = !memcmp(b + k, SENTINEL, sizeof(SENTINEL) - 1);
        }
        *unchanged = *unchanged && (!owned[i] || same);
        if (owned[i] && !holds && overridable && same) {
            continue;
        }
        if (owned[i] ? !holds : !alike(a, a_size, b, b_size)) {
            snprintf(why, capacity, "%s line %zu: %.*s", owned[i] ? "no sentinel in" : "the edit changes", i + 1,
                     (int)(b_size > 300 ? 300 : b_size), b);
            return why;
        }
    }
    return NULL;
}

static bool substitute(report *r, const markdown_core_document *original, const uint8_t *source, size_t length,
                       const char *raw, const char *dump, const fields *list, const bool *pick, const char *what,
                       bool overridable) {
    bool unchanged = true;
    const size_t lines = line_count(dump);
    bool *owned = calloc(lines + 1, sizeof(*owned));
    if (!owned) {
        return true;
    }
    for (size_t i = 0; i < list->count; i++) {
        size_t line;
        if (pick[i] && node_line(original, raw, list->items[i].node, source, length, &line) && line < lines) {
            owned[line] = true;
        }
    }
    r->checks++;
    /* A field whose bytes a construct around its node reads too (a space
     * or a parenthesis that keeps a destination from being one) is judged
     * by insertion: the sentinel written before the field, which keeps its
     * bytes. The substitution's failure is the one reported. */
    char first[512] = "", why[512];
    for (int keep = 0; keep < 2; keep++) {
        size_t size;
        markdown_core_document *document = NULL;
        char *after = NULL;
        uint8_t *edited = replaced(source, length, list, pick, keep, &size);
        const char *failed = "the edit could not be parsed";
        if (edited && markdown_core_document_parse(edited, size, &document) == MARKDOWN_CORE_OK &&
            (after = dumped(document, edited, size, false))) {
            failed = judged(dump, after, owned, overridable, &unchanged, why, sizeof(why));
        }
        markdown_core_document_free(document);
        free(after);
        free(edited);
        if (!failed) {
            first[0] = 0;
            break;
        }
        if (!keep) {
            snprintf(first, sizeof(first), "%s", failed);
        }
    }
    if (first[0]) {
        fail(r, source, length, what, first);
    }
    free(owned);
    return unchanged;
}

static void check_example(report *r, const uint8_t *source, size_t length) {
    markdown_core_document *document;
    if (markdown_core_document_parse(source, length, &document) != MARKDOWN_CORE_OK) {
        fail(r, source, length, "parse", "the example could not be parsed");
        return;
    }
    char *dump = dumped(document, source, length, false), *raw = dumped(document, source, length, true);
    nodes all;
    fields list = {0};
    if (!dump || !raw || !collect(document, source, length, &all)) {
        fail(r, source, length, "parse", "the example could not be read");
        free(dump);
        free(raw);
        markdown_core_document_free(document);
        return;
    }
    r->examples++;
    bool *taken = malloc(length + 1);
    for (size_t n = 0; taken && n < all.count; n++) {
        const markdown_core_node *node = all.items[n];
        const char *kind = kind_of(node);
        markdown_core_scope *scopes;
        size_t count;
        memset(taken, 0, length + 1);
        char what[160];
        if (markdown_core_document_markers(document, node, source, length, &scopes, &count) != MARKDOWN_CORE_OK) {
            snprintf(what, sizeof(what), "%s markers", kind);
            fail(r, source, length, what, "the query failed");
            continue;
        }
        for (size_t i = 0; i < count; i++) {
            size_t from, to;
            extent(source, length, scopes[i], &from, &to);
            r->checks++;
            /* A list item's ordinal, the fence and the word that make a
             * fence a formula's, and a caption's `Table:` are syntax. */
            const bool counts = (!strcmp(kind, "ListItem") && ordinal(source + from, to - from)) ||
                                (!strcmp(kind, "FormulaBlock") && (source[from] == '`' || source[from] == '~')) ||
                                (!strcmp(kind, "TableCaption") && heads(source + from, to - from, "table:"));
            for (size_t k = from; k < to && k < length; k++) {
                taken[k] = true;
                if (!counts && !syntax(source[k])) {
                    snprintf(what, sizeof(what), "%s marker %.*s", kind, (int)(to - from > 40 ? 40 : to - from),
                             (const char *)source + from);
                    fail(r, source, length, what, "a marker holds a byte no syntax writes");
                    break;
                }
            }
        }
        markdown_core_scopes_free(scopes);
        for (int slot = MARKDOWN_CORE_SLOT_NONE; slot <= MARKDOWN_CORE_SLOT_VALUE; slot++) {
            if (markdown_core_document_field(document, node, (markdown_core_slot_kind)slot, source, length, &scopes,
                                             &count) != MARKDOWN_CORE_OK) {
                snprintf(what, sizeof(what), "%s %s", kind, slot_of((markdown_core_slot_kind)slot));
                fail(r, source, length, what, "the query failed");
                continue;
            }
            for (size_t i = 0; i < count; i++) {
                size_t from, to;
                extent(source, length, scopes[i], &from, &to);
                snprintf(what, sizeof(what), "%s %s %.*s", kind, slot_of((markdown_core_slot_kind)slot),
                         (int)(to - from > 40 ? 40 : to - from), (const char *)source + from);
                r->checks++;
                for (size_t k = from; k < to && k < length; k++) {
                    if (taken[k]) {
                        fail(r, source, length, what, "the field shares a byte with another field or a marker");
                        break;
                    }
                    taken[k] = true;
                }
                if (slot != MARKDOWN_CORE_SLOT_NONE &&
                    !add_field(&list, (field){node, n, (markdown_core_slot_kind)slot, from, to, count > 1})) {
                    break;
                }
            }
            markdown_core_scopes_free(scopes);
        }
    }
    free(taken);
    /* EXACTNESS and SUBSTITUTION, field by field. */
    bool *pick = calloc(list.count + 1, sizeof(*pick));
    for (size_t i = 0; pick && i < list.count; i++) {
        const field *f = &list.items[i];
        char what[160];
        snprintf(what, sizeof(what), "%s %s %.*s", kind_of(f->node), slot_of(f->slot),
                 (int)(f->to - f->from > 40 ? 40 : f->to - f->from), (const char *)source + f->from);
        if (f->split) {
            continue;
        }
        memset(pick, 0, list.count * sizeof(*pick));
        bool first = true;
        if (naming(f->slot)) {
            /* Renamed wherever the same name is written, once, when a
             * definition declares it: a heading's text declares a label
             * too, and no field writes that. */
            bool declared = false;
            for (size_t k = 0; k < list.count; k++) {
                const field *g = &list.items[k];
                if (g->slot == f->slot && !g->split &&
                    same_label(source + g->from, g->to - g->from, source + f->from, f->to - f->from)) {
                    pick[k] = true;
                    first = first && k >= i;
                    const char *owner = kind_of(g->node);
                    declared = declared || !strcmp(owner, "Reference") || !strcmp(owner, "Footnote") ||
                               !strcmp(owner, "Specimen");
                }
            }
            /* A collapsed or shortcut reference names its label with its
             * text, which no label field writes and no rename reaches. */
            size_t uses = 0, named = 0;
            for (const char *at = dump;
                 f->slot == MARKDOWN_CORE_SLOT_LABEL && (at = strstr(at, "dest=reference(\""));) {
                at += 16;
                const char *end = strchr(at, '"');
                uses += end && same_label((const uint8_t *)at, (size_t)(end - at), source + f->from, f->to - f->from);
            }
            for (size_t k = 0; k < list.count; k++) {
                named += pick[k] && strcmp(kind_of(list.items[k].node), "Reference");
            }
            for (size_t k = f->from; k < f->to; k++) {
                declared = declared && source[k] != '\\' && source[k] != '&' && source[k] < 0x80;
            }
            first = first && declared && uses == named;
        } else {
            pick[i] = true;
        }
        /* An autolink's destination is its text, which the sentinel does
         * not keep an autolink. */
        if ((f->slot == MARKDOWN_CORE_SLOT_DEST || f->slot == MARKDOWN_CORE_SLOT_ANCHOR) &&
            !strcmp(kind_of(f->node), "Link")) {
            markdown_core_scope *scopes;
            size_t count;
            if (markdown_core_document_scope(document, f->node, source, length, &scopes, &count) == MARKDOWN_CORE_OK) {
                size_t start, end;
                extent(source, length, scopes[0], &start, &end);
                first = first && source[start] == '[';
                markdown_core_scopes_free(scopes);
            }
        }
        /* A properties member's name is one of the grammar's fixed field
         * names, which no sentinel spells, so exactness alone judges it. */
        first = first && f->slot != MARKDOWN_CORE_SLOT_FIELD;
        /* An identifier a later one overrides is no value of its node, nor
         * is a record named `id` or `class`, or its value, whose node holds
         * it as an anchor or classes. */
        const bool overridable = f->slot == MARKDOWN_CORE_SLOT_ID || f->slot == MARKDOWN_CORE_SLOT_RECORD ||
                                 f->slot == MARKDOWN_CORE_SLOT_VALUE;
        if (first && substitute(r, document, source, length, raw, dump, &list, pick, what, overridable) &&
            overridable) {
            continue;
        }
        /* A record named `id` or `class` is no record of its node, and a
         * class run's value is its classes. */
        const bool special = (f->slot == MARKDOWN_CORE_SLOT_RECORD &&
                              ((f->to - f->from == 2 && !memcmp(source + f->from, "id", 2)) ||
                               (f->to - f->from == 5 && !memcmp(source + f->from, "class", 5)))) ||
                             (f->slot == MARKDOWN_CORE_SLOT_VALUE && f->from >= 6 &&
                              (!memcmp(source + f->from - 6, "class=", 6) ||
                               (f->from >= 7 && !memcmp(source + f->from - 7, "class=", 6))));
        /* A label's dump is its normalized form, which folds Unicode case
         * and decodes escapes, as an apostrophe-quoted scalar decodes a
         * doubled apostrophe. */
        bool plain = !special;
        for (size_t k = f->from; naming(f->slot) && k < f->to; k++) {
            plain = plain && source[k] < 0x80;
        }
        for (size_t k = f->from; k < f->to; k++) {
            plain = plain && source[k] != '\\' && source[k] != '&' && source[k] != '"' && source[k] >= ' ' &&
                    !(source[k] == '\'' && k + 1 < f->to && source[k + 1] == '\'');
        }
        size_t line;
        const char *text;
        size_t size;
        if (plain && node_line(document, raw, f->node, source, length, &line) && line_at(dump, line, &text, &size)) {
            bool found = false;
            const size_t width = f->to - f->from;
            for (size_t k = 0; k + width <= size && !found; k++) {
                found = naming(f->slot) ? same_label((const uint8_t *)text + k, width, source + f->from, width)
                                        : !memcmp(text + k, source + f->from, width);
            }
            r->checks++;
            if (!found) {
                char why[512];
                snprintf(why, sizeof(why), "the field is not a value of its node: %.*s", (int)(size > 300 ? 300 : size),
                         text);
                fail(r, source, length, what, why);
            }
        }
    }
    free(pick);
    free(list.items);
    free((void *)all.items);
    free(dump);
    free(raw);
    markdown_core_document_free(document);
}

static void check_one(void *context, const char *source, size_t length, int number) {
    report *r = context;
    r->line = number;
    check_example(r, (const uint8_t *)source, length);
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
        fputs("usage: field_runner [--exceptions FILE] [--generate] [FILE...]\n", stderr);
        return 2;
    }
    r.path = "generated";
    if (generate && !samples_each(check_one, &r)) {
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
            r.line = example->start_line;
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
    printf("%zu examples, %zu checks, %zu failures, %zu excepted, %zu stale exceptions\n", r.examples, r.checks,
           r.failures, r.expected, stale);
    return r.failures || stale ? 1 : 0;
}
