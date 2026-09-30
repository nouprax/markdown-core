/* The incremental correctness runner (docs/plans/2026-09-29-incremental-gates.md,
 * sections 4 and 8).
 *
 * It drives a subject through every workload of the tracked correctness set,
 * `specs/incremental/`, in both coordinate units, and checks the oracles that
 * apply before sessions exist after every step:
 *
 *   3.1  a composite document's fresh parse has as many root children as its
 *        parts' fresh parses together
 *   4.1  the subject's text equals the harness's text model, and the canonical
 *        dump of its document equals the dump of a fresh parse of that text
 *        (after every chunk of a stream too, 4.6)
 *   4.2  the document's ids number its nodes from 1 in canonical walk order,
 *        and its ids and extents equal a fresh parse's
 *   4.5  every scripted identity expectation names a node of its kind where
 *        it says, so the script means what it states once ids exist
 *   4.7  a batch's dump equals the dump after its edits one at a time, in
 *        descending order of their start offsets
 *   4.8  every invalid argument is rejected
 *
 *   incremental_runner --set DIR [--family F]  check the `reparse` subject
 *                                             on every case, or on the cases
 *                                             of one declared family
 *   incremental_runner --set DIR --self-test   require every faulty subject
 *                                             to fail with its oracle
 *
 * The manifest declares the set's families, and every run checks that each
 * case belongs to a declared family and each declared family has a case, so a
 * family is never left out of the tests that run one family each.
 *
 * The faulty subjects (section 8) live here and nowhere else: they are never
 * linked into a product target.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <markdown_core.h>

#include "edit_harness.h"
#include "test_support.h"

typedef struct run {
    const eh_subject_class *subject;
    /* What failed, by oracle, and the first failure's description. */
    size_t failures;
    const char *first_oracle;
    char first[512];
    /* A self-test run stops at its first failure. */
    bool stop_at_failure;
    /* The family this run checks, or NULL for every family, and how many of
     * its cases ran. */
    const char *family;
    size_t checked;
    const char *label;
} run;

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
static void fail(run *state, const char *oracle, const char *format, ...) {
    char message[512];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (!state->failures) {
        state->first_oracle = oracle;
        memcpy(state->first, message, sizeof(message));
    }
    if (!state->stop_at_failure) {
        fprintf(stderr, "FAILED [%s] %s\n", oracle, message);
    }
    state->failures++;
}

static bool stopped(const run *state) { return state->stop_at_failure && state->failures; }

/* The canonical dump of a document parsed from `source`; the caller frees it. */
static uint8_t *dump_of(const markdown_core_document *document, const uint8_t *source, size_t source_length,
                        size_t *length) {
    uint8_t *output = NULL;
    markdown_core_error *error = NULL;
    if (!markdown_core_document_dump(document, NULL, source, source_length, &output, length, &error)) {
        markdown_core_error_free(error);
        return NULL;
    }
    return output;
}

static uint8_t *dump_text(const uint8_t *bytes, size_t length, size_t *dump_length) {
    markdown_core_document *document = ts_ast_parse(bytes, length);
    uint8_t *dump;
    if (!document) {
        return NULL;
    }
    dump = dump_of(document, bytes, length, dump_length);
    markdown_core_document_free(document);
    return dump;
}

static bool same(const uint8_t *left, size_t left_length, const uint8_t *right, size_t right_length) {
    return left && right && left_length == right_length && memcmp(left, right, left_length) == 0;
}

/* ------------------------------------------------------ identifiers (4.2) */

/* A document's nodes in canonical walk order: kind, id and extent. */
typedef struct {
    markdown_core_node_kind kind;
    uint64_t id;
    markdown_core_extent extent;
} walked_node;

typedef struct {
    walked_node *nodes;
    size_t count, capacity;
} walked;

static int visit_record(const markdown_core_node *node, ts_ast_range range, void *context) {
    walked *list = (walked *)context;
    (void)range;
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 64;
        walked_node *nodes = (walked_node *)realloc(list->nodes, capacity * sizeof(*nodes));
        if (!nodes) {
            return 1;
        }
        list->nodes = nodes;
        list->capacity = capacity;
    }
    list->nodes[list->count++] =
        (walked_node){markdown_core_node_get_kind(node), markdown_core_node_id(node), markdown_core_node_extent(node)};
    return 0;
}

