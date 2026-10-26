#include "alloc.h"
#include "properties.h"
#include "metadata.h"
#include "parser.h"
#include "node.h"
#include "parts.h"
#include "buffer.h"
#include "yaml.h"
#include <string.h>
#include <limits.h>

/* PROPERTIES decodes an envelope's payload member by member, in order. A
 * member is a line that holds a name at its start, with the lines it owns:
 * its indented continuation, its block list, and a bracketed or quoted value
 * left open (block_boundary). The YAML parser (yaml.h) reads each member on
 * its own, so an error ends the member, not the payload: the next member
 * begins where the lines say, and its valid neighbours are kept. A member
 * the parser reads as one name and one value of the properties grammar
 * (docs/specs/dialect/properties.md) sets its field, the first time only;
 * any other member is skipped as a whole. No source range is read twice.
 * The parse reads the members of the common shapes directly (direct), as
 * the parser would, and leaves it the rest.
 *
 * The parse decodes the payload once, into the document's metadata, and
 * the places' reader decodes it again on request (properties.h): each
 * member's name, its values, and the bytes the fields skip, for the
 * fields' scopes (parts.h) and the context query's slots. */
typedef struct {
    const unsigned char *text;
    /* Where the payload ends: the closing fence's line. */
    size_t end;
    markdown_core_properties_work *work;
    markdown_core_metadata_fields *metadata;
    /* The reader's places, or NULL in the parse. */
    markdown_core_metadata_places *places;
    markdown_core_yaml_parser yaml;
    /* Whether the direct reader may read a member (direct), and how many
     * it read. */
    bool direct;
    size_t taken;
    bool failed;
    /* One-line lookahead: the boundary line is the next member's line, and
     * its classification belongs to it. `line` starts it, `first` is its
     * first byte that is no space, `key` the byte after a name's colon, or
     * 0, and `stop` where its line ending starts. */
    size_t line, first, key, stop;
    bool classified;
} properties;

static bool plain_start(const unsigned char *s, size_t start, size_t end) {
    if (start == end || markdown_core_is_space_or_tab(s[start]) || strchr(",[]{}#&*!|>'\"%@`", s[start])) {
        return false;
    }
    return !((s[start] == '?' || s[start] == ':') && (start + 1 == end || markdown_core_is_whitespace(s[start + 1])));
}
static size_t next_line(const unsigned char *s, size_t p, size_t end) {
    if (p < end && s[p] == '\r') {
        p++;
    }
    if (p < end && s[p] == '\n') {
        p++;
    }
    return p;
}
static void free_value(markdown_core_metadata_value *v) {
    if (v->kind == MARKDOWN_CORE_METADATA_SCALAR &&
        (v->as.scalar.kind == MARKDOWN_CORE_METADATA_NUMBER || v->as.scalar.kind == MARKDOWN_CORE_METADATA_TEXT)) {
        markdown_core_free((void *)v->as.scalar.value.string.data);
    } else if (v->kind == MARKDOWN_CORE_METADATA_LIST) {
        for (size_t i = 0; i < v->as.list.count; i++) {
            markdown_core_free((void *)v->as.list.items[i].value.data);
        }
        markdown_core_free(v->as.list.items);
    }
    memset(v, 0, sizeof(*v));
}
void markdown_core_metadata_fields_free(markdown_core_metadata_fields *metadata) {
    if (!metadata) {
        return;
    }
    free_value(&metadata->name);
    free_value(&metadata->title);
    free_value(&metadata->subtitle);
    free_value(&metadata->time);
    free_value(&metadata->date);
    free_value(&metadata->authors);
    free_value(&metadata->keywords);
    free_value(&metadata->abstract);
    free_value(&metadata->state);
    free_value(&metadata->comment);
}
static bool equals(const unsigned char *text, size_t size, const char *word) {
    const size_t n = strlen(word);
    return size == n && (!n || memcmp(text, word, n) == 0);
}
/* Resolve the decoded name directly to its optional destination field. */
static markdown_core_metadata_value *field_slot(markdown_core_metadata_fields *metadata, const unsigned char *name,
                                                size_t size) {
    static const char *const names[] = {"name",    "title",    "subtitle", "time",  "date",
                                        "authors", "keywords", "abstract", "state", "comment"};
    markdown_core_metadata_value *const slots[] = {
        &metadata->name,    &metadata->title,    &metadata->subtitle, &metadata->time,  &metadata->date,
        &metadata->authors, &metadata->keywords, &metadata->abstract, &metadata->state, &metadata->comment};
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++) {
        if (equals(name, size, names[i])) {
            return slots[i];
        }
    }
    return NULL;
}
static bool number(const unsigned char *s, size_t size) {
    size_t i = 0;
    if (i < size && s[i] == '-') {
        i++;
    }
    if (i == size) {
        return false;
    }
    if (s[i] == '0') {
        i++;
    } else {
        if (s[i] < '1' || s[i] > '9') {
            return false;
        }
        do {
            i++;
        } while (i < size && s[i] >= '0' && s[i] <= '9');
    }
    if (i < size && s[i] == '.') {
        i++;
        while (i < size && s[i] >= '0' && s[i] <= '9') {
            i++;
        }
    }
    if (i < size && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < size && (s[i] == '+' || s[i] == '-')) {
            i++;
        }
        const size_t digits = i;
        while (i < size && s[i] >= '0' && s[i] <= '9') {
            i++;
        }
        if (digits == i) {
            return false;
        }
    }
    return i == size;
}

