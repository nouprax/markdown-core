#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The deepest nesting a read takes; the reader recurses once per level. */
enum { JSON_DEPTH = 512 };

typedef struct reader {
    json_tree *tree;
    const uint8_t *text;
    size_t size;
    size_t at;
    json_read_status status;
} reader;

static bool fail(reader *in, json_read_status status) {
    if (in->status == JSON_READ_OK) {
        in->status = status;
    }
    return false;
}

static bool widen(void **data, size_t *capacity, size_t need, size_t width) {
    if (need <= *capacity) {
        return true;
    }
    size_t next = *capacity < 64 ? 64 : *capacity;
    while (next < need) {
        if (next > SIZE_MAX / 2 / width) {
            return false;
        }
        next *= 2;
    }
    void *grown = realloc(*data, next * width);
    if (!grown) {
        return false;
    }
    *data = grown;
    *capacity = next;
    return true;
}

/* Appends a value of `kind` and answers its index, or SIZE_MAX. */
static size_t push(reader *in, json_kind kind) {
    json_tree *tree = in->tree;
    if (!widen((void **)&tree->values, &tree->capacity, tree->count + 1, sizeof(*tree->values))) {
        fail(in, JSON_READ_FAILED);
        return SIZE_MAX;
    }
    json_value *value = &tree->values[tree->count];
    value->kind = kind;
    value->end = tree->count + 1;
    value->at = tree->size;
    value->size = 0;
    return tree->count++;
}

static bool put(reader *in, const uint8_t *bytes, size_t size) {
    json_tree *tree = in->tree;
    /* An empty tree has no bytes yet, and no copy may name it. */
    if (size == 0) {
        return true;
    }
    if (!widen((void **)&tree->bytes, &tree->room, tree->size + size, 1)) {
        return fail(in, JSON_READ_FAILED);
    }
    memcpy(tree->bytes + tree->size, bytes, size);
    tree->size += size;
    return true;
}

static void skip_space(reader *in) {
    while (in->at < in->size) {
        uint8_t byte = in->text[in->at];
        if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r') {
            return;
        }
        in->at++;
    }
}

static bool literal(reader *in, const char *word) {
    size_t size = strlen(word);
    if (in->size - in->at < size || memcmp(in->text + in->at, word, size) != 0) {
        return fail(in, JSON_READ_MALFORMED);
    }
    in->at += size;
    return true;
}

static int hex(uint8_t byte) {
    if (byte >= '0' && byte <= '9') {
        return byte - '0';
    }
    if (byte >= 'a' && byte <= 'f') {
        return byte - 'a' + 10;
    }
    if (byte >= 'A' && byte <= 'F') {
        return byte - 'A' + 10;
    }
    return -1;
}

/* The four hex digits of a `\u` escape at `at`, or -1. */
static long escape_unit(const reader *in, size_t at) {
    if (in->size - at < 4) {
        return -1;
    }
    long unit = 0;
    for (size_t i = 0; i < 4; i++) {
        int digit = hex(in->text[at + i]);
        if (digit < 0) {
            return -1;
        }
        unit = unit * 16 + digit;
    }
    return unit;
}

static bool put_scalar(reader *in, unsigned long scalar) {
    uint8_t bytes[4];
    size_t size;
    if (scalar < 0x80) {
        bytes[0] = (uint8_t)scalar;
        size = 1;
    } else if (scalar < 0x800) {
        bytes[0] = (uint8_t)(0xC0 | (scalar >> 6));
        bytes[1] = (uint8_t)(0x80 | (scalar & 0x3F));
        size = 2;
    } else if (scalar < 0x10000) {
        bytes[0] = (uint8_t)(0xE0 | (scalar >> 12));
        bytes[1] = (uint8_t)(0x80 | ((scalar >> 6) & 0x3F));
        bytes[2] = (uint8_t)(0x80 | (scalar & 0x3F));
        size = 3;
    } else {
        bytes[0] = (uint8_t)(0xF0 | (scalar >> 18));
        bytes[1] = (uint8_t)(0x80 | ((scalar >> 12) & 0x3F));
        bytes[2] = (uint8_t)(0x80 | ((scalar >> 6) & 0x3F));
        bytes[3] = (uint8_t)(0x80 | (scalar & 0x3F));
        size = 4;
    }
    return put(in, bytes, size);
}

