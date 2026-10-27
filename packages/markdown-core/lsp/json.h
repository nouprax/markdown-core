#ifndef MARKDOWN_CORE_LSP_JSON_H
#define MARKDOWN_CORE_LSP_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * JSON (RFC 8259), read and written in C (plan, D8): the protocol's messages
 * for the adapter, and later a CSL JSON bibliography for the engine.
 *
 * A READ text becomes a TREE: its values in an array in document order,
 * each container followed by its members and each object member by its key
 * then its value, so a value's first member is the value after it and
 * `end` is the index after its last descendant. A string's bytes are
 * decoded, escapes resolved, into the tree's own bytes; a lone surrogate
 * escape becomes U+FFFD, which counts as one UTF-16 unit as the surrogate
 * did. A number keeps its spelling there. The tree owns everything it
 * hands out.
 */

typedef enum json_kind {
    JSON_NULL = 0,
    JSON_FALSE = 1,
    JSON_TRUE = 2,
    JSON_NUMBER = 3,
    JSON_STRING = 4,
    JSON_ARRAY = 5,
    JSON_OBJECT = 6
} json_kind;

typedef struct json_value {
    json_kind kind;
    /* The index after the value's last descendant. */
    size_t end;
    /* A string's decoded bytes or a number's spelling, in the tree's bytes. */
    size_t at;
    size_t size;
} json_value;

typedef struct json_tree {
    json_value *values;
    size_t count;
    size_t capacity;
    uint8_t *bytes;
    size_t size;
    size_t room;
} json_tree;

typedef enum json_read_status { JSON_READ_OK = 0, JSON_READ_MALFORMED = 1, JSON_READ_FAILED = 2 } json_read_status;

/* Reads `size` bytes of `text` into `tree`, replacing what it held. MALFORMED
 * when the text is not one JSON value, or nests deeper than 512 levels;
 * FAILED when an allocation fails. */
json_read_status json_read(json_tree *tree, const uint8_t *text, size_t size);
void json_tree_release(json_tree *tree);

/* The tree's root, after a read that answered OK. */
const json_value *json_root(const json_tree *tree);
/* A container's first member, an object's first key, or NULL when it has
 * none. */
const json_value *json_first(const json_tree *tree, const json_value *value);
/* The value after `value` and its descendants among its container's
 * members, or NULL after the last; `container` holds it. */
const json_value *json_next(const json_tree *tree, const json_value *container, const json_value *value);
/* The value of `object`'s first member named `key`, or NULL when it has none
 * or `object` is NULL or not an object. */
const json_value *json_member(const json_tree *tree, const json_value *object, const char *key);
/* A string's or a number's bytes. */
const uint8_t *json_bytes(const json_tree *tree, const json_value *value);
/* Whether `value` is a string equal to `text`. */
bool json_is(const json_tree *tree, const json_value *value, const char *text);
/* Whether `value` is a number written as an integer, with no fraction or
 * exponent, in [min, max]; `*integer` receives it. */
bool json_integer(const json_tree *tree, const json_value *value, int64_t min, int64_t max, int64_t *integer);
/* Whether two values are equal: the same kind, the same bytes, the same
 * members in order for an array, and the same members in any order for an
 * object whose keys are distinct. */
bool json_equal(const json_tree *left_tree, const json_value *left, const json_tree *right_tree,
                const json_value *right);

/* A text being WRITTEN; `failed` says an allocation failed, and every write
 * after it does nothing. */
typedef struct json_out {
    uint8_t *data;
    size_t size;
    size_t capacity;
    bool failed;
} json_out;

void json_out_raw(json_out *out, const char *text);
void json_out_bytes(json_out *out, const uint8_t *bytes, size_t size);
/* A string: `size` bytes of `bytes`, quoted and escaped. */
void json_out_string(json_out *out, const uint8_t *bytes, size_t size);
void json_out_text(json_out *out, const char *text);
void json_out_integer(json_out *out, int64_t integer);
/* Writes `value` of `tree` as it reads, in its own spelling. */
void json_out_value(json_out *out, const json_tree *tree, const json_value *value);
void json_out_release(json_out *out);

#endif