/* A scalar's kind: quoted text is text; plain text is null when empty or
 * `null`, a boolean when `true` or `false`, a number when it spells one,
 * and text otherwise. */
static markdown_core_metadata_scalar_kind kind_of(bool plain, const unsigned char *text, size_t size) {
    return !plain                                                      ? MARKDOWN_CORE_METADATA_TEXT
           : !size || equals(text, size, "null")                       ? MARKDOWN_CORE_METADATA_NULL
           : equals(text, size, "true") || equals(text, size, "false") ? MARKDOWN_CORE_METADATA_BOOL
           : number(text, size)                                        ? MARKDOWN_CORE_METADATA_NUMBER
                                                                       : MARKDOWN_CORE_METADATA_TEXT;
}

/* A scalar's value of `kind` and `size` bytes, its text moved in, if any:
 * a boolean keeps no text,
 * and empty text owns one byte, as every string of the value does. */
static bool settle(properties *p, markdown_core_metadata_scalar_kind kind, unsigned char *text, size_t size,
                   markdown_core_metadata_scalar *scalar) {
    scalar->kind = kind;
    if (kind == MARKDOWN_CORE_METADATA_BOOL) {
        /* `true` is the boolean of four letters, `false` of five. */
        scalar->value.boolean = size == 4;
    } else if (kind == MARKDOWN_CORE_METADATA_TEXT || kind == MARKDOWN_CORE_METADATA_NUMBER) {
        unsigned char *owned = text ? text : markdown_core_alloc(1, 1);
        if (!owned) {
            p->failed = true;
            return false;
        }
        scalar->value.string = (markdown_core_string){owned, size};
        return true;
    }
    markdown_core_free(text);
    return true;
}

/* A scalar's value from its event. A value on one line holds no decoded
 * line ending. The scalar's text moves into the value. */
static bool resolve(properties *p, markdown_core_yaml_event *e, markdown_core_metadata_scalar *scalar) {
    unsigned char *text = e->text;
    const size_t size = e->size;
    if (e->style != MARKDOWN_CORE_YAML_LITERAL && (memchr(text ? text : (const unsigned char *)"", '\n', size) ||
                                                   memchr(text ? text : (const unsigned char *)"", '\r', size))) {
        return false;
    }
    e->text = NULL;
    return settle(p, kind_of(e->style == MARKDOWN_CORE_YAML_PLAIN, text, size), text, size, scalar);
}

static void record(properties *p, markdown_core_slot_kind slot, markdown_core_yaml_style style, size_t from,
                   size_t to) {
    markdown_core_metadata_places *places = p->places;
    markdown_core_metadata_place *items =
        markdown_core_reserve(places->items, &places->capacity, places->count + 1, sizeof(*items));
    if (!items) {
        p->failed = true;
        return;
    }
    places->items = items;
    items[places->count++] = (markdown_core_metadata_place){slot, style, from, to};
}

/* Records a place for the reader; the parse keeps none. */
static inline void place(properties *p, markdown_core_slot_kind slot, markdown_core_yaml_style style, size_t from,
                         size_t to) {
    if (p->places) {
        record(p, slot, style, from, to);
    }
}

/* Reads the member that starts its line at `start`, its name at `first`,
 * and ends at `end`: a block mapping of one name, whose value is a scalar
 * on the name's line, a literal there on a prose field, or a list of text
 * and numbers. */
