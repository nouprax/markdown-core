/* The incremental correctness runner (docs/plans/2026-09-29-incremental-gates.md,
 * sections 4 and 8).
 *
 * It drives the `session` subject through every workload of the tracked
 * correctness set, `specs/incremental/`, in both coordinate units, and checks
 * the oracles of incremental_oracles.h after the open and after every step.
 *
 *   incremental_runner --set DIR [--family F]  check the `session` subject
 *                                             on every case, or on the cases
 *                                             of one declared family
 *   incremental_runner --set DIR --self-test   require every faulty subject
 *                                             to fail with its oracle
 *   incremental_runner --fuzz COUNT FILE...    check COUNT seeded scripts on
 *                                             each document, as the session
 *                                             fuzz target decodes them
 *
 * The manifest declares the set's families, and every run checks that each
 * case belongs to a declared family and each declared family has a case, so a
 * family is never left out of the tests that run one family each.
 *
 * The faulty subjects (section 8) live here and nowhere else: they are never
 * linked into a product target.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <markdown_core.h>

#include "incremental_oracles.h"
#include "test_support.h"

static char *join_path(const char *directory, const char *relative) {
    size_t length = strlen(directory) + strlen(relative) + 2;
    char *path = (char *)malloc(length);
    if (path) {
        snprintf(path, length, "%s/%s", directory, relative);
    }
    return path;
}

/* Whether a case of `family` is this run's. The family is marked as seen
 * whatever the answer. */
static bool wanted(run *state, const eh_manifest *manifest, bool *seen, const char *where, const char *family) {
    size_t index = eh_family_index(manifest, family);
    if (index == manifest->family_count) {
        fail(state, "harness", "%s: family %s is not declared", where, family);
        return false;
    }
    seen[index] = true;
    if (state->family && strcmp(state->family, family) != 0) {
        return false;
    }
    state->checked++;
    return true;
}

static void run_set(run *state, const char *directory) {
    char *manifest_path = join_path(directory, "manifest.txt");
    char *sizes_path = join_path(directory, "token-sizes.txt");
    eh_manifest manifest;
    eh_sizes tokens;
    bool *seen = NULL, ready;
    size_t index;
    if (!manifest_path || !sizes_path || !eh_manifest_load(manifest_path, &manifest)) {
        fail(state, "harness", "%s: cannot load the manifest", directory);
        free(manifest_path);
        free(sizes_path);
        return;
    }
    if (!eh_sizes_load(sizes_path, &tokens)) {
        fail(state, "harness", "%s: cannot load the token sizes", directory);
        eh_manifest_free(&manifest);
        free(manifest_path);
        free(sizes_path);
        return;
    }
    seen = (bool *)calloc(manifest.family_count + 1, sizeof(*seen));
    ready = seen && (!state->family || eh_family_index(&manifest, state->family) < manifest.family_count);
    if (!ready) {
        fail(state, "harness", "%s: %s", directory,
             seen ? "the manifest does not declare the family" : "out of memory");
    }
    for (index = 0; ready && index < manifest.count && !stopped(state); index++) {
        const eh_case *entry = &manifest.cases[index];
        char *document_path;
        size_t length = 0;
        uint8_t *document;
        char where[400];
        eh_unit unit;
        /* A script file holds scripts of several families; every other case
         * is of one. */
        if (entry->kind != EH_CASE_EDITS && !wanted(state, &manifest, seen, entry->document, eh_case_family(entry))) {
            continue;
        }
        document_path = join_path(directory, entry->document);
        document = document_path ? eh_read_file(document_path, &length) : NULL;
        if (!document) {
            fail(state, "harness", "%s: cannot read", entry->document);
            free(document_path);
            continue;
        }
        if (entry->kind == EH_CASE_DOCUMENT) {
            if (entry->part_count) {
                check_parts(state, entry->document, document, length, entry);
            }
        } else if (entry->kind == EH_CASE_EDITS) {
            char *script_path = join_path(directory, entry->script);
            eh_scripts scripts;
            size_t which;
            if (!script_path || !eh_scripts_load(script_path, &scripts)) {
                fail(state, "harness", "%s: cannot load", entry->script);
            } else {
                for (which = 0; which < scripts.count && !stopped(state); which++) {
                    if (!wanted(state, &manifest, seen, entry->script, scripts.scripts[which].family)) {
                        continue;
                    }
                    for (unit = EH_UTF8; unit <= EH_UTF16 && !stopped(state); unit = (eh_unit)(unit + 1)) {
                        snprintf(where, sizeof(where), "%s %s (%s)", entry->script, scripts.scripts[which].name,
                                 eh_unit_name(unit));
                        run_edits(state, where, unit, document, length, &scripts.scripts[which]);
                    }
                }
                eh_scripts_free(&scripts);
            }
            free(script_path);
        } else if (strcmp(entry->script, "splits") == 0) {
            size_t point;
            for (unit = EH_UTF8; unit <= EH_UTF16 && !stopped(state); unit = (eh_unit)(unit + 1)) {
                for (point = 1; point < length && !stopped(state); point++) {
                    size_t ends[2];
                    if (!eh_utf8_boundary(document, length, point)) {
                        continue;
                    }
                    ends[0] = point;
                    ends[1] = length;
                    snprintf(where, sizeof(where), "%s splits at %zu (%s)", entry->document, point, eh_unit_name(unit));
                    run_stream(state, where, unit, document, ends, 2);
                }
            }
        } else {
            size_t *ends = NULL, count = 0;
            if (!eh_stream_ends(document, length, entry->script, &tokens, &ends, &count)) {
                fail(state, "harness", "%s: no stream family %s", entry->document, entry->script);
            } else {
                for (unit = EH_UTF8; unit <= EH_UTF16 && !stopped(state); unit = (eh_unit)(unit + 1)) {
                    snprintf(where, sizeof(where), "%s %s (%s)", entry->document, entry->script, eh_unit_name(unit));
                    run_stream(state, where, unit, document, ends, count);
                }
            }
            free(ends);
        }
        free(document);
        free(document_path);
    }
    for (index = 0; ready && index < manifest.family_count && !stopped(state); index++) {
        if (!seen[index]) {
            fail(state, "harness", "%s: family %s has no case", directory, manifest.families[index]);
        }
    }
    if (ready && state->family && !state->checked) {
        fail(state, "harness", "%s: no case of family %s ran", directory, state->family);
    }
    free(seen);
    eh_sizes_free(&tokens);
    eh_manifest_free(&manifest);
    free(manifest_path);
    free(sizes_path);
}

