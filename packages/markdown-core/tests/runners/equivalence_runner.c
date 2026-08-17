/* Document equivalence suite: an incrementally appended document must
 * always dump byte-identically to a one-shot parse of the same final text,
 * and its identities must behave — ids never resurrect, revisions never
 * regress, an unchanged (id, revision) means an unchanged projection, an
 * unchanged subtree keeps its revision, and an empty append moves nothing.
 *
 * Every replay drives the public facade only, through the shared append
 * replay harness (support/append_replay.h): a shadow text buffer receives
 * the same bytes as the document, so each append can be checked against
 * markdown_core_document_new of the shadow bytes; a double walk over the
 * predecessor and successor trees checks the identity contract against a
 * cumulative id ledger after every append, which catches adoption bugs
 * that dumps cannot see.
 *
 *   equivalence_runner --list
 *   equivalence_runner --case canonical --fixtures DIR NAME MASK [NAME MASK ...]
 *   equivalence_runner --case spec --spec FILE
 *   equivalence_runner --case regressions
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "append_replay.h"
#include "test_support.h"

#include <markdown_core.h>

/* Per-byte replays are quadratic; keep them for short inputs. */
#define EQ_PER_BYTE_LIMIT 2048
/* Sampling strides keep the spec-wide replays within the suite budget. */
#define EQ_SPEC_PER_BYTE_STRIDE 7
#define EQ_SPEC_RANDOM_STRIDE 31
#define EQ_RANDOM_SEED UINT64_C(0x4d32c0de)

static int failures;

static void eq_fail(const char *context, const char *message) {
    fprintf(stderr, "FAILED: %s: %s\n", context, message);
    failures++;
}

/* Failures inside the shared harness land in the same counter as the
 * runner's own checks. */
static void eq_report(void *user, const char *context, const char *message) {
    (void)user;
    eq_fail(context, message);
}

static int eq_open(er_replay *replay, const char *context, const markdown_core_parse_options *options) {
    return er_replay_open(replay, context, options, eq_report, NULL);
}

/* --- replays over one input --------------------------------------------- */

static int eq_replay_per_line(
    const char *context,
    const uint8_t *text,
    size_t length,
    const markdown_core_parse_options *options
) {
    er_replay replay;
    size_t offset = 0;
    int result = -1;

    if (eq_open(&replay, context, options) != 0) {
        return -1;
    }
    while (offset < length) {
        size_t line_end = offset;
        while (line_end < length && text[line_end] != '\n') {
            line_end++;
        }
        if (line_end < length) {
            line_end++;
        }
        if (er_replay_append(&replay, text + offset, line_end - offset) != 0) {
            goto done;
        }
        offset = line_end;
    }
    /* Empty inputs still must mutate cleanly: the empty append advances
     * the chain and the oracle must hold on identical bytes. */
    if (length == 0 && er_replay_append(&replay, text, 0) != 0) {
        goto done;
    }
    result = 0;
done:
    er_replay_close(&replay);
    return result;
}

/* THE PRIMARY APPEND GATE (streaming plan §7): token-sized strides, 3-8
 * bytes, deliberately NOT line-aligned — the per-line replay is blind to the
 * whole partial-line mechanism class, which is exactly where the warm path
 * will live from P2 on. Deterministically seeded per input. */
static int eq_replay_per_token(
    const char *context,
    const uint8_t *text,
    size_t length,
    const markdown_core_parse_options *options
) {
    er_replay replay;
    ts_prng prng;
    size_t offset = 0;
    int result = -1;

    ts_prng_seed(&prng, UINT64_C(0x70ce55a11d) ^ (uint64_t)length);
    if (eq_open(&replay, context, options) != 0) {
        return -1;
    }
    while (offset < length) {
        size_t step = 3 + (size_t)(ts_prng_next(&prng) % 6);
        if (step > length - offset) {
            step = length - offset;
        }
        if (er_replay_append(&replay, text + offset, step) != 0) {
            goto done;
        }
        offset += step;
    }
    /* The empty append is a mutation too: the chain must advance and the
     * oracle must hold on identical bytes. */
    if (er_replay_append(&replay, text, 0) != 0) {
        goto done;
    }
    result = 0;
done:
    er_replay_close(&replay);
    return result;
}