/* A string at the opening quote, decoded into the tree's bytes. */
static bool read_string(reader *in) {
    size_t index = push(in, JSON_STRING);
    if (index == SIZE_MAX) {
        return false;
    }
    size_t start = in->tree->size;
    in->at++;
    for (;;) {
        size_t run = in->at;
        while (in->at < in->size && in->text[in->at] != '"' && in->text[in->at] != '\\' && in->text[in->at] >= 0x20) {
            in->at++;
        }
        if (!put(in, in->text + run, in->at - run)) {
            return false;
        }
        if (in->at == in->size || in->text[in->at] < 0x20) {
            return fail(in, JSON_READ_MALFORMED);
        }
        if (in->text[in->at] == '"') {
            in->at++;
            break;
        }
        /* An escape. */
        if (in->size - in->at < 2) {
            return fail(in, JSON_READ_MALFORMED);
        }
        uint8_t letter = in->text[in->at + 1];
        static const char from[] = "\"\\/bfnrt";
        static const char to[] = "\"\\/\b\f\n\r\t";
        const char *found = letter ? strchr(from, letter) : NULL;
        if (found) {
            uint8_t byte = (uint8_t)to[found - from];
            if (!put(in, &byte, 1)) {
                return false;
            }
            in->at += 2;
            continue;
        }
        if (letter != 'u') {
            return fail(in, JSON_READ_MALFORMED);
        }
        long unit = escape_unit(in, in->at + 2);
        if (unit < 0) {
            return fail(in, JSON_READ_MALFORMED);
        }
        in->at += 6;
        unsigned long scalar = (unsigned long)unit;
        if (unit >= 0xD800 && unit <= 0xDBFF && in->size - in->at >= 6 && in->text[in->at] == '\\' &&
            in->text[in->at + 1] == 'u') {
            long low = escape_unit(in, in->at + 2);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                scalar = 0x10000 + (((unsigned long)unit - 0xD800) << 10) + ((unsigned long)low - 0xDC00);
                in->at += 6;
            }
        }
        if (scalar >= 0xD800 && scalar <= 0xDFFF) {
            scalar = 0xFFFD;
        }
        if (!put_scalar(in, scalar)) {
            return false;
        }
    }
    json_value *value = &in->tree->values[index];
    value->at = start;
    value->size = in->tree->size - start;
    return true;
}

static bool decimals(reader *in) {
    size_t start = in->at;
    while (in->at < in->size && in->text[in->at] >= '0' && in->text[in->at] <= '9') {
        in->at++;
    }
    return in->at > start || fail(in, JSON_READ_MALFORMED);
}

static bool read_number(reader *in) {
    size_t start = in->at;
    if (in->text[in->at] == '-') {
        in->at++;
    }
    if (in->at < in->size && in->text[in->at] == '0') {
        in->at++;
    } else if (!decimals(in)) {
        return false;
    }
    if (in->at < in->size && in->text[in->at] == '.') {
        in->at++;
        if (!decimals(in)) {
            return false;
        }
    }
    if (in->at < in->size && (in->text[in->at] == 'e' || in->text[in->at] == 'E')) {
        in->at++;
        if (in->at < in->size && (in->text[in->at] == '+' || in->text[in->at] == '-')) {
            in->at++;
        }
        if (!decimals(in)) {
            return false;
        }
    }
    size_t index = push(in, JSON_NUMBER);
    if (index == SIZE_MAX || !put(in, in->text + start, in->at - start)) {
        return false;
    }
    in->tree->values[index].size = in->at - start;
    return true;
}

static bool read_value(reader *in, unsigned depth);

/* An array or an object at its opening bracket. */
static bool read_container(reader *in, unsigned depth, json_kind kind) {
    if (depth >= JSON_DEPTH) {
        return fail(in, JSON_READ_MALFORMED);
    }
    size_t index = push(in, kind);
    if (index == SIZE_MAX) {
        return false;
    }
    uint8_t close = kind == JSON_ARRAY ? ']' : '}';
    in->at++;
    skip_space(in);
    if (in->at < in->size && in->text[in->at] == close) {
        in->at++;
        return true;
    }
    for (;;) {
        if (kind == JSON_OBJECT) {
            skip_space(in);
            if (in->at == in->size || in->text[in->at] != '"') {
                return fail(in, JSON_READ_MALFORMED);
            }
            if (!read_string(in)) {
                return false;
            }
            skip_space(in);
            if (in->at == in->size || in->text[in->at] != ':') {
                return fail(in, JSON_READ_MALFORMED);
            }
            in->at++;
        }
        if (!read_value(in, depth + 1)) {
            return false;
        }
        skip_space(in);
        if (in->at == in->size) {
            return fail(in, JSON_READ_MALFORMED);
        }
        if (in->text[in->at] == close) {
            in->at++;
            break;
        }
        if (in->text[in->at] != ',') {
            return fail(in, JSON_READ_MALFORMED);
        }
        in->at++;
    }
    in->tree->values[index].end = in->tree->count;
    return true;
}