static bool same_node(walked_node left, walked_node right) {
    return left.kind == right.kind && left.id == right.id && left.extent.lead == right.extent.lead &&
           left.extent.span == right.extent.span;
}

static bool walk_document(const markdown_core_document *document, walked *list) {
    return document && ts_ast_walk(markdown_core_document_root(document), visit_record, list) == 0;
}

/* 4.2 for a fresh parse: ids number the nodes from 1 in canonical walk order,
 * so they are unique across every owned relation, and the document equals a
 * second fresh parse of the text, ids included. */
static void check_identifiers(run *state, const char *where, size_t step, const markdown_core_document *document,
                              const markdown_core_document *fresh) {
    walked actual = {0}, expected = {0};
    if (!walk_document(document, &actual) || !walk_document(fresh, &expected)) {
        fail(state, "harness", "%s step %zu: a document could not be walked", where, step);
    } else {
        size_t index;
        for (index = 0; index < actual.count && actual.nodes[index].id == index + 1; index++) {
        }
        if (index < actual.count) {
            fail(state, "4.2", "%s step %zu: node %zu of the canonical walk has id %llu", where, step, index + 1,
                 (unsigned long long)actual.nodes[index].id);
        } else {
            for (index = 0; index < actual.count && index < expected.count &&
                            same_node(actual.nodes[index], expected.nodes[index]);
                 index++) {
            }
            if (actual.count != expected.count || index < actual.count) {
                fail(state, "4.2", "%s step %zu: the document's ids or extents differ from a fresh parse's", where,
                     step);
            }
        }
    }
    free(actual.nodes);
    free(expected.nodes);
}

/* 4.1: the subject's text is the model's, and its document is a fresh parse's;
 * then 4.2 against that same fresh parse. */
static void check_equivalence(run *state, const char *where, size_t step, void *subject,
                              const markdown_core_document *document, const eh_text *model) {
    size_t length = 0;
    const uint8_t *text = state->subject->text(subject, &length);
    size_t actual_length = 0, expected_length = 0;
    uint8_t *actual;
    uint8_t *expected;
    markdown_core_document *fresh;
    if (!same(text, length, model->bytes, model->length) && !(length == 0 && model->length == 0)) {
        fail(state, "4.1", "%s step %zu: the subject's text differs from the text model", where, step);
        return;
    }
    fresh = ts_ast_parse(model->bytes, model->length);
    actual = dump_of(document, text, length, &actual_length);
    expected = fresh ? dump_of(fresh, model->bytes, model->length, &expected_length) : NULL;
    if (!same(actual, actual_length, expected, expected_length)) {
        fail(state, "4.1", "%s step %zu: the document differs from a fresh parse of the text", where, step);
    } else {
        check_identifiers(state, where, step, document, fresh);
    }
    markdown_core_dump_free(actual);
    markdown_core_dump_free(expected);
    markdown_core_document_free(fresh);
}

/* The model applies a batch as the gates define it: every edit against the
 * text before the batch, the pieces between them copied forward. */
static bool model_batch(eh_text *model, const eh_edit *edits, size_t count) {
    eh_text next = {0};
    size_t *order = (size_t *)malloc(count * sizeof(size_t));
    size_t index, at = 0;
    bool ok = order != NULL;
    for (index = 0; ok && index < count; index++) {
        size_t place = index;
        while (place > 0 && edits[order[place - 1]].start > edits[index].start) {
            order[place] = order[place - 1];
            place--;
        }
        order[place] = index;
    }
    ok = ok && eh_text_assign(&next, NULL, 0);
    for (index = 0; ok && index < count; index++) {
        const eh_edit *edit = &edits[order[index]];
        ok = edit->start >= at && edit->end <= model->length &&
             eh_text_replace(&next, next.length, next.length, model->bytes + at, edit->start - at) &&
             eh_text_replace(&next, next.length, next.length, edit->text, edit->length);
        at = edit->end;
    }
    ok = ok && eh_text_replace(&next, next.length, next.length, model->bytes + at, model->length - at);
    free(order);
    if (ok) {
        eh_text_free(model);
        *model = next;
    } else {
        eh_text_free(&next);
    }
    return ok;
}