static bool decode(properties *p, size_t start, size_t end, markdown_core_metadata_value **slot,
                   markdown_core_metadata_value *value) {
    markdown_core_yaml_parser *yaml = &p->yaml;
    markdown_core_yaml_event e;
    markdown_core_yaml_open(yaml, p->text, start, end);
    size_t list_capacity = 0;
    bool valid = markdown_core_yaml_next(yaml, &e) && e.kind == MARKDOWN_CORE_YAML_STREAM_START &&
                 markdown_core_yaml_next(yaml, &e) && e.kind == MARKDOWN_CORE_YAML_DOCUMENT_START && !e.explicit &&
                 markdown_core_yaml_next(yaml, &e) && e.kind == MARKDOWN_CORE_YAML_MAP_START && !e.flow &&
                 markdown_core_yaml_next(yaml, &e);
    /* The name. */
    valid = valid && e.kind == MARKDOWN_CORE_YAML_SCALAR && e.style != MARKDOWN_CORE_YAML_LITERAL && e.size &&
            !memchr(e.text, '\n', e.size) && !memchr(e.text, '\r', e.size);
    const size_t line = e.start.line;
    *slot = valid ? field_slot(p->metadata, e.text, e.size) : NULL;
    const bool prose = *slot == &p->metadata->abstract || *slot == &p->metadata->comment;
    valid = valid && *slot && !(*slot)->kind;
    if (valid) {
        place(p, MARKDOWN_CORE_SLOT_FIELD, e.style, e.from, e.to);
    }
    markdown_core_yaml_event_free(&e);
    /* The value. */
    valid = valid && markdown_core_yaml_next(yaml, &e);
    if (valid && e.kind == MARKDOWN_CORE_YAML_SCALAR) {
        value->kind = MARKDOWN_CORE_METADATA_SCALAR;
        valid = e.start.line == line && (e.style != MARKDOWN_CORE_YAML_LITERAL || prose) &&
                resolve(p, &e, &value->as.scalar);
        if (valid) {
            place(p, MARKDOWN_CORE_SLOT_VALUE, e.style, e.from, e.to);
        }
        markdown_core_yaml_event_free(&e);
    } else if (valid && e.kind == MARKDOWN_CORE_YAML_LIST_START) {
        value->kind = MARKDOWN_CORE_METADATA_LIST;
        while ((valid = markdown_core_yaml_next(yaml, &e)) && e.kind == MARKDOWN_CORE_YAML_SCALAR) {
            markdown_core_metadata_scalar item = {0};
            valid = e.style != MARKDOWN_CORE_YAML_LITERAL && resolve(p, &e, &item) &&
                    (item.kind == MARKDOWN_CORE_METADATA_TEXT || item.kind == MARKDOWN_CORE_METADATA_NUMBER);
            markdown_core_metadata_list_item *items = NULL;
            if (valid) {
                place(p, MARKDOWN_CORE_SLOT_VALUE, e.style, e.from, e.to);
                items = markdown_core_reserve(value->as.list.items, &list_capacity, value->as.list.count + 1,
                                              sizeof(*items));
                p->failed = p->failed || !items;
            } else if (item.kind == MARKDOWN_CORE_METADATA_TEXT || item.kind == MARKDOWN_CORE_METADATA_NUMBER) {
                markdown_core_free((void *)item.value.string.data);
            }
            markdown_core_yaml_event_free(&e);
            if (!items) {
                if (valid) {
                    markdown_core_free((void *)item.value.string.data);
                }
                valid = false;
                break;
            }
            value->as.list.items = items;
            items[value->as.list.count++] = (markdown_core_metadata_list_item){
                item.kind == MARKDOWN_CORE_METADATA_NUMBER ? MARKDOWN_CORE_METADATA_ITEM_NUMBER
                                                           : MARKDOWN_CORE_METADATA_ITEM_TEXT,
                item.value.string};
        }
        valid = valid && e.kind == MARKDOWN_CORE_YAML_LIST_END;
        markdown_core_yaml_event_free(&e);
    } else {
        valid = false;
        markdown_core_yaml_event_free(&e);
    }
    /* The end of the mapping, the document and the stream. */
    valid = valid && markdown_core_yaml_next(yaml, &e) && e.kind == MARKDOWN_CORE_YAML_MAP_END &&
            markdown_core_yaml_next(yaml, &e) && e.kind == MARKDOWN_CORE_YAML_DOCUMENT_END && !e.explicit &&
            markdown_core_yaml_next(yaml, &e) && e.kind == MARKDOWN_CORE_YAML_STREAM_END;
    markdown_core_yaml_event_free(&e);
    p->failed = p->failed || yaml->failed;
    return valid;
}

/* THE DIRECT READER. The parse reads the members of the common shapes
 * without the YAML parser: a name at the line's start, then on its line
 * nothing, a plain scalar, a quoted one without escapes, a flow list of
 * those, or a literal of the default header; the member's other lines are
 * blank, or the literal's. It knows each such shape as the YAML parser
 * reads it, so its verdict is the parser's (the direct suite holds the two
 * to each other); any other member, and any byte it does not know, it
 * leaves to the parser. */
typedef enum { DIRECT_UNSURE, DIRECT_KEPT, DIRECT_DROPPED } direct_verdict;

/* The width of the character at `at` that a scalar may hold as it is: an
 * ASCII one that prints, or a tab where `tab`, or a well-formed UTF-8 one,
 * of its shortest form, that prints and breaks no line; else 0. */
static size_t direct_width(const unsigned char *s, size_t at, size_t end, bool tab) {
    const unsigned char c = s[at];
    if (c < 0x80) {
        return (c >= 0x20 && c < 0x7f) || (tab && c == '\t') ? 1 : 0;
    }
    const size_t width = c < 0xc2 ? 0 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : c < 0xf5 ? 4 : 0;
    if (!width || end - at < width) {
        return 0;
    }
    uint32_t code = c & (0x7f >> width);
    for (size_t i = 1; i < width; i++) {
        if ((s[at + i] & 0xc0) != 0x80) {
            return 0;
        }
        code = code << 6 | (s[at + i] & 0x3f);
    }
    const uint32_t least = width == 2 ? 0x80 : width == 3 ? 0x800 : 0x10000;
    return code >= least && code <= 0x10ffff && code >= 0xa0 && code != 0x2028 && code != 0x2029 && code != 0xfeff &&
                   !(code >= 0xd800 && code <= 0xdfff) && code != 0xfffe && code != 0xffff
               ? width
               : 0;
}