/* --------------------------------------------------------- faulty subjects */

/* Each wraps the session and breaks one rule. `inner` is the session; a
 * document the wrapper makes itself is `made`, released at its next step. */
typedef struct faulty {
    eh_unit unit;
    void *inner, *other;
    size_t steps;
    eh_text text;
    markdown_core_document *made;
} faulty;

static void *faulty_open(eh_unit unit, const uint8_t *text, size_t length, const markdown_core_document **document) {
    faulty *subject = (faulty *)calloc(1, sizeof(*subject));
    if (!subject) {
        return NULL;
    }
    subject->unit = unit;
    subject->inner = eh_session.open(unit, text, length, document);
    if (!subject->inner || !eh_text_assign(&subject->text, text, length)) {
        eh_session.close(subject->inner);
        free(subject);
        return NULL;
    }
    return subject;
}

static void faulty_close(void *handle) {
    faulty *subject = (faulty *)handle;
    if (subject) {
        eh_session.close(subject->inner);
        eh_session.close(subject->other);
        markdown_core_document_free(subject->made);
        eh_text_free(&subject->text);
        free(subject);
    }
}

static const uint8_t *faulty_text(const void *handle, size_t *length) {
    return eh_session.text(((const faulty *)handle)->inner, length);
}

/* Replaces the step's document with a fresh parse of `text`. */
static eh_status faulty_fresh(faulty *subject, const uint8_t *text, size_t length,
                              const markdown_core_document **document) {
    markdown_core_document *fresh = NULL;
    if (markdown_core_document_parse_in(text, length, unit_of(subject->unit), &fresh) != MARKDOWN_CORE_OK) {
        return EH_FAILED;
    }
    markdown_core_document_free(subject->made);
    subject->made = fresh;
    *document = fresh;
    return EH_OK;
}

/* Remembers the session's text before a step, for the faults that need it. */
static bool faulty_remember(faulty *subject) {
    size_t length = 0;
    const uint8_t *text = eh_session.text(subject->inner, &length);
    return text && eh_text_assign(&subject->text, text, length);
}

/* Returns the previous document for one step: its third, in every run. */
static eh_status stale_after(faulty *subject, eh_status status, const markdown_core_document **document) {
    return status == EH_OK && ++subject->steps == 3
               ? faulty_fresh(subject, subject->text.bytes, subject->text.length, document)
               : status;
}

