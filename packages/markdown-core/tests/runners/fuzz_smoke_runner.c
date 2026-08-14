/* Deterministic fuzz smoke suite.
 *
 * Feeds fixed corpora and seeded pseudo-random byte streams through the
 * read-only facade: parse, traverse every node and accessor, dump twice
 * (checking dump determinism), and free.  Seeded append scripts additionally
 * drive incremental documents through the shared replay harness
 * (support/append_replay.h), so every append is checked against a one-shot
 * parse and the identity-ledger invariants.  No renderer is involved and no
 * network or random device is read; the same inputs are generated on every
 * run.  Long-running fuzz campaigns stay in the explicit AFL/libFuzzer
 * maintenance tasks (fuzz_document_appends consumes the same script format).
 *
 *   fuzz_smoke_runner [--corpus FILE]... [--generated COUNT]
 *                     [--script FILE]... [--script-generated COUNT]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <markdown_core.h>

#include "append_replay.h"
#include "test_support.h"

static size_t nodes_visited;

static int traverse(const markdown_core_node *node) {
    const markdown_core_node *child;
    markdown_core_scope scope;
    markdown_core_string view;
    markdown_core_optional_bool checked;
    int32_t level;
    bool flag;

    if (!node) {
        return 0;
    }
    nodes_visited++;
    (void)markdown_core_node_get_kind(node);
    (void)markdown_core_node_kind_name(markdown_core_node_get_kind(node));
    scope = markdown_core_node_scope(node);
    if (scope.start.line < 0 || scope.end.line < 0) {
        return -1;
    }
    (void)markdown_core_node_literal(node, &view);
    (void)markdown_core_node_heading_level(node, &level);
    (void)markdown_core_node_list_item_checked(node, &checked);
    (void)markdown_core_node_table_row_is_header(node, &flag);
    for (child = markdown_core_node_get_first_child(node); child; child = markdown_core_node_get_next_sibling(child)) {
        if (traverse(child) != 0) {
            return -1;
        }
    }
    return 0;
}

static int smoke(const uint8_t *bytes, size_t length, const char *label) {
    markdown_core_document *document;
    markdown_core_error *error = NULL;
    uint8_t *first = NULL;
    uint8_t *second = NULL;
    size_t first_length = 0;
    size_t second_length = 0;
    int result = -1;

    document = markdown_core_document_new(mc_sv(bytes, length), NULL, &error);
    if (!document) {
        /* Parse failures must still produce a well-formed error object. */
        if (!error) {
            fprintf(stderr, "%s: parse failed without an error\n", label);
            return -1;
        }
        if (markdown_core_error_get_message(error).length == 0) {
            fprintf(stderr, "%s: parse error carries no diagnostic\n", label);
            markdown_core_error_free(error);
            return -1;
        }
        markdown_core_error_free(error);
        return 0;
    }

    if (traverse(markdown_core_document_root(document)) != 0) {
        fprintf(stderr, "%s: traversal produced an invalid scope\n", label);
        goto done;
    }
    if (!markdown_core_document_dump(document, &first, &first_length, &error) ||
        !markdown_core_document_dump(document, &second, &second_length, &error)) {
        fprintf(stderr, "%s: dump failed\n", label);
        goto done;
    }
    if (first_length != second_length || memcmp(first, second, first_length) != 0) {
        fprintf(stderr, "%s: dump is not deterministic\n", label);
        goto done;
    }
    result = 0;

done:
    markdown_core_dump_free(first);
    markdown_core_dump_free(second);
    markdown_core_document_free(document);
    markdown_core_error_free(error);
    return result;
}

/* Failed replays are counted by their return value; the callback only
 * explains them. */
static void script_report(void *user, const char *context, const char *message) {
    (void)user;
    fprintf(stderr, "FAILED: %s: %s\n", context, message);
}

/* Chunk payloads drawn from this table make generated scripts overwhelmingly
 * more likely to assemble real constructs than uniform bytes would; the
 * uniform half of the generation keeps raw byte-noise covered. */
static const char SCRIPT_TOKENS[] = "\n\n\n `#>-*[]()|:$^~_!\".= abc\r";

/* Multi-byte fragments whose interiors a chunk boundary can land in: CRLF,
 * fence and emphasis markers, task and table markers, and two-, three-, and
 * four-byte UTF-8 sequences. Splitting these mid-fragment is the point. */