/* The end of a plain scalar's text from `at` on its line before `stop`,
 * or 0 when it holds a byte the reader does not know: a `: `, a ` #`
 * starts no text, and in a flow list nor does a flow indicator or `:`. */
static size_t direct_plain(const unsigned char *s, size_t at, size_t stop, bool flow) {
    if (at == stop || (strchr("-?:,[]{}#&*!|>'\"%@`", s[at]) &&
                       !(s[at] == '-' && at + 1 < stop && s[at + 1] != ' ' && s[at + 1] != '\t'))) {
        return 0;
    }
    size_t last = at;
    for (size_t i = at; i < stop;) {
        const unsigned char c = s[i];
        if (c == ' ') {
            i++;
            continue;
        }
        if ((c == '#' && s[i - 1] == ' ') || (flow && (c == ',' || c == ']'))) {
            break;
        }
        if (c == ':' ? flow || i + 1 == stop || s[i + 1] == ' ' || s[i + 1] == '\t' : flow && strchr("[{}", c)) {
            return 0;
        }
        const size_t width = direct_width(s, i, stop, false);
        if (!width) {
            return 0;
        }
        i += width;
        last = i;
    }
    return last;
}

/* The end of a quoted scalar opened at `at`, past its closing quote, or 0
 * when it escapes, doubles its quote or leaves its line. */
static size_t direct_quoted(const unsigned char *s, size_t at, size_t stop) {
    const unsigned char quote = s[at];
    for (size_t i = at + 1; i < stop;) {
        if (s[i] == quote) {
            return i + 1 < stop && s[i + 1] == quote ? 0 : i + 1;
        }
        const size_t width = s[i] == '\\' && quote == '"' ? 0 : direct_width(s, i, stop, true);
        if (!width) {
            return 0;
        }
        i += width;
    }
    return 0;
}

/* Whether the line goes on from `at` to `stop` with nothing but blanks and
 * then a comment separated from what precedes `at`. */
static bool direct_rest(const unsigned char *s, size_t at, size_t stop) {
    size_t i = at;
    while (i < stop && s[i] == ' ') {
        i++;
    }
    return i == stop || (i > at && s[i] == '#');
}

/* A scalar's value from the text at `from` to `to`, copied. */
static direct_verdict direct_scalar(properties *p, bool plain, size_t from, size_t to,
                                    markdown_core_metadata_scalar *scalar) {
    const unsigned char *text = p->text + from;
    const size_t size = to - from;
    const markdown_core_metadata_scalar_kind kind = kind_of(plain, text, size);
    unsigned char *copy = NULL;
    if (kind == MARKDOWN_CORE_METADATA_TEXT || kind == MARKDOWN_CORE_METADATA_NUMBER) {
        copy = markdown_core_alloc(size + 1, 1);
        if (!copy) {
            p->failed = true;
            return DIRECT_DROPPED;
        }
        memcpy(copy, text, size);
    }
    return settle(p, kind, copy, size, scalar) ? DIRECT_KEPT : DIRECT_DROPPED;
}

/* A flow list from its `[` at `at` on its line before `stop`. */
static direct_verdict direct_list(properties *p, size_t at, size_t stop, markdown_core_metadata_value *value) {
    const unsigned char *s = p->text;
    size_t capacity = 0;
    value->kind = MARKDOWN_CORE_METADATA_LIST;
    for (size_t i = at + 1;;) {
        while (i < stop && s[i] == ' ') {
            i++;
        }
        if (i < stop && s[i] == ']') {
            return direct_rest(s, i + 1, stop) ? DIRECT_KEPT : DIRECT_UNSURE;
        }
        if (i == stop) {
            return DIRECT_UNSURE;
        }
        const bool plain = s[i] != '"' && s[i] != '\'';
        const size_t to = plain ? direct_plain(s, i, stop, true) : direct_quoted(s, i, stop);
        if (!to) {
            return DIRECT_UNSURE;
        }
        /* A null or boolean item drops the list, whatever follows. */
        markdown_core_metadata_scalar item = {0};
        if (direct_scalar(p, plain, plain ? i : i + 1, plain ? to : to - 1, &item) != DIRECT_KEPT ||
            (item.kind != MARKDOWN_CORE_METADATA_TEXT && item.kind != MARKDOWN_CORE_METADATA_NUMBER)) {
            return DIRECT_DROPPED;
        }
        markdown_core_metadata_list_item *items =
            markdown_core_reserve(value->as.list.items, &capacity, value->as.list.count + 1, sizeof(*items));
        if (!items) {
            markdown_core_free((void *)item.value.string.data);
            p->failed = true;
            return DIRECT_DROPPED;
        }
        value->as.list.items = items;
        items[value->as.list.count++] = (markdown_core_metadata_list_item){item.kind == MARKDOWN_CORE_METADATA_NUMBER
                                                                               ? MARKDOWN_CORE_METADATA_ITEM_NUMBER
                                                                               : MARKDOWN_CORE_METADATA_ITEM_TEXT,
                                                                           item.value.string};
        i = to;
        while (i < stop && s[i] == ' ') {
            i++;
        }
        if (i < stop && s[i] == ',') {
            i++;
        } else if (i == stop || s[i] != ']') {
            return DIRECT_UNSURE;
        }
    }
}