/* 4.7: the same edits one at a time, in descending order of their starts. */
static void check_batch(run *state, const char *where, size_t step, const markdown_core_document *document,
                        const eh_text *before, const eh_edit *edits, size_t count) {
    eh_text text = {0};
    bool *done = (bool *)calloc(count, sizeof(bool));
    size_t round, actual_length = 0, expected_length = 0;
    uint8_t *actual;
    uint8_t *expected;
    bool ok = done && eh_text_assign(&text, before->bytes, before->length);
    for (round = 0; ok && round < count; round++) {
        size_t index, pick = count;
        for (index = 0; index < count; index++) {
            if (!done[index] && (pick == count || edits[index].start > edits[pick].start)) {
                pick = index;
            }
        }
        done[pick] = true;
        ok = eh_text_replace(&text, edits[pick].start, edits[pick].end, edits[pick].text, edits[pick].length);
    }
    free(done);
    if (!ok) {
        fail(state, "4.7", "%s step %zu: the edits cannot be applied one at a time", where, step);
        eh_text_free(&text);
        return;
    }
    actual = dump_of(document, text.bytes, text.length, &actual_length);
    expected = dump_text(text.bytes, text.length, &expected_length);
    if (!same(actual, actual_length, expected, expected_length)) {
        fail(state, "4.7", "%s step %zu: the batch differs from its edits one at a time", where, step);
    }
    markdown_core_dump_free(actual);
    markdown_core_dump_free(expected);
    eh_text_free(&text);
}

/* ------------------------------------------------ scripted identity (4.5) */

typedef struct locate {
    const char *kind;
    size_t at;
    bool found;
} locate;

static int visit_locate(const markdown_core_node *node, ts_ast_range range, void *context) {
    locate *search = (locate *)context;
    if (strcmp(markdown_core_node_kind_name(markdown_core_node_get_kind(node)), search->kind) == 0 &&
        range.start == (int64_t)search->at) {
        search->found = true;
        return 1;
    }
    return 0;
}

static bool node_at(const uint8_t *text, size_t length, const char *kind, size_t at) {
    markdown_core_document *document = ts_ast_parse(text, length);
    locate search = {kind, at, false};
    if (document) {
        ts_ast_walk(markdown_core_document_root(document), visit_locate, &search);
    }
    markdown_core_document_free(document);
    return search.found;
}

static void check_expectations(run *state, const char *where, size_t step, const eh_step *entry, const eh_text *before,
                               const eh_text *after) {
    size_t index;
    for (index = 0; index < entry->expectation_count; index++) {
        const eh_expectation *expectation = &entry->expectations[index];
        bool ok = true;
        if (strcmp(expectation->verb, "only") == 0) {
            continue;
        }
        if (strcmp(expectation->verb, "retired") == 0) {
            ok = node_at(before->bytes, before->length, expectation->kind, expectation->at);
        } else {
            ok = node_at(after->bytes, after->length, expectation->kind, expectation->at);
            if (ok && strcmp(expectation->verb, "kept") == 0) {
                ok = node_at(before->bytes, before->length, expectation->kind, expectation->from);
            }
        }
        if (!ok) {
            fail(state, "4.5", "%s: no %s node where step %zu expects one", where, expectation->kind, step);
        }
    }
}

/* ------------------------------------------------------------------ runs */

/* An edit script in one unit: steps are converted from the script's UTF-8
 * offsets to the unit through the text model before each step. */