static bool read_value(reader *in, unsigned depth) {
    skip_space(in);
    if (in->at == in->size) {
        return fail(in, JSON_READ_MALFORMED);
    }
    switch (in->text[in->at]) {
    case '{':
        return read_container(in, depth, JSON_OBJECT);
    case '[':
        return read_container(in, depth, JSON_ARRAY);
    case '"':
        return read_string(in);
    case 't':
        return literal(in, "true") && push(in, JSON_TRUE) != SIZE_MAX;
    case 'f':
        return literal(in, "false") && push(in, JSON_FALSE) != SIZE_MAX;
    case 'n':
        return literal(in, "null") && push(in, JSON_NULL) != SIZE_MAX;
    default:
        if (in->text[in->at] == '-' || (in->text[in->at] >= '0' && in->text[in->at] <= '9')) {
            return read_number(in);
        }
        return fail(in, JSON_READ_MALFORMED);
    }
}

json_read_status json_read(json_tree *tree, const uint8_t *text, size_t size) {
    reader in = {tree, text, size, 0, JSON_READ_OK};
    tree->count = 0;
    tree->size = 0;
    if (read_value(&in, 0)) {
        skip_space(&in);
        if (in.at != size) {
            fail(&in, JSON_READ_MALFORMED);
        }
    }
    return in.status;
}

void json_tree_release(json_tree *tree) {
    free(tree->values);
    free(tree->bytes);
    memset(tree, 0, sizeof(*tree));
}

const json_value *json_root(const json_tree *tree) { return tree->values; }

const json_value *json_first(const json_tree *tree, const json_value *value) {
    size_t index = (size_t)(value - tree->values);
    return value->end > index + 1 ? value + 1 : NULL;
}

const json_value *json_next(const json_tree *tree, const json_value *container, const json_value *value) {
    return value->end < container->end ? tree->values + value->end : NULL;
}

const json_value *json_member(const json_tree *tree, const json_value *object, const char *key) {
    if (!object || object->kind != JSON_OBJECT) {
        return NULL;
    }
    for (const json_value *name = json_first(tree, object); name; name = json_next(tree, object, name + 1)) {
        if (json_is(tree, name, key)) {
            return name + 1;
        }
    }
    return NULL;
}

const uint8_t *json_bytes(const json_tree *tree, const json_value *value) { return tree->bytes + value->at; }

bool json_is(const json_tree *tree, const json_value *value, const char *text) {
    size_t size = strlen(text);
    return value && value->kind == JSON_STRING && value->size == size &&
           (size == 0 || memcmp(json_bytes(tree, value), text, size) == 0);
}

bool json_integer(const json_tree *tree, const json_value *value, int64_t min, int64_t max, int64_t *integer) {
    if (!value || value->kind != JSON_NUMBER) {
        return false;
    }
    const uint8_t *bytes = json_bytes(tree, value);
    size_t i = bytes[0] == '-' ? 1 : 0;
    /* Accumulate negatively, so INT64_MIN reads. */
    int64_t total = 0;
    for (; i < value->size; i++) {
        if (bytes[i] < '0' || bytes[i] > '9') {
            return false;
        }
        int digit = bytes[i] - '0';
        if (total < (INT64_MIN + digit) / 10) {
            return false;
        }
        total = total * 10 - digit;
    }
    if (bytes[0] != '-') {
        if (total == INT64_MIN) {
            return false;
        }
        total = -total;
    }
    if (total < min || total > max) {
        return false;
    }
    *integer = total;
    return true;
}

static size_t member_count(const json_tree *tree, const json_value *container) {
    size_t count = 0;
    for (const json_value *member = json_first(tree, container); member;
         member = json_next(tree, container, container->kind == JSON_OBJECT ? member + 1 : member)) {
        count++;
    }
    return count;
}