/* A literal of the default header, its lines from `line` to `end`: each
 * past the indentation of its first line with text, blank lines kept
 * between them and one line ending kept after the last. A blank line of
 * more spaces than that indentation, a line of less, a tab in it and a
 * carriage return are the parser's. */
static direct_verdict direct_literal(properties *p, size_t line, size_t end, markdown_core_metadata_scalar *scalar) {
    const unsigned char *s = p->text;
    unsigned char *text = markdown_core_alloc(end - line + 1, 1);
    if (!text) {
        p->failed = true;
        return DIRECT_DROPPED;
    }
    /* Blank lines before the first line with text are no deeper than it. */
    size_t size = 0, kept = 0, indent = 0, deepest = 0;
    for (size_t at = line; at < end;) {
        size_t stop = at, spaces;
        while (stop < end && s[stop] != '\n') {
            stop++;
        }
        for (spaces = at; spaces < stop && s[spaces] == ' '; spaces++) {
        }
        bool known = stop < end;
        if (spaces == stop) {
            known = known && (!indent || spaces - at <= indent);
            deepest = indent || deepest > spaces - at ? deepest : spaces - at;
            text[size++] = '\n';
        } else {
            indent = indent ? indent : spaces - at;
            known = known && spaces - at >= indent && deepest <= indent && s[spaces] != '\t';
            for (size_t i = at + indent; known && i < stop;) {
                const size_t width = direct_width(s, i, stop, true);
                known = width != 0;
                memcpy(text + size, s + i, width);
                size += width;
                i += width;
            }
            text[size++] = '\n';
            kept = size;
        }
        if (!known) {
            markdown_core_free(text);
            return DIRECT_UNSURE;
        }
        at = stop + 1;
    }
    return settle(p, MARKDOWN_CORE_METADATA_TEXT, text, kept, scalar) ? DIRECT_KEPT : DIRECT_DROPPED;
}

/* Reads the member at `start`, whose plain name, from `start` to its colon
 * before `key`, names the unset field `slot`, and which ends at `end`. */
static direct_verdict direct(properties *p, size_t start, size_t key, size_t end,
                             const markdown_core_metadata_value *slot, markdown_core_metadata_value *value) {
    const unsigned char *s = p->text;
    if (s[key - 2] == ' ' || s[key - 2] == '\t') {
        return DIRECT_UNSURE;
    }
    size_t stop = key;
    while (stop < end && s[stop] != '\n' && s[stop] != '\r') {
        stop++;
    }
    size_t next = stop < end && s[stop] == '\r' ? stop + 1 : stop;
    if (next == end || s[next] != '\n') {
        return DIRECT_UNSURE;
    }
    size_t at = key;
    while (at < stop && s[at] == ' ') {
        at++;
    }
    const bool prose = slot == &p->metadata->abstract || slot == &p->metadata->comment;
    if (at + 1 == stop && s[at] == '|') {
        value->kind = MARKDOWN_CORE_METADATA_SCALAR;
        if (!prose) {
            return DIRECT_DROPPED;
        }
        return s[stop] == '\r' ? DIRECT_UNSURE : direct_literal(p, next + 1, end, &value->as.scalar);
    }
    /* The member's other lines are blank. */
    for (size_t i = next + 1; i < end; i++) {
        if (s[i] != ' ' && s[i] != '\n' && !(s[i] == '\r' && i + 1 < end && s[i + 1] == '\n')) {
            return DIRECT_UNSURE;
        }
    }
    if (at < stop && at == key) {
        return DIRECT_UNSURE;
    }
    if (at == stop || s[at] == '#') {
        value->kind = MARKDOWN_CORE_METADATA_SCALAR;
        return DIRECT_KEPT;
    }
    if (s[at] == '[') {
        return direct_list(p, at, stop, value);
    }
    const bool plain = s[at] != '"' && s[at] != '\'';
    const size_t to = plain ? direct_plain(s, at, stop, false) : direct_quoted(s, at, stop);
    if (!to || !direct_rest(s, to, stop)) {
        return DIRECT_UNSURE;
    }
    value->kind = MARKDOWN_CORE_METADATA_SCALAR;
    return direct_scalar(p, plain, plain ? at : at + 1, plain ? to : to - 1, &value->as.scalar);
}