static eh_status stale_edit(void *handle, const eh_edit *edits, size_t count, const markdown_core_document **document) {
    faulty *subject = (faulty *)handle;
    if (!faulty_remember(subject)) {
        return EH_FAILED;
    }
    return stale_after(subject, eh_session.edit(subject->inner, edits, count, document), document);
}

static eh_status stale_append(void *handle, const uint8_t *text, size_t length,
                              const markdown_core_document **document) {
    faulty *subject = (faulty *)handle;
    if (!faulty_remember(subject)) {
        return EH_FAILED;
    }
    return stale_after(subject, eh_session.append(subject->inner, text, length, document), document);
}

static const eh_subject_class stale = {
    "returns the previous document for one step", faulty_open, stale_edit, stale_append, faulty_text, faulty_close};

/* Renumbers every id on every step: a fresh parse of the session's text. */
static eh_status renumber_after(faulty *subject, eh_status status, const markdown_core_document **document) {
    size_t length = 0;
    const uint8_t *text;
    if (status != EH_OK) {
        return status;
    }
    text = eh_session.text(subject->inner, &length);
    return text ? faulty_fresh(subject, text, length, document) : EH_FAILED;
}

static eh_status renumber_edit(void *handle, const eh_edit *edits, size_t count,
                               const markdown_core_document **document) {
    faulty *subject = (faulty *)handle;
    return renumber_after(subject, eh_session.edit(subject->inner, edits, count, document), document);
}

static eh_status renumber_append(void *handle, const uint8_t *text, size_t length,
                                 const markdown_core_document **document) {
    faulty *subject = (faulty *)handle;
    return renumber_after(subject, eh_session.append(subject->inner, text, length, document), document);
}

static const eh_subject_class renumber = {
    "renumbers every id on every step", faulty_open, renumber_edit, renumber_append, faulty_text, faulty_close};

/* Rewrites every node with the same ids: two sessions take every step, and
 * the subject answers with each in turn. */
static void *rewrite_open(eh_unit unit, const uint8_t *text, size_t length, const markdown_core_document **document) {
    faulty *subject = (faulty *)faulty_open(unit, text, length, document);
    const markdown_core_document *twin;
    if (subject && !(subject->other = eh_session.open(unit, text, length, &twin))) {
        faulty_close(subject);
        return NULL;
    }
    return subject;
}

static eh_status rewrite_after(faulty *subject, eh_status first, eh_status second,
                               const markdown_core_document **document, const markdown_core_document *twin) {
    if (first != EH_OK || second != EH_OK) {
        return first != EH_OK ? first : second;
    }
    if (++subject->steps % 2) {
        *document = twin;
    }
    return EH_OK;
}

static eh_status rewrite_edit(void *handle, const eh_edit *edits, size_t count,
                              const markdown_core_document **document) {
    faulty *subject = (faulty *)handle;
    const markdown_core_document *twin = NULL;
    eh_status first = eh_session.edit(subject->inner, edits, count, document);
    eh_status second = eh_session.edit(subject->other, edits, count, &twin);
    return rewrite_after(subject, first, second, document, twin);
}

static eh_status rewrite_append(void *handle, const uint8_t *text, size_t length,
                                const markdown_core_document **document) {
    faulty *subject = (faulty *)handle;
    const markdown_core_document *twin = NULL;
    eh_status first = eh_session.append(subject->inner, text, length, document);
    eh_status second = eh_session.append(subject->other, text, length, &twin);
    return rewrite_after(subject, first, second, document, twin);
}

static const eh_subject_class rewrite = {
    "rewrites every node with the same ids", rewrite_open, rewrite_edit, rewrite_append, faulty_text, faulty_close};

/* Gives an edited title_subject a new id: an edit of one line that starts with `#`
 * replaces the whole line, so none of the title_subject's bytes survive. */