static void run_edits(run *state, const char *where, eh_unit unit, const uint8_t *document, size_t length,
                      const eh_script *script) {
    eh_text model = {0};
    markdown_core_document *current = NULL;
    void *subject;
    size_t index;
    if (!eh_text_assign(&model, document, length)) {
        fail(state, "harness", "%s: out of memory", where);
        return;
    }
    subject = state->subject->open(unit, document, length, &current);
    if (!subject) {
        fail(state, "harness", "%s: the subject did not open", where);
        eh_text_free(&model);
        return;
    }
    for (index = 0; index < script->count && !stopped(state); index++) {
        const eh_step *step = &script->steps[index];
        markdown_core_document *next = NULL;
        eh_status status;
        if (step->kind == EH_STEP_REJECT_EDIT || step->kind == EH_STEP_REJECT_APPEND) {
            if (step->unit != unit) {
                continue;
            }
            status = step->kind == EH_STEP_REJECT_EDIT
                         ? state->subject->edit(subject, step->edits, step->count, &next)
                         : state->subject->append(subject, step->edits[0].text, step->edits[0].length, &next);
            if (status != EH_INVALID) {
                fail(state, "4.8", "%s step %zu: an invalid argument was accepted", where, index + 1);
            }
            markdown_core_document_free(next);
            /* A subject that accepted it no longer holds the model's text. */
            if (status == EH_OK) {
                break;
            }
            continue;
        }
        {
            eh_edit *converted = (eh_edit *)malloc(step->count * sizeof(eh_edit));
            eh_text before = {0};
            size_t edit;
            if (!converted || !eh_text_assign(&before, model.bytes, model.length)) {
                free(converted);
                fail(state, "harness", "%s: out of memory", where);
                break;
            }
            for (edit = 0; edit < step->count; edit++) {
                converted[edit] = step->edits[edit];
                if (unit == EH_UTF16) {
                    converted[edit].start = eh_utf16_units(model.bytes, step->edits[edit].start);
                    converted[edit].end = eh_utf16_units(model.bytes, step->edits[edit].end);
                }
            }
            status = step->kind == EH_STEP_EDIT
                         ? state->subject->edit(subject, converted, step->count, &next)
                         : state->subject->append(subject, step->edits[0].text, step->edits[0].length, &next);
            free(converted);
            if (status != EH_OK ||
                !(step->kind == EH_STEP_EDIT ? model_batch(&model, step->edits, step->count)
                                             : eh_text_replace(&model, model.length, model.length, step->edits[0].text,
                                                               step->edits[0].length))) {
                fail(state, status == EH_INVALID ? "4.8" : "harness", "%s step %zu: a valid step failed", where,
                     index + 1);
                eh_text_free(&before);
                markdown_core_document_free(next);
                break;
            }
            markdown_core_document_free(current);
            current = next;
            check_equivalence(state, where, index + 1, subject, current, &model);
            if (step->kind == EH_STEP_EDIT && step->count > 1) {
                check_batch(state, where, index + 1, current, &before, step->edits, step->count);
            }
            check_expectations(state, where, index + 1, step, &before, &model);
            eh_text_free(&before);
        }
    }
    markdown_core_document_free(current);
    state->subject->close(subject);
    eh_text_free(&model);
}

/* A stream from an empty session: every chunk is appended, and 4.1 holds
 * after each one (4.6). */
static void run_stream(run *state, const char *where, eh_unit unit, const uint8_t *document, const size_t *ends,
                       size_t count) {
    eh_text model = {0};
    markdown_core_document *current = NULL;
    void *subject = state->subject->open(unit, (const uint8_t *)"", 0, &current);
    size_t index, from = 0;
    if (!subject || !eh_text_assign(&model, NULL, 0)) {
        fail(state, "harness", "%s: the subject did not open", where);
        markdown_core_document_free(current);
        if (subject) {
            state->subject->close(subject);
        }
        return;
    }
    for (index = 0; index < count && !stopped(state); index++) {
        markdown_core_document *next = NULL;
        eh_status status = state->subject->append(subject, document + from, ends[index] - from, &next);
        if (status != EH_OK ||
            !eh_text_replace(&model, model.length, model.length, document + from, ends[index] - from)) {
            fail(state, status == EH_INVALID ? "4.8" : "harness", "%s chunk %zu: a valid chunk failed", where,
                 index + 1);
            markdown_core_document_free(next);
            break;
        }
        markdown_core_document_free(current);
        current = next;
        check_equivalence(state, where, index + 1, subject, current, &model);
        from = ends[index];
    }
    markdown_core_document_free(current);
    state->subject->close(subject);
    eh_text_free(&model);
}

/* 3.1: parts that keep their meaning when joined. */
static void check_parts(run *state, const char *where, const uint8_t *document, size_t length, const eh_case *entry) {
    size_t index, from = 0, sum = 0;
    markdown_core_document *whole;
    for (index = 0; index < entry->part_count; index++) {
        markdown_core_document *part;
        if (entry->parts[index] > length - from) {
            fail(state, "3.1", "%s: part %zu runs past the document", where, index + 1);
            return;
        }
        part = ts_ast_parse(document + from, entry->parts[index]);
        if (!part) {
            fail(state, "harness", "%s: part %zu did not parse", where, index + 1);
            return;
        }
        sum += markdown_core_node_child_count(markdown_core_document_root(part));
        markdown_core_document_free(part);
        from += entry->parts[index];
    }
    if (from != length) {
        fail(state, "3.1", "%s: the parts do not cover the document", where);
        return;
    }
    whole = ts_ast_parse(document, length);
    if (!whole || markdown_core_node_child_count(markdown_core_document_root(whole)) != sum) {
        fail(state, "3.1", "%s: the composite's root children differ from its parts' sum %zu", where, sum);
    }
    markdown_core_document_free(whole);
}

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