/* Reads the member that starts its line at `start`, its name at `first`
 * and the byte after the name's colon at `key`, and ends at `end`. The
 * line's classification found the name: a member without one, and one
 * whose plain name is no field or a field already set, is skipped without
 * reading its value. */
static void member(properties *p, size_t start, size_t first, size_t key, size_t end) {
    markdown_core_metadata_value *slot = NULL, value = {0};
    const size_t count = p->places ? p->places->count : 0;
    bool known = key != 0;
    if (known && p->text[first] != '"' && p->text[first] != '\'') {
        size_t stop = key - 1;
        while (stop > first && markdown_core_is_space_or_tab(p->text[stop - 1])) {
            stop--;
        }
        slot = field_slot(p->metadata, p->text + first, stop - first);
        known = slot && !slot->kind;
    }
    if (known) {
        p->work->decoded_bytes += end - start;
    }
    direct_verdict verdict =
        slot && known && p->direct && first == start ? direct(p, start, key, end, slot, &value) : DIRECT_UNSURE;
    p->taken += verdict != DIRECT_UNSURE;
    if (verdict == DIRECT_UNSURE && known) {
        free_value(&value);
        verdict = decode(p, start, end, &slot, &value) ? DIRECT_KEPT : DIRECT_DROPPED;
    }
    if (verdict == DIRECT_KEPT && !p->failed) {
        *slot = value;
        for (size_t i = 0; p->places && i < p->yaml.comment_count; i++) {
            place(p, MARKDOWN_CORE_SLOT_NONE, MARKDOWN_CORE_YAML_PLAIN, p->yaml.comments[i].from,
                  p->yaml.comments[i].to);
        }
        return;
    }
    free_value(&value);
    /* A member skipped as a whole is one field of no slot kind, from its
     * name to its last byte that is no space or line ending. */
    if (p->places) {
        p->places->count = count;
    }
    while (end > first && markdown_core_is_whitespace(p->text[end - 1])) {
        end--;
    }
    place(p, MARKDOWN_CORE_SLOT_NONE, MARKDOWN_CORE_YAML_PLAIN, first, end);
}

/* Return the byte after a directly authored block key's colon. The same
 * lexical rule identifies recovery boundaries. Flow punctuation is
 * ordinary content inside a block plain key; only a separated # is a comment. */
static size_t block_key_end(const unsigned char *s, size_t start, size_t end) {
    if (start == end) {
        return 0;
    }
    if (s[start] == '\'' || s[start] == '"') {
        unsigned char quote = s[start++];
        bool closed = false;
        while (start < end) {
            unsigned char c = s[start++];
            if (quote == '"' && c == '\\' && start < end) {
                start++;
            } else if (c == quote) {
                if (quote == '\'' && start < end && s[start] == '\'') {
                    start++;
                } else {
                    closed = true;
                    break;
                }
            }
        }
        while (start < end && markdown_core_is_space_or_tab(s[start])) {
            start++;
        }
        return closed && start < end && s[start] == ':' &&
                       (start + 1 == end || markdown_core_is_space_or_tab(s[start + 1]))
                   ? start + 1
                   : 0;
    }
    if (!plain_start(s, start, end)) {
        return 0;
    }
    for (size_t i = start; i < end; i++) {
        if (s[i] == ':' && (i + 1 == end || markdown_core_is_space_or_tab(s[i + 1]))) {
            return i + 1;
        }
        if (s[i] == '#' && (i == start || markdown_core_is_space_or_tab(s[i - 1]))) {
            return 0;
        }
    }
    return 0;
}

/* A boundary probe leaves its classification for the member it found. All
 * callers progress in line order, so no per-member allocation or rescan is
 * needed, including when a malformed quoted value recovers at a later key. */
static void classify_line(properties *p, size_t line) {
    if (p->classified && p->line == line) {
        return;
    }
    p->classified = true;
    p->line = line;
    /* The line ends at its first carriage return or line feed: two scans
     * that skip whole words of the line. */
    const unsigned char *feed = memchr(p->text + line, '\n', p->end - line);
    const size_t limit = feed ? (size_t)(feed - p->text) : p->end;
    const unsigned char *ret = memchr(p->text + line, '\r', limit - line);
    const size_t stop = ret ? (size_t)(ret - p->text) : limit;
    p->stop = stop;
    size_t first = line;
    while (first < stop && p->text[first] == ' ') {
        first++;
    }
    p->first = first;
    p->key = block_key_end(p->text, first, stop);
    p->work->key_work += stop - line;
}

/* Return where the first line after `line` that the member starting there
 * does not own starts, or the payload's end when it runs to the closing
 * fence. */
