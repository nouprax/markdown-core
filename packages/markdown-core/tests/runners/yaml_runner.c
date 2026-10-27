/* YAML parser runner (elements/yaml.h).
 *
 * Reads each case of the events fixture (tests/yaml/events.txt, whose
 * header gives its notation) and checks the events and comments the parser
 * reads it to, or that it reads none. Then it checks the limits, which take
 * texts too long for the fixture: collections nest at most 1000 deep, block
 * and flow together, and a simple name holds at most 1024 characters to
 * its `:`.
 *
 *   yaml_runner FILE */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yaml.h"

typedef struct {
    char *data;
    size_t size, room;
} text;

static void widen(text *t, size_t more) {
    if (t->size + more + 1 > t->room) {
        t->room = (t->size + more + 1) * 2;
        t->data = realloc(t->data, t->room);
        if (!t->data) {
            fputs("yaml_runner: out of memory\n", stderr);
            exit(2);
        }
    }
}
static void add(text *t, const char *bytes, size_t size) {
    widen(t, size);
    memcpy(t->data + t->size, bytes, size);
    t->size += size;
    t->data[t->size] = 0;
}
static void adds(text *t, const char *s) { add(t, s, strlen(s)); }
static void addf(text *t, size_t value) {
    char number[32];
    snprintf(number, sizeof(number), "%lu", (unsigned long)value);
    adds(t, number);
}
/* Appends `size` bytes with the fixture's escapes. */
static void escape(text *t, const unsigned char *s, size_t size) {
    for (size_t i = 0; i < size; i++) {
        const unsigned char c = s[i];
        char hex[8];
        if (c == '\\') {
            adds(t, "\\\\");
        } else if (c == '\n') {
            adds(t, "\\n");
        } else if (c == '\r') {
            adds(t, "\\r");
        } else if (c == '\t') {
            adds(t, "\\t");
        } else if (c < 0x20 || c == 0x7f) {
            snprintf(hex, sizeof(hex), "\\x%02x", c);
            adds(t, hex);
        } else {
            add(t, (const char *)&c, 1);
        }
    }
}
static int nibble(int c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}
/* The bytes a case's line writes, its escapes read. */
static void unescape(text *t, const char *s, size_t size) {
    for (size_t i = 0; i < size; i++) {
        char c = s[i];
        if (c == '\\' && i + 1 < size) {
            const char e = s[++i];
            if (e == 'x' && i + 2 < size) {
                c = (char)(nibble(s[i + 1]) * 16 + nibble(s[i + 2]));
                i += 2;
            } else {
                c = e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : e;
            }
        }
        add(t, &c, 1);
    }
}

/* The events and comments `p` read from `start`, in the fixture's notation. */
static void render(text *t, const markdown_core_yaml_parser *p, size_t start) {
    static const char *const names[] = {"+STR", "-STR", "+DOC", "-DOC", "=VAL", "+SEQ", "-SEQ", "+MAP", "-MAP"};
    static const char styles[] = ":'\"|";
    for (size_t i = 0; i < p->count; i++) {
        const markdown_core_yaml_event *e = &p->events[i];
        adds(t, names[e->kind]);
        if (e->kind == MARKDOWN_CORE_YAML_SCALAR) {
            adds(t, " ");
            addf(t, e->from - start);
            adds(t, "-");
            addf(t, e->to - start);
            adds(t, " ");
            addf(t, e->line);
            adds(t, " ");
            add(t, &styles[e->style], 1);
            escape(t, e->text, e->size);
        } else if ((e->kind == MARKDOWN_CORE_YAML_LIST_START || e->kind == MARKDOWN_CORE_YAML_MAP_START) && e->flow) {
            adds(t, e->kind == MARKDOWN_CORE_YAML_MAP_START ? " {}" : " []");
        } else if ((e->kind == MARKDOWN_CORE_YAML_DOCUMENT_START || e->kind == MARKDOWN_CORE_YAML_DOCUMENT_END) &&
                   e->explicit) {
            adds(t, e->kind == MARKDOWN_CORE_YAML_DOCUMENT_START ? " ---" : " ...");
        }
        adds(t, "\n");
    }
    for (size_t i = 0; i < p->comment_count; i++) {
        adds(t, "#");
        addf(t, p->comments[i].from - start);
        adds(t, "-");
        addf(t, p->comments[i].to - start);
        adds(t, "\n");
    }
}

static size_t failures, checks;