/* Returns the previous document for one step: its third, in every run. */
typedef struct stale_subject {
    void *inner;
    size_t steps;
    eh_text previous;
} stale_subject;

static void *stale_open(eh_unit unit, const uint8_t *text, size_t length, markdown_core_document **document) {
    stale_subject *subject = (stale_subject *)calloc(1, sizeof(*subject));
    if (!subject) {
        return NULL;
    }
    subject->inner = eh_reparse.open(unit, text, length, document);
    if (!subject->inner) {
        free(subject);
        return NULL;
    }
    return subject;
}

static eh_status stale_step(stale_subject *subject, eh_status status, markdown_core_document **document) {
    if (status == EH_OK && ++subject->steps == 3) {
        markdown_core_document_free(*document);
        *document = ts_ast_parse(subject->previous.bytes, subject->previous.length);
        return *document ? EH_OK : EH_FAILED;
    }
    return status;
}

static bool stale_remember(stale_subject *subject) {
    size_t length = 0;
    const uint8_t *text = eh_reparse.text(subject->inner, &length);
    return eh_text_assign(&subject->previous, text, length);
}

static eh_status stale_edit(void *handle, const eh_edit *edits, size_t count, markdown_core_document **document) {
    stale_subject *subject = (stale_subject *)handle;
    *document = NULL;
    if (!stale_remember(subject)) {
        return EH_FAILED;
    }
    return stale_step(subject, eh_reparse.edit(subject->inner, edits, count, document), document);
}

static eh_status stale_append(void *handle, const uint8_t *text, size_t length, markdown_core_document **document) {
    stale_subject *subject = (stale_subject *)handle;
    *document = NULL;
    if (!stale_remember(subject)) {
        return EH_FAILED;
    }
    return stale_step(subject, eh_reparse.append(subject->inner, text, length, document), document);
}

static const uint8_t *stale_text(const void *handle, size_t *length) {
    return eh_reparse.text(((const stale_subject *)handle)->inner, length);
}

static void stale_close(void *handle) {
    stale_subject *subject = (stale_subject *)handle;
    if (subject) {
        eh_reparse.close(subject->inner);
        eh_text_free(&subject->previous);
        free(subject);
    }
}

static const eh_subject_class stale = {
    "returns the previous document for one step", stale_open, stale_edit, stale_append, stale_text, stale_close};

/* Accepts an end inside a scalar: it is the reparse subject without the
 * boundary check, so an offset inside a scalar counts to the byte it names,
 * or in UTF-16 to the end of the scalar it falls in. */
typedef struct lenient_subject {
    eh_unit unit;
    eh_text text;
} lenient_subject;

static size_t lenient_offset(const lenient_subject *subject, size_t offset) {
    size_t at = 0, units = 0;
    if (subject->unit == EH_UTF8) {
        return offset;
    }
    while (units < offset && at < subject->text.length) {
        units += subject->text.bytes[at] >= 0xf0 ? 2 : 1;
        do {
            at++;
        } while (at < subject->text.length && (subject->text.bytes[at] & 0xc0) == 0x80);
    }
    return units < offset ? subject->text.length + 1 : at;
}

static void *lenient_open(eh_unit unit, const uint8_t *text, size_t length, markdown_core_document **document) {
    lenient_subject *subject = (lenient_subject *)calloc(1, sizeof(*subject));
    if (subject) {
        subject->unit = unit;
    }
    if (!subject || !eh_text_assign(&subject->text, text, length)) {
        free(subject);
        return NULL;
    }
    *document = ts_ast_parse(subject->text.bytes, subject->text.length);
    return subject;
}