static int eq_replay_per_byte(
    const char *context,
    const uint8_t *text,
    size_t length,
    const markdown_core_parse_options *options
) {
    er_replay replay;
    size_t offset;
    int result = -1;

    if (eq_open(&replay, context, options) != 0) {
        return -1;
    }
    for (offset = 0; offset < length; offset++) {
        if (er_replay_append(&replay, text + offset, 1) != 0) {
            goto done;
        }
    }
    result = 0;
done:
    er_replay_close(&replay);
    return result;
}

/* --- cases ---------------------------------------------------------------- */

static int parse_option_mask(const char *mask, markdown_core_parse_options *options) {
    bool *fields[] = {
        &options->smart_punctuation,
        &options->footnotes,
        &options->tables,
        &options->strikethrough,
        &options->autolinks,
        &options->task_lists,
        &options->formulas,
        &options->directives,
        &options->cross_links,
        &options->embeds
    };
    size_t i;
    if (strlen(mask) != sizeof(fields) / sizeof(fields[0])) {
        return 0;
    }
    for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (mask[i] != '0' && mask[i] != '1') {
            return 0;
        }
        *fields[i] = mask[i] == '1';
    }
    return 1;
}

static int case_canonical(const char *fixtures_dir, char **names, size_t name_count) {
    size_t i;
    for (i = 0; i < name_count; i += 2) {
        const char *name = names[i];
        const char *mask = names[i + 1];
        char path[1024];
        char context[1100];
        uint8_t *markdown;
        size_t length = 0;
        markdown_core_parse_options options;

        markdown_core_parse_options_init(&options);
        if (!parse_option_mask(mask, &options)) {
            eq_fail(name, "manifest parse option mask is invalid");
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s.md", fixtures_dir, name);
        markdown = ts_read_file(path, &length);
        if (!markdown) {
            eq_fail(name, "fixture is unreadable");
            continue;
        }
        snprintf(context, sizeof(context), "canonical %s per-line", name);
        eq_replay_per_line(context, markdown, length, &options);
        snprintf(context, sizeof(context), "canonical %s per-token", name);
        eq_replay_per_token(context, markdown, length, &options);
        if (length <= EQ_PER_BYTE_LIMIT) {
            snprintf(context, sizeof(context), "canonical %s per-byte", name);
            eq_replay_per_byte(context, markdown, length, &options);
        }
        free(markdown);
    }
    return failures ? -1 : 0;
}

static int case_spec(const char *spec_path) {
    ts_spec_file file;
    size_t i;

    if (ts_spec_load(spec_path, &file) != 0) {
        eq_fail(spec_path, "spec fixture failed to load");
        return -1;
    }
    for (i = 0; i < file.count; i++) {
        ts_spec_case *example = &file.cases[i];
        char context[256];
        markdown_core_parse_options options;
        ts_ast_options_none(&options);

        snprintf(context, sizeof(context), "spec example %d per-line", example->example);
        eq_replay_per_line(context, (const uint8_t *)example->markdown, example->markdown_length, &options);
        snprintf(context, sizeof(context), "spec example %d per-token", example->example);
        eq_replay_per_token(context, (const uint8_t *)example->markdown, example->markdown_length, &options);
        if (i % EQ_SPEC_PER_BYTE_STRIDE == 0 && example->markdown_length <= EQ_PER_BYTE_LIMIT) {
            snprintf(context, sizeof(context), "spec example %d per-byte", example->example);
            eq_replay_per_byte(context, (const uint8_t *)example->markdown, example->markdown_length, &options);
        }
    }
    ts_spec_free(&file);
    return failures ? -1 : 0;
}

/* --- streaming regressions ----------------------------------------------- */

/* Texts the living tree's adversarial reviews found, each once a wrong
 * projection, a lost id, a spurious revision or a read of freed memory, kept
 * here under the same oracle every corpus runs — and additionally with an
 * EMPTY APPEND AFTER EVERY BYTE, since two of them moved a revision only
 * when the tree was asked to stay exactly as it was. The mask is the
 * per-token replay's option string (smart, footnotes, tables, strikethrough,
 * autolinks, task lists, formulas, directives, cross links, embeds). */
static const struct {
    const char *text;
    const char *mask;
    const char *why;
} EQ_REGRESSIONS[] = {
    {"[a]: /1\n> [b]\n\n[b]: /2\nx\n",
     "0000000000",
     "the run behind a vanished leaf was retired unowned and unthreaded, and a definition flipped it after retirement"},
    {"[a]: /1\n> [b]: /2\n",
     "0000000000",
     "the held line closed the leaf (nothing but definitions) and itself yielded a second such paragraph, and the leaf "
     "was second on the parser's list"},
    {"[x]: /y\n> [a]: /u\n\nz\n",
     "0000000000",
     "the same, under a block quote whose first line is a complete definition"},
    {"[x]: /y\n- [a]: /u\n\nz\n", "0000000000", "the same, under a list item"},
    {"$$x$$\np\n", "0000001000", "a paragraph promoted to a formula block kept its pre-promotion inline child"},
    {"$$x$$\n[a]: /u\n[a]\n", "0000001000", "the same, followed by a definition and a mention"},
    {"[a] x\n\n[a]: /1\n$$x$$\n[a]: /u\n[a]\n", "0000001000", "the same, after a settled mention"},
    {"[a] [b] [c]\n\n[a]: /a\n[b]: /b\n[c]: /c\nnot title\n\n[a] [b] [c]\n",
     "0000000000",
     "a definition closing for good paired its units against children the retract had refined and never published"},
    {"```\ncode [a]\n```\n", "0000000000", "an open fence's literal is re-allocated by every close"},
    {"| h | b |\n|---|---|\n\nafter\n", "0010000000", "a held delimiter row re-mints the table's payload every close"},
    {"<div>\nx\n", "0000000000", "an open HTML block's literal is re-allocated by every close"},
    {"[a] first\n\n[a]: /aa \"tt\"\n",
     "0000000000",
     "a definitions-only leaf revived by the retract vanished again and had moved the root"},
    {"$$\n`\nx+y\n$$\n\nafter\n",
     "0000001000",
     "a display formula's literal grew behind a stable payload pointer (PR #105 review)"},
    {"see [foo] and :e{=bad} here\n\n[foo]: /url\n\nmore [foo]\n",
     "0000000100",
     "a flipped unit raised its diagnostic twice"},
    {"[^1] note\n\n[^1]: the footnote\n\nafter\n", "0100000000", "a footnote definition on the spine"},
    {":::note\nbody [q]\n:::\n\n[q]: /q\n",
     "0000000100",
     "a directive container on the spine, flipped by a definition after it"},
    {"[a]: /1\n# Head\n",
     "0000000000",
     "a leaf the close takes again, with a block the held line appended beside it, re-minted that block every tick"},
    {"> [a]: /1\n> # h\n", "0000000000", "the same, under a block quote"},
    {"[a]: /1\n- x\ny\n", "0000000000", "the same, with a list beside it"},
    {"> [a]: /1\n> > [b]: /2\n", "0000000000", "the same, with a nested quote whose first line is a definition"},
    {"[a]: /1\n<div>\nx\n", "0000000000", "the same, with an HTML block beside it"},
    {"[a]: /1\n***\n", "0000000000", "the same, with a thematic break beside it"},
    {"$$x$$\n\nafter\n",
     "0000001000",
     "a leaf promoted to a formula block was re-minted every tick while it stayed open: the survivor is retired, not "
     "freed"},
    {"```formula\nx+y\n\nz\n```\n\nafter\n", "0000001000", "the same, for a formula fence"},
    {"> $$x$$\n\n- $$y$$\n\n- $$z$$\n", "0000001000", "the same, inside a quote and inside list items"},
};

static int eq_replay_per_byte_with_empties(
    const char *context,
    const uint8_t *text,
    size_t length,
    const markdown_core_parse_options *options
) {
    er_replay replay;
    size_t offset;
    int result = -1;

    if (eq_open(&replay, context, options) != 0) {
        return -1;
    }
    for (offset = 0; offset < length; offset++) {
        if (er_replay_append(&replay, text + offset, 1) != 0 || er_replay_append(&replay, text, 0) != 0) {
            goto done;
        }
    }
    result = 0;
done:
    er_replay_close(&replay);
    return result;
}

static int case_regressions(void) {
    size_t i;
    for (i = 0; i < sizeof(EQ_REGRESSIONS) / sizeof(EQ_REGRESSIONS[0]); i++) {
        const uint8_t *text = (const uint8_t *)EQ_REGRESSIONS[i].text;
        size_t length = strlen(EQ_REGRESSIONS[i].text);
        char context[256];
        markdown_core_parse_options options;
        markdown_core_parse_options_init(&options);
        if (!parse_option_mask(EQ_REGRESSIONS[i].mask, &options)) {
            eq_fail(EQ_REGRESSIONS[i].why, "regression option mask is invalid");
            continue;
        }
        snprintf(context, sizeof(context), "regression %zu (%s) per-line", i, EQ_REGRESSIONS[i].why);
        eq_replay_per_line(context, text, length, &options);
        snprintf(context, sizeof(context), "regression %zu (%s) per-token", i, EQ_REGRESSIONS[i].why);
        eq_replay_per_token(context, text, length, &options);
        snprintf(context, sizeof(context), "regression %zu (%s) per-byte", i, EQ_REGRESSIONS[i].why);
        eq_replay_per_byte(context, text, length, &options);
        snprintf(context, sizeof(context), "regression %zu (%s) per-byte with empties", i, EQ_REGRESSIONS[i].why);
        eq_replay_per_byte_with_empties(context, text, length, &options);
    }
    return failures ? -1 : 0;
}

/* --- entry point ---------------------------------------------------------- */

static const char *const EQ_CASES[] = {"canonical", "spec", "regressions"};

int main(int argc, char **argv) {
    const char *case_name = NULL;
    const char *fixtures_dir = NULL;
    const char *spec_path = NULL;
    char **positional = NULL;
    size_t positional_count = 0;
    int list_only = 0;
    int i;
    int result = 2;

    positional = (char **)calloc(argc ? (size_t)argc : 1, sizeof(*positional));
    if (!positional) {
        return 2;
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            list_only = 1;
        } else if (strcmp(argv[i], "--case") == 0 && i + 1 < argc) {
            case_name = argv[++i];
        } else if (strcmp(argv[i], "--fixtures") == 0 && i + 1 < argc) {
            fixtures_dir = argv[++i];
        } else if (strcmp(argv[i], "--spec") == 0 && i + 1 < argc) {
            spec_path = argv[++i];
        } else if (argv[i][0] != '-') {
            positional[positional_count++] = argv[i];
        } else {
            fputs(
                "usage: equivalence_runner [--list] --case NAME [--fixtures DIR NAME MASK ...] [--spec FILE]\n",
                stderr
            );
            goto done;
        }
    }

    if (list_only) {
        size_t c;
        for (c = 0; c < sizeof(EQ_CASES) / sizeof(EQ_CASES[0]); c++) {
            puts(EQ_CASES[c]);
        }
        result = 0;
        goto done;
    }

    if (case_name && strcmp(case_name, "canonical") == 0 && fixtures_dir && positional_count >= 2 &&
        positional_count % 2 == 0) {
        case_canonical(fixtures_dir, positional, positional_count);
    } else if (case_name && strcmp(case_name, "spec") == 0 && spec_path) {
        case_spec(spec_path);
    } else if (case_name && strcmp(case_name, "regressions") == 0) {
        case_regressions();
    } else {
        fputs("usage: equivalence_runner [--list] --case NAME [--fixtures DIR NAME MASK ...] [--spec FILE]\n", stderr);
        goto done;
    }

    printf("equivalence %s [%s]\n", case_name, failures ? "FAILED" : "PASSED");
    result = failures ? 1 : 0;
done:
    free(positional);
    return result;
}