static const char *const SCRIPT_FRAGMENTS[] = {
    "\r\n",
    "```",
    "~~~",
    "**",
    "~~",
    "- [x] ",
    "| --- |",
    "[^a]:",
    "\xC3\xA9",         /* U+00E9, two bytes */
    "\xE2\x82\xAC",     /* U+20AC, three bytes */
    "\xF0\x9F\x98\x80", /* U+1F600, four bytes */
};

/* Emits the append-script format er_script_replay interprets
 * (support/append_replay.h): two option bytes, then len8-prefixed literal
 * chunks. The content is synthesized first — markdown-ish tokens and
 * multi-byte fragments in token mode, raw byte noise otherwise — and then
 * partitioned with adversarial chunk lengths: empty and one-byte chunks are
 * frequent, so boundaries land inside multi-byte UTF-8 sequences and inside
 * markers by construction. Deterministic: the same prng state always yields
 * the same script. */
static uint8_t *script_generate(ts_prng *prng, size_t *length, int tokens) {
    size_t target = 64 + (size_t)(ts_prng_next(prng) % 448);
    size_t longest_fragment = 8;
    uint8_t *content = (uint8_t *)malloc(target + longest_fragment);
    uint8_t *script;
    size_t content_length = 0;
    size_t at = 0;
    size_t offset = 0;
    size_t empty_chunks_left = 8;

    if (!content) {
        return NULL;
    }
    while (content_length < target) {
        uint64_t roll = ts_prng_next(prng);
        if (tokens && roll % 4 == 0) {
            const char *fragment =
                SCRIPT_FRAGMENTS[ts_prng_next(prng) % (sizeof(SCRIPT_FRAGMENTS) / sizeof(SCRIPT_FRAGMENTS[0]))];
            size_t fragment_length = strlen(fragment);
            memcpy(content + content_length, fragment, fragment_length);
            content_length += fragment_length;
        } else if (tokens) {
            content[content_length++] = (uint8_t)SCRIPT_TOKENS[ts_prng_next(prng) % (sizeof(SCRIPT_TOKENS) - 1)];
        } else {
            content[content_length++] = (uint8_t)roll;
        }
    }

    /* Worst case: every chunk is one byte (two script bytes each), plus the
     * option bytes and the bounded run of empty chunks. */
    script = (uint8_t *)malloc(2 + 2 * content_length + empty_chunks_left);
    if (!script) {
        free(content);
        return NULL;
    }
    script[at++] = (uint8_t)ts_prng_next(prng);
    script[at++] = (uint8_t)ts_prng_next(prng);
    while (offset < content_length) {
        size_t chunk;
        switch (ts_prng_next(prng) % 8) {
        case 0: /* the empty append, still a verified mutation */
            if (empty_chunks_left > 0) {
                empty_chunks_left--;
                script[at++] = 0;
            }
            continue;
        case 1:
        case 2: /* one byte: guaranteed mid-sequence, mid-marker splits */
            chunk = 1;
            break;
        default:
            chunk = 2 + (size_t)(ts_prng_next(prng) % 46);
            break;
        }
        if (chunk > content_length - offset) {
            chunk = content_length - offset;
        }
        script[at++] = (uint8_t)chunk;
        memcpy(script + at, content + offset, chunk);
        at += chunk;
        offset += chunk;
    }
    free(content);
    *length = at;
    return script;
}

/* fuzz_appends (streaming plan §7): random byte soups streamed through the
 * REAL append mutation at random, deliberately arbitrary splits — mid-UTF-8,
 * mid-CRLF, mid-anything — with the shared harness's full per-mutation
 * verification (double walk + dump against a one-shot parse) at every step.
 * The soup mixes markdown-ish tokens with raw words so definitions, fences,
 * and tables actually form and dissolve mid-stream. */
