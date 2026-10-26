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

/* A scalar's value: quoted text is text; plain text is null when empty or
 * `null`, a boolean when `true` or `false`, a number when it spells one,
 * and text otherwise. A value on one line holds no decoded line ending.
 * The scalar's text moves into the value. */
static bool resolve(properties *p, markdown_core_yaml_event *e, markdown_core_metadata_scalar *scalar) {
    const unsigned char *text = e->text;
    const size_t size = e->size;
    if (e->style != MARKDOWN_CORE_YAML_LITERAL && (memchr(text ? text : (const unsigned char *)"", '\n', size) ||
                                                   memchr(text ? text : (const unsigned char *)"", '\r', size))) {
        return false;
    }
    scalar->kind = e->style != MARKDOWN_CORE_YAML_PLAIN                        ? MARKDOWN_CORE_METADATA_TEXT
                   : !size || equals(text, size, "null")                       ? MARKDOWN_CORE_METADATA_NULL
                   : equals(text, size, "true") || equals(text, size, "false") ? MARKDOWN_CORE_METADATA_BOOL
                   : number(text, size)                                        ? MARKDOWN_CORE_METADATA_NUMBER
                                                                               : MARKDOWN_CORE_METADATA_TEXT;
    if (scalar->kind == MARKDOWN_CORE_METADATA_BOOL) {
        scalar->value.boolean = equals(text, size, "true");
    } else if (scalar->kind == MARKDOWN_CORE_METADATA_TEXT || scalar->kind == MARKDOWN_CORE_METADATA_NUMBER) {
        /* Empty text owns one byte, as every string of the value does. */
        unsigned char *owned = e->text ? e->text : markdown_core_alloc(1, 1);
        if (!owned) {
            p->failed = true;
            return false;
        }
        e->text = NULL;
        scalar->value.string = (markdown_core_string){owned, size};
    }
    return true;
}

static void place(properties *p, markdown_core_slot_kind slot, markdown_core_yaml_style style, size_t from, size_t to) {
    markdown_core_metadata_places *places = p->places;
    if (!places) {
        return;
    }
    markdown_core_metadata_place *items =
        markdown_core_reserve(places->items, &places->capacity, places->count + 1, sizeof(*items));
    if (!items) {
        p->failed = true;
        return;
    }
    places->items = items;
    items[places->count++] = (markdown_core_metadata_place){slot, style, from, to};
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
    if (known && decode(p, start, end, &slot, &value) && !p->failed) {
        *slot = value;
        for (size_t i = 0; i < p->yaml.comment_count; i++) {
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
    size_t stop = line;
    while (stop < p->end && !markdown_core_is_line_end(p->text[stop])) {
        stop++;
    }
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
    properties p = {.text = source, .end = close, .work = work, .metadata = node->as.metadata};
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
