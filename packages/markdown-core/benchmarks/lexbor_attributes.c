/* lexbor's side of the attribute comparison.
 *
 * THE POINT OF AN EXTERNAL BASELINE is that no Markdown implementation builds
 * an attribute map, so nothing in the stage benchmark's two references does
 * the job `elements/attributes.c` does. An HTML start tag's attribute list
 * does: it is a bracketed run split into names and values, with quoting,
 * character references and an identifier and class run carried as attributes
 * of their own. lexbor is a C implementation of that, written for speed, so
 * the ratio against it is what this grammar costs against a fast one rather
 * than against a parser that does not have the feature.
 *
 * THE TOKENIZER, NOT THE PARSER. `lxb_html_parse` would build a document, a
 * DOM tree and interned elements, none of which has a counterpart on this
 * side, and the ratio would be against that instead. The tokenizer stops
 * exactly where the comparison does: tokens carrying name/value pairs.
 *
 * The identifier and the class run are read out of `id` and `class` because
 * that is where the isomorphism puts them -- `{#lane .stage}` and
 * `id="lane" class="stage"` are the same information -- and the class run is
 * folded as one value rather than split, so neither side is charged for a
 * pass the other does not make.
 */
#include <string.h>

#include "lexbor/html/tokenizer.h"

#include "attribute_runner.h"

const char *bench_baseline_name(void) { return "lexbor"; }

typedef struct {
    attribute_receipt *receipt;
    FILE *census;
} recovery;

static void write_value(FILE *census, const lxb_char_t *bytes, size_t length) {
    if (bytes && length > 0) {
        fwrite(bytes, 1, length, census);
    }
}

static lxb_html_token_t *on_token(lxb_html_tokenizer_t *tokenizer, lxb_html_token_t *token, void *context) {
    recovery *state = (recovery *)context;
    lxb_html_token_attr_t *identifier = NULL;
    lxb_html_token_attr_t *classes = NULL;
    lxb_html_token_attr_t *attribute;
    size_t records = 0;

    (void)tokenizer;
    if (!token->attr_first) {
        return token;
    }
    for (attribute = token->attr_first; attribute; attribute = attribute->next) {
        size_t length = 0;
        const lxb_char_t *name = lxb_html_token_attr_name(attribute, &length);

        if (name && length == 2 && memcmp(name, "id", 2) == 0) {
            identifier = attribute;
        } else if (name && length == 5 && memcmp(name, "class", 5) == 0) {
            classes = attribute;
        } else {
            records++;
        }
    }
    if (state->census) {
        fprintf(state->census, "list %zu id=", state->receipt->lists);
        if (identifier) {
            write_value(state->census, identifier->value, identifier->value_size);
        }
        fputs(" class=", state->census);
        if (classes) {
            write_value(state->census, classes->value, classes->value_size);
        }
        for (attribute = token->attr_first; attribute; attribute = attribute->next) {
            size_t length = 0;
            const lxb_char_t *name = lxb_html_token_attr_name(attribute, &length);

            if (attribute == identifier || attribute == classes) {
                continue;
            }
            fputc(' ', state->census);
            write_value(state->census, name, length);
            fputc('=', state->census);
            write_value(state->census, attribute->value, attribute->value_size);
        }
        fputc('\n', state->census);
    }
    state->receipt->lists++;
    state->receipt->values += 2 + records;
    return token;
}

int bench_parse_attributes(const char *source, size_t length, attribute_receipt *receipt, FILE *census) {
    recovery state = {receipt, census};
    lxb_html_tokenizer_t *tokenizer = lxb_html_tokenizer_create();
    lxb_status_t status;

    if (!tokenizer) {
        return 1;
    }
    if (lxb_html_tokenizer_init(tokenizer) != LXB_STATUS_OK) {
        lxb_html_tokenizer_destroy(tokenizer);
        return 1;
    }
    lxb_html_tokenizer_callback_token_done_set(tokenizer, on_token, &state);
    status = lxb_html_tokenizer_begin(tokenizer);
    if (status == LXB_STATUS_OK) {
        status = lxb_html_tokenizer_chunk(tokenizer, (const lxb_char_t *)source, length);
    }
    if (status == LXB_STATUS_OK) {
        status = lxb_html_tokenizer_end(tokenizer);
    }
    lxb_html_tokenizer_destroy(tokenizer);
    return status == LXB_STATUS_OK ? 0 : 1;
}