static eh_status title_edit(void *handle, const eh_edit *edits, size_t count, const markdown_core_document **document) {
    faulty *subject = (faulty *)handle;
    size_t start = edits[0].start, end = edits[0].end, first, last;
    eh_text line = {0};
    eh_edit whole;
    eh_status status;
    if (count != 1 || !faulty_remember(subject)) {
        return count != 1 ? eh_session.edit(subject->inner, edits, count, document) : EH_FAILED;
    }
    if (subject->unit == EH_UTF16 &&
        (!eh_utf8_offset(subject->text.bytes, subject->text.length, edits[0].start, &start) ||
         !eh_utf8_offset(subject->text.bytes, subject->text.length, edits[0].end, &end))) {
        return eh_session.edit(subject->inner, edits, count, document);
    }
    for (first = start; first > 0 && subject->text.bytes[first - 1] != '\n'; first--) {
    }
    for (last = end; last < subject->text.length && subject->text.bytes[last] != '\n'; last++) {
    }
    if (end > subject->text.length || first == start || subject->text.bytes[first] != '#' ||
        memchr(subject->text.bytes + start, '\n', end - start)) {
        return eh_session.edit(subject->inner, edits, count, document);
    }
    if (!eh_text_assign(&line, subject->text.bytes + first, start - first) ||
        !eh_text_replace(&line, line.length, line.length, edits[0].text, edits[0].length) ||
        !eh_text_replace(&line, line.length, line.length, subject->text.bytes + end, last - end)) {
        eh_text_free(&line);
        return EH_FAILED;
    }
    whole = (eh_edit){first, last, line.bytes, line.length};
    if (subject->unit == EH_UTF16) {
        whole.start = eh_utf16_units(subject->text.bytes, first);
        whole.end = eh_utf16_units(subject->text.bytes, last);
    }
    status = eh_session.edit(subject->inner, &whole, 1, document);
    eh_text_free(&line);
    return status;
}

static eh_status pass_append(void *handle, const uint8_t *text, size_t length,
                             const markdown_core_document **document) {
    return eh_session.append(((faulty *)handle)->inner, text, length, document);
}

static const eh_subject_class title_subject = {
    "gives an edited title_subject a new id", faulty_open, title_edit, pass_append, faulty_text, faulty_close};

/* Keeps a node whose text changed: a second session takes every step but
 * the second the first accepts, and the subject answers with its document
 * while it reports the first's text. */
typedef struct keep_subject {
    faulty base;
    const markdown_core_document *kept;
} keep_subject;

static void *keep_open(eh_unit unit, const uint8_t *text, size_t length, const markdown_core_document **document) {
    keep_subject *subject = (keep_subject *)calloc(1, sizeof(*subject));
    faulty *base = subject ? (faulty *)faulty_open(unit, text, length, document) : NULL;
    if (!base || !(base->other = eh_session.open(unit, text, length, &subject->kept))) {
        faulty_close(base);
        free(subject);
        return NULL;
    }
    subject->base = *base;
    free(base);
    return subject;
}

static eh_status keep_after(keep_subject *subject, eh_status status, eh_status kept, const markdown_core_document *twin,
                            const markdown_core_document **document) {
    if (status == EH_OK) {
        if (kept == EH_OK && twin) {
            subject->kept = twin;
        }
        *document = subject->kept;
    }
    return status;
}

static eh_status keep_edit(void *handle, const eh_edit *edits, size_t count, const markdown_core_document **document) {
    keep_subject *subject = (keep_subject *)handle;
    const markdown_core_document *twin = NULL;
    eh_status status = eh_session.edit(subject->base.inner, edits, count, document), kept = EH_OK;
    if (status == EH_OK && ++subject->base.steps != 2) {
        kept = eh_session.edit(subject->base.other, edits, count, &twin);
    }
    return keep_after(subject, status, kept, twin, document);
}

static eh_status keep_append(void *handle, const uint8_t *text, size_t length,
                             const markdown_core_document **document) {
    keep_subject *subject = (keep_subject *)handle;
    const markdown_core_document *twin = NULL;
    eh_status status = eh_session.append(subject->base.inner, text, length, document), kept = EH_OK;
    if (status == EH_OK && ++subject->base.steps != 2) {
        kept = eh_session.append(subject->base.other, text, length, &twin);
    }
    return keep_after(subject, status, kept, twin, document);
}

static const eh_subject_class keep = {
    "keeps a node whose text changed", keep_open, keep_edit, keep_append, faulty_text, faulty_close};

/* Reuses a retired id for a new node: from its second step the subject is a
 * new session on the text, whose ids start again from 1. */