static eh_status lenient_edit(void *handle, const eh_edit *edits, size_t count, markdown_core_document **document) {
    lenient_subject *subject = (lenient_subject *)handle;
    eh_edit *sorted = (eh_edit *)malloc((count ? count : 1) * sizeof(eh_edit));
    eh_status status = EH_OK;
    size_t index;
    *document = NULL;
    if (!sorted) {
        return EH_FAILED;
    }
    /* In offset order, then applied from the last, so that the offsets not
     * yet applied stay valid. */
    for (index = 0; index < count; index++) {
        eh_edit edit = edits[index];
        size_t place = index;
        edit.start = lenient_offset(subject, edit.start);
        edit.end = lenient_offset(subject, edit.end);
        while (place > 0 && sorted[place - 1].start > edit.start) {
            sorted[place] = sorted[place - 1];
            place--;
        }
        sorted[place] = edit;
    }
    for (index = count; status == EH_OK && index-- > 0;) {
        if (!eh_utf8_valid(sorted[index].text, sorted[index].length) || sorted[index].start > sorted[index].end ||
            sorted[index].end > subject->text.length ||
            !eh_text_replace(&subject->text, sorted[index].start, sorted[index].end, sorted[index].text,
                             sorted[index].length)) {
            status = EH_INVALID;
        }
    }
    free(sorted);
    if (status != EH_OK) {
        return status;
    }
    *document = ts_ast_parse(subject->text.bytes, subject->text.length);
    return *document ? EH_OK : EH_FAILED;
}

static eh_status lenient_append(void *handle, const uint8_t *text, size_t length, markdown_core_document **document) {
    lenient_subject *subject = (lenient_subject *)handle;
    *document = NULL;
    if (!eh_utf8_valid(text, length) ||
        !eh_text_replace(&subject->text, subject->text.length, subject->text.length, text, length)) {
        return EH_INVALID;
    }
    *document = ts_ast_parse(subject->text.bytes, subject->text.length);
    return *document ? EH_OK : EH_FAILED;
}

static const uint8_t *lenient_text(const void *handle, size_t *length) {
    const lenient_subject *subject = (const lenient_subject *)handle;
    *length = subject->text.length;
    return subject->text.bytes;
}

static void lenient_close(void *handle) {
    lenient_subject *subject = (lenient_subject *)handle;
    if (subject) {
        eh_text_free(&subject->text);
        free(subject);
    }
}

static const eh_subject_class lenient = {
    "accepts an end inside a scalar", lenient_open, lenient_edit, lenient_append, lenient_text, lenient_close};

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
    failures += !eh_utf8_valid(text, length) || eh_utf8_valid((const uint8_t *)"\xed\xa0\x80", 3) ||
                eh_utf8_valid((const uint8_t *)"\xc0\xaf", 2) || eh_utf8_valid((const uint8_t *)"\xe4\xb8", 2) ||
                !eh_utf8_valid((const uint8_t *)"\0", 1);
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

int main(int argc, char **argv) {
    const char *directory = NULL;
    bool self_test = false;
    const char *family = NULL;
    int index;
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
        fputs("usage: incremental_runner --set DIR [--family F | --self-test]\n", stderr);
        return 2;
    }
    if (check_harness()) {
        return 1;
    }
    if (self_test) {
        /* Each faulty subject must fail, and with its own oracle. */
        static const struct {
            const eh_subject_class *subject;
            const char *oracle;
        } faulty[] = {{&stale, "4.1"}, {&lenient, "4.8"}};
        int failures = 0;
        size_t which;
        for (which = 0; which < sizeof(faulty) / sizeof(faulty[0]); which++) {
            run state;
            memset(&state, 0, sizeof(state));
            state.subject = faulty[which].subject;
            state.stop_at_failure = true;
            run_set(&state, directory);
            if (!state.failures || strcmp(state.first_oracle, faulty[which].oracle) != 0) {
                fprintf(stderr, "FAILED: the subject that %s was not caught by %s%s%s\n", faulty[which].subject->name,
                        faulty[which].oracle, state.failures ? "; first failure: " : "",
                        state.failures ? state.first : "");
                failures++;
            } else {
                printf("ok: the subject that %s fails %s (%s)\n", faulty[which].subject->name, faulty[which].oracle,
                       state.first);
            }
        }
        return failures ? 1 : 0;
    }
    {
        run state;
        memset(&state, 0, sizeof(state));
        state.subject = &eh_reparse;
        state.family = family;
        run_set(&state, directory);
        if (state.failures) {
            fprintf(stderr, "%zu incremental check(s) failed\n", state.failures);
            return 1;
        }
        printf("incremental correctness set passed with the %s subject: %zu case(s)%s%s\n", eh_reparse.name,
               state.checked, family ? " of family " : "", family ? family : "");
    }
    return 0;
}