/* Checks that the text built by repeating reads (`ok`) or fails to. */
static void limit(const char *what, size_t n, const text *t, bool ok) {
    markdown_core_yaml_parser p = {0};
    const bool read = markdown_core_yaml_read(&p, (const unsigned char *)t->data, 0, t->size);
    checks++;
    if (read != ok) {
        failures++;
        printf("limit: %s %lu reads %s, expected %s\n", what, (unsigned long)n, read ? "events" : "an error",
               ok ? "events" : "an error");
    }
    markdown_core_yaml_close(&p);
}
static void repeat(text *t, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        adds(t, s);
    }
}

static void limits(void) {
    for (size_t n = 999; n <= 1001; n++) {
        const bool ok = n <= 1000;
        text t = {0};
        repeat(&t, "[", n);
        repeat(&t, "]", n);
        limit("flow lists", n, &t, ok);
        t.size = 0;
        for (size_t i = 0; i < n; i++) {
            repeat(&t, " ", i);
            adds(&t, "a:\n");
        }
        repeat(&t, " ", n);
        adds(&t, "b");
        limit("block mappings", n, &t, ok);
        t.size = 0;
        repeat(&t, "- ", n);
        adds(&t, "x");
        limit("block lists", n, &t, ok);
        t.size = 0;
        for (size_t i = 0; i < n - 10; i++) {
            repeat(&t, " ", i);
            adds(&t, "a:\n");
        }
        repeat(&t, " ", n - 10);
        repeat(&t, "[", 10);
        repeat(&t, "]", 10);
        limit("block mappings and flow lists", n, &t, ok);
        free(t.data);
    }
    for (size_t n = 1023; n <= 1025; n++) {
        const bool ok = n <= 1024;
        text t = {0};
        repeat(&t, "a", n);
        adds(&t, ": v");
        limit("name of bytes", n, &t, ok);
        t.size = 0;
        repeat(&t, "\xc3\xa9", n);
        adds(&t, ": v");
        limit("name of characters", n, &t, ok);
        t.size = 0;
        repeat(&t, "a", n - 3);
        adds(&t, "   : v");
        limit("name and spaces", n, &t, ok);
        t.size = 0;
        adds(&t, "[");
        repeat(&t, "a", n - 2);
        adds(&t, "]: v");
        limit("flow list name", n, &t, ok);
        free(t.data);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fputs("usage: yaml_runner FILE\n", stderr);
        return 2;
    }
    FILE *file = fopen(argv[1], "rb");
    if (!file) {
        fprintf(stderr, "yaml_runner: cannot open %s\n", argv[1]);
        return 2;
    }
    text all = {0};
    char chunk[65536];
    size_t size;
    while ((size = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        add(&all, chunk, size);
    }
    fclose(file);
    markdown_core_yaml_parser p = {0};
    size_t cases = 0;
    /* The header ends at the first blank line; each case is "> " and its
     * text, then its expected lines, then a blank line. */
    const char *at = strstr(all.data ? all.data : "", "\n\n");
    for (at = at ? at + 2 : all.data + all.size; at < all.data + all.size;) {
        const char *end = strstr(at, "\n\n");
        end = end ? end + 1 : all.data + all.size;
        const char *line = memchr(at, '\n', (size_t)(end - at));
        if (strncmp(at, "> ", 2) != 0 || !line) {
            printf("malformed case at byte %lu\n", (unsigned long)(at - all.data));
            return 2;
        }
        text input = {0}, expected = {0}, actual = {0};
        unescape(&input, at + 2, (size_t)(line - at - 2));
        add(&expected, line + 1, (size_t)(end - line - 1));
        if (markdown_core_yaml_read(&p, (const unsigned char *)(input.data ? input.data : ""), 0, input.size)) {
            render(&actual, &p, 0);
        } else {
            adds(&actual, "error\n");
        }
        cases++;
        checks++;
        if (actual.size != expected.size || memcmp(actual.data, expected.data, actual.size) != 0) {
            failures++;
            if (failures <= 20) {
                printf("case: %.*s\nexpected:\n%sactual:\n%s\n", (int)(line - at - 2), at + 2, expected.data,
                       actual.data);
            }
        }
        free(input.data);
        free(expected.data);
        free(actual.data);
        at = end + 1;
    }
    markdown_core_yaml_close(&p);
    limits();
    free(all.data);
    printf("yaml: %lu cases, %lu checks, %lu failures\n", (unsigned long)cases, (unsigned long)checks,
           (unsigned long)failures);
    return failures || !cases ? 1 : 0;
}