bool json_equal(const json_tree *left_tree, const json_value *left, const json_tree *right_tree,
                const json_value *right) {
    if (left->kind != right->kind) {
        return false;
    }
    switch (left->kind) {
    case JSON_NUMBER:
    case JSON_STRING:
        return left->size == right->size &&
               (left->size == 0 || memcmp(json_bytes(left_tree, left), json_bytes(right_tree, right), left->size) == 0);
    case JSON_ARRAY: {
        const json_value *a = json_first(left_tree, left), *b = json_first(right_tree, right);
        while (a && b) {
            if (!json_equal(left_tree, a, right_tree, b)) {
                return false;
            }
            a = json_next(left_tree, left, a);
            b = json_next(right_tree, right, b);
        }
        return !a && !b;
    }
    case JSON_OBJECT:
        if (member_count(left_tree, left) != member_count(right_tree, right)) {
            return false;
        }
        for (const json_value *name = json_first(left_tree, left); name; name = json_next(left_tree, left, name + 1)) {
            /* The key is looked up in the left tree's bytes against the right
             * tree's names. */
            const json_value *found = NULL;
            for (const json_value *other = json_first(right_tree, right); other;
                 other = json_next(right_tree, right, other + 1)) {
                if (other->size == name->size &&
                    (name->size == 0 ||
                     memcmp(json_bytes(right_tree, other), json_bytes(left_tree, name), name->size) == 0)) {
                    found = other + 1;
                    break;
                }
            }
            if (!found || !json_equal(left_tree, name + 1, right_tree, found)) {
                return false;
            }
        }
        return true;
    default:
        return true;
    }
}

static void reserve_out(json_out *out, size_t size) {
    if (!out->failed && !widen((void **)&out->data, &out->capacity, out->size + size, 1)) {
        out->failed = true;
    }
}

void json_out_bytes(json_out *out, const uint8_t *bytes, size_t size) {
    reserve_out(out, size);
    if (out->failed) {
        return;
    }
    if (size) {
        memcpy(out->data + out->size, bytes, size);
    }
    out->size += size;
}

void json_out_raw(json_out *out, const char *text) { json_out_bytes(out, (const uint8_t *)text, strlen(text)); }

void json_out_string(json_out *out, const uint8_t *bytes, size_t size) {
    static const char HEX[] = "0123456789abcdef";
    json_out_raw(out, "\"");
    size_t run = 0;
    for (size_t i = 0; i < size; i++) {
        uint8_t byte = bytes[i];
        if (byte >= 0x20 && byte != '"' && byte != '\\') {
            continue;
        }
        json_out_bytes(out, bytes + run, i - run);
        run = i + 1;
        char escape[7] = {'\\', 0, 0, 0, 0, 0, 0};
        switch (byte) {
        case '"':
        case '\\':
            escape[1] = (char)byte;
            break;
        case '\n':
            escape[1] = 'n';
            break;
        case '\r':
            escape[1] = 'r';
            break;
        case '\t':
            escape[1] = 't';
            break;
        case '\b':
            escape[1] = 'b';
            break;
        case '\f':
            escape[1] = 'f';
            break;
        default:
            escape[1] = 'u';
            escape[2] = '0';
            escape[3] = '0';
            escape[4] = HEX[byte >> 4];
            escape[5] = HEX[byte & 0xF];
            break;
        }
        json_out_raw(out, escape);
    }
    json_out_bytes(out, bytes + run, size - run);
    json_out_raw(out, "\"");
}

void json_out_text(json_out *out, const char *text) { json_out_string(out, (const uint8_t *)text, strlen(text)); }

void json_out_integer(json_out *out, int64_t integer) {
    char spelled[24];
    snprintf(spelled, sizeof(spelled), "%lld", (long long)integer);
    json_out_raw(out, spelled);
}

void json_out_value(json_out *out, const json_tree *tree, const json_value *value) {
    switch (value->kind) {
    case JSON_NULL:
        json_out_raw(out, "null");
        break;
    case JSON_FALSE:
        json_out_raw(out, "false");
        break;
    case JSON_TRUE:
        json_out_raw(out, "true");
        break;
    case JSON_NUMBER:
        json_out_bytes(out, json_bytes(tree, value), value->size);
        break;
    case JSON_STRING:
        json_out_string(out, json_bytes(tree, value), value->size);
        break;
    case JSON_ARRAY:
    case JSON_OBJECT: {
        bool object = value->kind == JSON_OBJECT;
        json_out_raw(out, object ? "{" : "[");
        for (const json_value *member = json_first(tree, value); member;
             member = json_next(tree, value, object ? member + 1 : member)) {
            if (member != json_first(tree, value)) {
                json_out_raw(out, ",");
            }
            json_out_value(out, tree, member);
            if (object) {
                json_out_raw(out, ":");
                json_out_value(out, tree, member + 1);
            }
        }
        json_out_raw(out, object ? "}" : "]");
        break;
    }
    }
}

void json_out_release(json_out *out) {
    free(out->data);
    memset(out, 0, sizeof(*out));
}