static size_t block_boundary(properties *p, size_t line, size_t indent) {
    const unsigned char *s = p->text;
    enum { VALUE_PREFIX, VALUE_SCALAR, VALUE_QUOTED, VALUE_SEQUENCE, VALUE_BRACKETED } form = VALUE_PREFIX;
    size_t array_depth = 0, object_depth = 0;
    unsigned char quote = 0;
    bool first = true;
    for (; line < p->end; line = next_line(s, p->stop, p->end)) {
        classify_line(p, line);
        size_t cursor = line, e = p->stop, nonspace = p->first;
        if (!first && nonspace < e && nonspace - cursor <= indent) {
            bool list_line = nonspace - cursor == indent && s[nonspace] == '-' &&
                             (nonspace + 1 == e || markdown_core_is_space_or_tab(s[nonspace + 1]));
            bool recovery_key = p->key != 0;
            size_t content = nonspace;
            while (content < e && markdown_core_is_space_or_tab(s[content])) {
                content++;
            }
            bool separation = content == e || s[content] == '#';
            bool continuation = content == e ||
                                ((form == VALUE_PREFIX || form == VALUE_SEQUENCE) && (list_line || separation)) ||
                                array_depth != 0 || object_depth != 0 || (quote && !recovery_key);
            if (!continuation) {
                return line;
            }
        }
        size_t token = first ? p->key : 0;
        for (size_t i = token ? token : nonspace; i < e; i++) {
            unsigned char c = s[i];
            if (form == VALUE_PREFIX) {
                if (markdown_core_is_space_or_tab(c)) {
                    continue;
                }
                if (c == '#') {
                    break;
                }
                /* Only the value's first token can open a bracketed collection.
                 * Brackets and quotes within block plain scalars, including
                 * block sequence items, never extend the root member. */
                form = c == '[' || c == '{'                                                            ? VALUE_BRACKETED
                       : c == '\'' || c == '"'                                                         ? VALUE_QUOTED
                       : !first && c == '-' && (i + 1 == e || markdown_core_is_space_or_tab(s[i + 1])) ? VALUE_SEQUENCE
                                                                                                       : VALUE_SCALAR;
            }
            if (form != VALUE_BRACKETED && form != VALUE_QUOTED) {
                break;
            }
            if (quote) {
                if (quote == '"' && c == '\\' && i + 1 < e) {
                    i++;
                } else if (c == quote) {
                    if (quote == '\'' && i + 1 < e && s[i + 1] == '\'') {
                        i++;
                    } else {
                        quote = 0;
                        if (form == VALUE_QUOTED) {
                            form = VALUE_SCALAR;
                        }
                    }
                }
            } else if ((c == '\'' || c == '"') &&
                       (i == nonspace || markdown_core_is_space_or_tab(s[i - 1]) || strchr(":,[{?", s[i - 1]))) {
                quote = c;
            } else if (c == '#' && (i == nonspace || markdown_core_is_space_or_tab(s[i - 1]))) {
                break;
            } else if (c == '[') {
                array_depth++;
            } else if (c == '{') {
                object_depth++;
            } else if (c == ']' || c == '}') {
                /* A mismatched closer cannot release an unfinished member.
                 * These counters only bound ownership; nested values are not decoded. */
                size_t *depth = c == ']' ? &array_depth : &object_depth;
                if (*depth) {
                    --*depth;
                }
                if (!array_depth && !object_depth) {
                    form = VALUE_SCALAR;
                }
            }
        }
        first = false;
    }
    return p->end;
}

/* Decodes the payload from `start` to `p->end`. A blank line, a comment
 * line, a `%` line and a `...` line between members are no member; the
 * reader takes the bytes of each but a blank one as a field of no slot
 * kind. */
static void payload(properties *p, size_t start) {
    const unsigned char *s = p->text;
    for (size_t line = start; line < p->end && !p->failed;) {
        classify_line(p, line);
        size_t first = p->first, e = p->stop;
        size_t content = first;
        while (content < e && markdown_core_is_space_or_tab(s[content])) {
            content++;
        }
        if (content == e || s[content] == '#' || s[first] == '%' ||
            (e - first >= 3 && memcmp(s + first, "...", 3) == 0 &&
             (e == first + 3 || markdown_core_is_space_or_tab(s[first + 3])))) {
            place(p, MARKDOWN_CORE_SLOT_NONE, MARKDOWN_CORE_YAML_PLAIN, content, e);
            line = next_line(s, e, p->end);
            continue;
        }
        const size_t key = p->key, boundary = block_boundary(p, line, first - line);
        member(p, line, first, key, boundary);
        line = boundary;
    }
}

static void properties_close(properties *p) { markdown_core_yaml_close(&p->yaml); }

/* Whether the input holds the fence's three dashes at `at`. Each byte asks
 * whether the input goes on there (markdown_core_parser_input_ends). */
static bool properties_dashes_at(markdown_core_parser *parser, const unsigned char *source, size_t at) {
    for (size_t i = at; i < at + 3; i++) {
        if (markdown_core_parser_input_ends(parser, source + i) || source[i] != '-') {
            return false;
        }
    }
    return true;
}