static int append_stream(ts_prng *prng, int round, const char *label) {
    er_replay replay;
    markdown_core_parse_options options;
    size_t total = 64 + (size_t)(ts_prng_next(prng) % 2048);
    uint8_t *soup = (uint8_t *)malloc(total + 1);
    size_t at = 0;
    size_t offset = 0;
    int result = -1;

    if (!soup) {
        return -1;
    }
    while (at < total) {
        if (ts_prng_next(prng) % 3 == 0) {
            uint64_t word = ts_prng_next(prng);
            size_t remaining = total - at < 8 ? total - at : 8;
            memcpy(soup + at, &word, remaining);
            at += remaining;
        } else {
            size_t pick = (size_t)(ts_prng_next(prng) % (sizeof(SCRIPT_TOKENS) - 1));
            soup[at++] = (uint8_t)SCRIPT_TOKENS[pick];
        }
    }
    soup[total] = 0;

    markdown_core_parse_options_init(&options);
    /* Round parity toggles footnotes+tables so both option worlds stream. */
    options.footnotes = round % 2 == 0;
    options.tables = round % 2 == 0;
    if (er_replay_open(&replay, label, &options, script_report, NULL) != 0) {
        er_replay_close(&replay);
        free(soup);
        return -1;
    }
    while (offset < total) {
        size_t step = 1 + (size_t)(ts_prng_next(prng) % 24);
        if (step > total - offset) {
            step = total - offset;
        }
        if (er_replay_append(&replay, soup + offset, step) != 0) {
            goto done;
        }
        offset += step;
    }
    result = 0;
done:
    er_replay_close(&replay);
    free(soup);
    return result;
}

int main(int argc, char **argv) {
    int i;
    size_t generated = 256;
    size_t script_generated = 0;
    size_t append_streams = 0;
    size_t failures = 0;
    ts_prng prng;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--corpus") == 0 && i + 1 < argc) {
            const char *path = argv[++i];
            size_t length = 0;
            uint8_t *bytes = ts_read_file(path, &length);
            if (!bytes) {
                fprintf(stderr, "cannot read corpus file: %s\n", path);
                failures++;
                continue;
            }
            if (smoke(bytes, length, path) != 0) {
                failures++;
            }
            free(bytes);
        } else if (strcmp(argv[i], "--generated") == 0 && i + 1 < argc) {
            generated = (size_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) {
            const char *path = argv[++i];
            size_t length = 0;
            uint8_t *bytes = ts_read_file(path, &length);
            if (!bytes) {
                fprintf(stderr, "cannot read script file: %s\n", path);
                failures++;
                continue;
            }
            if (er_script_replay(bytes, length, path, script_report, NULL) != 0) {
                failures++;
            }
            free(bytes);
        } else if (strcmp(argv[i], "--script-generated") == 0 && i + 1 < argc) {
            script_generated = (size_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--append-streams") == 0 && i + 1 < argc) {
            append_streams = (size_t)atoi(argv[++i]);
        } else {
            fputs(
                "usage: fuzz_smoke_runner [--corpus FILE]... [--generated COUNT]"
                " [--script FILE]... [--script-generated COUNT] [--append-streams COUNT]\n",
                stderr
            );
            return 2;
        }
    }

    ts_prng_seed(&prng, UINT64_C(0x6D61726B646F776E)); /* "markdown" */
    for (i = 0; (size_t)i < generated; i++) {
        char label[64];
        size_t length = (size_t)(ts_prng_next(&prng) % 8192);
        uint8_t *bytes = (uint8_t *)malloc(length + 1);
        size_t offset;
        if (!bytes) {
            failures++;
            break;
        }
        for (offset = 0; offset < length; offset += 8) {
            uint64_t word = ts_prng_next(&prng);
            size_t remaining = length - offset < 8 ? length - offset : 8;
            memcpy(bytes + offset, &word, remaining);
        }
        bytes[length] = 0;
        snprintf(label, sizeof(label), "generated[%d]", i);
        if (smoke(bytes, length, label) != 0) {
            failures++;
        }
        free(bytes);
    }

    for (i = 0; (size_t)i < script_generated; i++) {
        char label[64];
        size_t length = 0;
        /* Alternate token-biased and uniform payloads. */
        uint8_t *script = script_generate(&prng, &length, i % 2 == 0);
        if (!script) {
            failures++;
            break;
        }
        snprintf(label, sizeof(label), "script[%d]", i);
        if (er_script_replay(script, length, label, script_report, NULL) != 0) {
            failures++;
        }
        free(script);
    }

    for (i = 0; (size_t)i < append_streams; i++) {
        char label[64];
        snprintf(label, sizeof(label), "append-stream[%d]", i);
        if (append_stream(&prng, i, label) != 0) {
            failures++;
        }
    }

    if (failures) {
        fprintf(stderr, "%zu fuzz smoke input(s) failed\n", failures);
        return 1;
    }
    printf("fuzz smoke passed; %zu nodes traversed across all inputs\n", nodes_visited);
    return 0;
}