static eh_status revive_after(faulty *subject, eh_status status, const markdown_core_document **document) {
    size_t length = 0;
    const uint8_t *text;
    void *reopened;
    if (status != EH_OK || ++subject->steps != 2) {
        return status;
    }
    text = eh_session.text(subject->inner, &length);
    if (!text || !eh_text_assign(&subject->text, text, length)) {
        return EH_FAILED;
    }
    reopened = eh_session.open(subject->unit, subject->text.bytes, subject->text.length, document);
    if (!reopened) {
        return EH_FAILED;
    }
    eh_session.close(subject->inner);
    subject->inner = reopened;
    return EH_OK;
}

static eh_status revive_edit(void *handle, const eh_edit *edits, size_t count,
                             const markdown_core_document **document) {
    faulty *subject = (faulty *)handle;
    return revive_after(subject, eh_session.edit(subject->inner, edits, count, document), document);
}

static eh_status revive_append(void *handle, const uint8_t *text, size_t length,
                               const markdown_core_document **document) {
    faulty *subject = (faulty *)handle;
    return revive_after(subject, eh_session.append(subject->inner, text, length, document), document);
}

static const eh_subject_class revive = {
    "reuses a retired id for a new node", faulty_open, revive_edit, revive_append, faulty_text, faulty_close};

/* Accepts an offset between the two units of one scalar: in UTF-16 such an
 * offset reaches the session as the end of its scalar. */
static eh_status lenient_edit(void *handle, const eh_edit *edits, size_t count,
                              const markdown_core_document **document) {
    faulty *subject = (faulty *)handle;
    eh_edit *moved = (eh_edit *)malloc((count ? count : 1) * sizeof(*moved));
    eh_status status;
    size_t index, byte;
    if (!moved || !faulty_remember(subject)) {
        free(moved);
        return EH_FAILED;
    }
    for (index = 0; index < count; index++) {
        moved[index] = edits[index];
        if (subject->unit == EH_UTF16) {
            if (!eh_utf8_offset(subject->text.bytes, subject->text.length, moved[index].start, &byte) &&
                moved[index].start < eh_utf16_units(subject->text.bytes, subject->text.length)) {
                moved[index].start++;
            }
            if (!eh_utf8_offset(subject->text.bytes, subject->text.length, moved[index].end, &byte) &&
                moved[index].end < eh_utf16_units(subject->text.bytes, subject->text.length)) {
                moved[index].end++;
            }
        }
    }
    status = eh_session.edit(subject->inner, moved, count, document);
    free(moved);
    return status;
}

static const eh_subject_class lenient = {"accepts an offset between the two units of one scalar",
                                         faulty_open,
                                         lenient_edit,
                                         pass_append,
                                         faulty_text,
                                         faulty_close};

/* ------------------------------------------------------------ unit checks */

/* The text model and the unit conversions, on inputs whose answers are known. */
static int check_harness(void) {
    static const uint8_t text[] = "a\xc3\xa9\xe4\xb8\x80\xf0\xa0\x80\x80z"; /* a é 一 𠀀 z */
    const size_t length = sizeof(text) - 1;
    static const size_t utf16[] = {0, 1, 2, 3, 5, 6};
    static const size_t utf8[] = {0, 1, 3, 6, 10, 11};
    eh_text model = {0};
    size_t index, offset = 0;
    int failures = 0;
    for (index = 0; index < 6; index++) {
        failures += eh_utf16_units(text, utf8[index]) != utf16[index];
        failures += !eh_utf8_offset(text, length, utf16[index], &offset) || offset != utf8[index];
    }
    /* Between the halves of a surrogate pair, and past the end. */
    failures += eh_utf8_offset(text, length, 4, &offset);
    failures += eh_utf8_offset(text, length, 7, &offset);
    failures += eh_utf8_boundary(text, length, 2) || !eh_utf8_boundary(text, length, 3);
    /* Bytes that are no scalar count by the same rule. */
    failures += eh_utf16_units((const uint8_t *)"\x7f\x80\xc3", 3) != 2 ||
                !eh_utf8_offset((const uint8_t *)"\x80\x80"
                                                 "a",
                                3, 1, &offset) ||
                offset != 3;
    failures += !eh_text_assign(&model, (const uint8_t *)"hello", 5) ||
                !eh_text_replace(&model, 1, 4, (const uint8_t *)"EY", 2) || model.length != 4 ||
                memcmp(model.bytes, "hEYo", 4) != 0 || eh_text_replace(&model, 3, 2, NULL, 0) ||
                eh_text_replace(&model, 0, 5, NULL, 0);
    eh_text_free(&model);
    if (failures) {
        fprintf(stderr, "FAILED [harness] the text model or a unit conversion is wrong (%d)\n", failures);
    }
    return failures;
}