int markdown_core_properties_fence(const unsigned char *data) {
    int dashes = 0;
    while (dashes < 3 && data[dashes] == '-') {
        dashes++;
    }
    return markdown_core_is_line_end(data[dashes]) ? dashes : -1;
}

bool markdown_core_properties_closed(markdown_core_properties_work *work, markdown_core_parser *parser, size_t *fence) {
    size_t length;
    const unsigned char *source = markdown_core_parser_input_text(parser, &length);
    /* The opener is exactly "---" and a line ending: a peek, not a scan. */
    if (!properties_dashes_at(parser, source, 0) || markdown_core_parser_input_ends(parser, source + 3) ||
        !markdown_core_is_line_end(source[3])) {
        return false;
    }
    size_t close = next_line(source, 3, length);
    /* THE FENCE NEEDS NO LINE GEOMETRY. It is "---" bracketed by line ends,
     * so the search for it is one `memchr` pass over the dashes, checked at
     * each hit for the line start before it and the line end after it, and
     * it allocates nothing: a document that opens with a thematic break and
     * never closes an envelope -- ordinary Markdown -- costs one pass and
     * one byte of state. The shared input index is extended through the
     * closing fence afterwards; the block driver later reuses those entries. */
    bool closed = false;
    size_t fence_work = 0;
    while (!markdown_core_parser_input_ends(parser, source + close)) {
        const unsigned char *hit = memchr(source + close, '-', length - close);
        if (!hit) {
            fence_work += length - close;
            close = length;
            continue;
        }
        size_t at = (size_t)(hit - source);
        fence_work += at - close + 1;
        if (markdown_core_is_line_end(source[at - 1]) && properties_dashes_at(parser, source, at) &&
            (markdown_core_parser_input_ends(parser, source + at + 3) || markdown_core_is_line_end(source[at + 3]))) {
            close = at;
            closed = true;
            break;
        }
        close = at + 1;
    }
    work->line_work += fence_work;
    *fence = close;
    return closed;
}

size_t markdown_core_properties_parse(markdown_core_properties_work *work, markdown_core_parser *parser, size_t close) {
    size_t length;
    const unsigned char *source = markdown_core_parser_input_text(parser, &length);
    int number = 1;
    markdown_core_input_line *line;
    do {
        line = markdown_core_parser_source_line(parser, number++);
    } while (line && line->start < close);
    if (!line) {
        return 0;
    }
    const size_t count = (size_t)number - 3;
    markdown_core_node *node = markdown_core_parser_make_node(parser, MARKDOWN_CORE_NODE_METADATA);
    if (!node) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return 0;
    }
    size_t consumed = next_line(source, close + 3, length);
    properties p = {.text = source, .end = close, .work = work, .metadata = node->as.metadata, .direct = true};
    node->place = (markdown_core_place){0, (uint32_t)(close + 3)};
    payload(&p, next_line(source, 3, length));
    properties_close(&p);
    if (p.failed) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    if (parser->error) {
        markdown_core_parser_release_node(parser, node);
        return 0;
    }
    parser->root->as.document->metadata = node;
    markdown_core_parser_settle_field(parser, node);
    parser->at.line_number = (int)(count + 2);
    parser->at.last_line_end = (bufsize_t)(close + 3);
    return consumed;
}

bool markdown_core_metadata_text(const unsigned char *text, size_t size) {
    return size && !equals(text, size, "null") && !equals(text, size, "true") && !equals(text, size, "false") &&
           !number(text, size);
}

bool markdown_core_metadata_places_read(const unsigned char *text, size_t length,
                                        markdown_core_metadata_places *places) {
    const size_t end = length >= 3 ? length - 3 : 0;
    markdown_core_metadata_fields fields = {0};
    markdown_core_properties_work work = {0};
    properties p = {.text = text, .end = end, .work = &work, .metadata = &fields, .places = places};
    payload(&p, next_line(text, 3 < end ? 3 : end, end));
    properties_close(&p);
    markdown_core_metadata_fields_free(&fields);
    return !p.failed;
}

bool markdown_core_metadata_fields_read(const unsigned char *text, size_t length, bool direct,
                                        markdown_core_metadata_fields *fields, size_t *taken) {
    const size_t end = length >= 3 ? length - 3 : 0;
    markdown_core_properties_work work = {0};
    properties p = {.text = text, .end = end, .work = &work, .metadata = fields, .direct = direct};
    payload(&p, next_line(text, 3 < end ? 3 : end, end));
    properties_close(&p);
    *taken = p.taken;
    return !p.failed;
}

/* An envelope's parts: its places that hold bytes. */
void markdown_core_metadata_parts(markdown_core_parts *parts) {
    markdown_core_metadata_places places = {0};
    parts->failed = !markdown_core_metadata_places_read(parts->text, (size_t)parts->length, &places) || parts->failed;
    for (size_t i = 0; i < places.count; i++) {
        markdown_core_parts_add(parts, places.items[i].slot, (bufsize_t)places.items[i].from,
                                (bufsize_t)places.items[i].to);
    }
    markdown_core_free(places.items);
}