/* The fuzz target's decoding (4.11) run deterministically: each document
 * with `count` scripts of seeded bytes. */
static int run_fuzz(size_t count, char **files, int file_count) {
    run state;
    uint64_t seed = 0x9e3779b97f4a7c15u;
    int file;
    memset(&state, 0, sizeof(state));
    state.subject = &eh_session;
    for (file = 0; file < file_count; file++) {
        size_t length, index, at;
        uint8_t *document = ts_read_file(files[file], &length);
        uint8_t *input = document ? (uint8_t *)malloc(length + 4 + 1024) : NULL;
        if (!input) {
            fprintf(stderr, "FAILED: cannot read %s\n", files[file]);
            free(document);
            return 1;
        }
        for (index = 0; index < 4; index++) {
            input[index] = (uint8_t)(length >> (8 * index));
        }
        memcpy(input + 4, document, length);
        for (index = 0; index < count; index++) {
            size_t script = 16 + (size_t)(seed >> 33) % 1008;
            for (at = 0; at < script; at++) {
                seed ^= seed << 13;
                seed ^= seed >> 7;
                seed ^= seed << 17;
                input[4 + length + at] = (uint8_t)(seed >> 24);
            }
            check_script_bytes(&state, input, 4 + length + script);
        }
        free(input);
        free(document);
    }
    if (state.failures) {
        fprintf(stderr, "%zu incremental check(s) failed\n", state.failures);
        return 1;
    }
    printf("incremental fuzz scripts passed: %zu on each of %d document(s)\n", count, file_count);
    return 0;
}

int main(int argc, char **argv) {
    const char *directory = NULL;
    bool self_test = false;
    const char *family = NULL;
    int index;
    if (argc > 3 && strcmp(argv[1], "--fuzz") == 0) {
        return check_harness() ? 1 : run_fuzz((size_t)strtoul(argv[2], NULL, 10), argv + 3, argc - 3);
    }
    for (index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--set") == 0 && index + 1 < argc) {
            directory = argv[++index];
        } else if (strcmp(argv[index], "--self-test") == 0) {
            self_test = true;
        } else if (strcmp(argv[index], "--family") == 0 && index + 1 < argc && !family) {
            family = argv[++index];
        } else {
            directory = NULL;
            break;
        }
    }
    if (!directory || (self_test && family)) {
        fputs("usage: incremental_runner --set DIR [--family F | --self-test] | --fuzz COUNT FILE...\n", stderr);
        return 2;
    }
    if (check_harness()) {
        return 1;
    }
    if (self_test) {
        /* Each faulty subject must fail every oracle its row of section 8
         * names; a subject may fail others on the way. */
        static const struct {
            const eh_subject_class *subject;
            const char *oracle;
        } faulty[] = {{&stale, "4.1"}, {&renumber, "4.2 4.4 4.5"}, {&rewrite, "4.3"}, {&title_subject, "4.4"},
                      {&keep, "4.1"},  {&revive, "4.2"},           {&lenient, "4.8"}};
        int failures = 0;
        size_t which;
        for (which = 0; which < sizeof(faulty) / sizeof(faulty[0]); which++) {
            run state;
            memset(&state, 0, sizeof(state));
            state.subject = faulty[which].subject;
            state.stop_at_failure = true;
            state.required = faulty[which].oracle;
            run_set(&state, directory);
            if (!state.failures || !failed_all(&state)) {
                fprintf(stderr, "FAILED: the subject that %s was not caught by %s, only by %s%s%s\n",
                        faulty[which].subject->name, faulty[which].oracle, state.caught[0] ? state.caught : "nothing",
                        state.failures ? "; first failure: " : "", state.failures ? state.first : "");
                failures++;
            } else {
                printf("ok: the subject that %s fails %s (first: %s)\n", faulty[which].subject->name,
                       faulty[which].oracle, state.first);
            }
        }
        return failures ? 1 : 0;
    }
    {
        run state;
        memset(&state, 0, sizeof(state));
        state.subject = &eh_session;
        state.family = family;
        run_set(&state, directory);
        if (state.failures) {
            fprintf(stderr, "%zu incremental check(s) failed\n", state.failures);
            return 1;
        }
        printf("incremental correctness set passed with the %s subject: %zu case(s)%s%s\n", eh_session.name,
               state.checked, family ? " of family " : "", family ? family : "");
    }
    return 0;
}
