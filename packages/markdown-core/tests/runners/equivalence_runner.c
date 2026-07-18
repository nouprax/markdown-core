/* Session equivalence suite: an incrementally edited session must always
 * dump byte-identically to a one-shot parse of the same final text, and its
 * deltas must account for every observable node change.
 *
 * Every replay drives the public facade only.  A shadow text buffer receives
 * the same edits as the session, so each commit can be checked against
 * markdown_core_document_parse of the shadow bytes; a shadow id->revision
 * mirror is maintained purely from deltas and compared against a fresh
 * walk after every commit, which catches adoption bugs that dumps cannot
 * see.
 *
 *   equivalence_runner --list
 *   equivalence_runner --case canonical --fixtures DIR NAME MASK [NAME MASK ...]
 *   equivalence_runner --case spec --spec FILE
 *   equivalence_runner --case random_edits --spec FILE
 *   equivalence_runner --case link_ref_edits
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* --- shadow text -------------------------------------------------------- */

typedef struct eq_text {
    uint8_t *bytes;
    size_t length;
    size_t capacity;
} eq_text;

/* The buffer is always allocated (even for an empty text, so no pointer
 * arithmetic ever touches NULL) and always NUL-terminated one byte past
 * `length` (the link_ref_edits script locates edit positions with strstr). */
static int eq_text_splice(eq_text *text, size_t start, size_t end, const uint8_t *insert, size_t insert_length) {
    size_t removed = end - start;
    size_t new_length = text->length - removed + insert_length;
    if (new_length + 1 > text->capacity) {
        size_t capacity = text->capacity ? text->capacity : 64;
        uint8_t *grown;
        while (capacity < new_length + 1) {
            capacity *= 2;
        }
        grown = (uint8_t *)realloc(text->bytes, capacity);
        if (!grown) {
            return -1;
        }
        text->bytes = grown;
        text->capacity = capacity;
    }
    memmove(text->bytes + start + insert_length, text->bytes + end, text->length - end);
    if (insert_length) {
        memcpy(text->bytes + start, insert, insert_length);
    }
    text->bytes[new_length] = '\0';
    text->length = new_length;
    return 0;
}

/* --- delta mirror ---------------------------------------------------- */

typedef struct eq_mirror_entry {
    markdown_core_node_id id;
    uint64_t revision;
} eq_mirror_entry;

typedef struct eq_mirror {
    eq_mirror_entry *entries;
    size_t count;
    size_t capacity;
} eq_mirror;

static eq_mirror_entry *eq_mirror_find(eq_mirror *mirror, markdown_core_node_id id) {
    size_t i;
    for (i = 0; i < mirror->count; i++) {
        if (mirror->entries[i].id == id) {
            return &mirror->entries[i];
        }
    }
    return NULL;
}

static int eq_mirror_insert(eq_mirror *mirror, markdown_core_node_id id, uint64_t revision) {
    if (mirror->count == mirror->capacity) {
        size_t capacity = mirror->capacity ? mirror->capacity * 2 : 64;
        eq_mirror_entry *grown = (eq_mirror_entry *)realloc(mirror->entries, capacity * sizeof(*grown));
        if (!grown) {
            return -1;
        }
        mirror->entries = grown;
        mirror->capacity = capacity;
    }
    mirror->entries[mirror->count].id = id;
    mirror->entries[mirror->count].revision = revision;
    mirror->count++;
    return 0;
}

static void eq_mirror_remove(eq_mirror *mirror, eq_mirror_entry *entry) {
    *entry = mirror->entries[mirror->count - 1];
    mirror->count--;
}

/* --- replay harness ------------------------------------------------------ */

typedef struct eq_replay {
    const char *context;
    markdown_core_session *session;
    eq_text shadow;
    eq_mirror mirror;
    const markdown_core_parse_options *options;
} eq_replay;

static int eq_replay_open(eq_replay *replay, const char *context, const markdown_core_parse_options *options) {
    markdown_core_error *error = NULL;
    memset(replay, 0, sizeof(*replay));
    replay->context = context;
    replay->options = options;
    replay->session = markdown_core_session_open(options, &error);
    if (!replay->session) {
        markdown_core_error_free(error);
        eq_fail(context, "session open failed");
        return -1;
    }
    /* Revision 0 (empty document) seeds the mirror. */
    {
        const markdown_core_document *document = markdown_core_session_document(replay->session);
        const markdown_core_node *root = markdown_core_document_root(document);
        if (!root ||
            eq_mirror_insert(&replay->mirror, markdown_core_node_get_id(root), markdown_core_node_get_revision(root)) !=
                0) {
            eq_fail(context, "empty session has no addressable root");
            return -1;
        }
    }
    return 0;
}

static void eq_replay_close(eq_replay *replay) {
    markdown_core_session_free(replay->session);
    free(replay->shadow.bytes);
    free(replay->mirror.entries);
    memset(replay, 0, sizeof(*replay));
}

static int eq_replay_edit(eq_replay *replay, size_t start, size_t end, const uint8_t *bytes, size_t length) {
    markdown_core_error *error = NULL;
    if (!markdown_core_session_edit(replay->session, start, end, bytes, length, &error)) {
        markdown_core_error_free(error);
        eq_fail(replay->context, "session edit failed");
        return -1;
    }
    if (eq_text_splice(&replay->shadow, start, end, bytes, length) != 0) {
        eq_fail(replay->context, "shadow splice allocation failed");
        return -1;
    }
    return 0;
}

static int eq_delta_disjoint(eq_replay *replay, markdown_core_delta *changes) {
    const markdown_core_node_id *arrays[4];
    size_t counts[4];
    size_t a;
    size_t b;
    size_t i;
    size_t k;

    counts[0] = markdown_core_delta_added(changes, &arrays[0]);
    counts[1] = markdown_core_delta_removed(changes, &arrays[1]);
    counts[2] = markdown_core_delta_changed(changes, &arrays[2]);
    counts[3] = markdown_core_delta_bubbled(changes, &arrays[3]);
    for (a = 0; a < 4; a++) {
        for (i = 0; i < counts[a]; i++) {
            for (b = a; b < 4; b++) {
                for (k = b == a ? i + 1 : 0; k < counts[b]; k++) {
                    if (arrays[a][i] == arrays[b][k]) {
                        eq_fail(replay->context, "delta arrays repeat an id");
                        return -1;
                    }
                }
            }
        }
    }
    return 0;
}

static int eq_apply_delta(eq_replay *replay, markdown_core_delta *changes, uint64_t expected_after) {
    const markdown_core_node_id *ids;
    size_t count;
    size_t i;
    uint64_t before;
    uint64_t after;

    markdown_core_delta_revisions(changes, &before, &after);
    if (after != expected_after) {
        eq_fail(replay->context, "delta revisions disagree with the session");
        return -1;
    }

    if (eq_delta_disjoint(replay, changes) != 0) {
        return -1;
    }

    count = markdown_core_delta_removed(changes, &ids);
    for (i = 0; i < count; i++) {
        eq_mirror_entry *entry = eq_mirror_find(&replay->mirror, ids[i]);
        if (!entry) {
            eq_fail(replay->context, "delta removed an id the mirror never saw");
            return -1;
        }
        eq_mirror_remove(&replay->mirror, entry);
    }

    count = markdown_core_delta_added(changes, &ids);
    for (i = 0; i < count; i++) {
        if (eq_mirror_find(&replay->mirror, ids[i])) {
            eq_fail(replay->context, "delta added an id that already exists");
            return -1;
        }
        if (eq_mirror_insert(&replay->mirror, ids[i], after) != 0) {
            eq_fail(replay->context, "mirror allocation failed");
            return -1;
        }
    }

    count = markdown_core_delta_changed(changes, &ids);
    for (i = 0; i < count; i++) {
        eq_mirror_entry *entry = eq_mirror_find(&replay->mirror, ids[i]);
        if (!entry) {
            eq_fail(replay->context, "delta changed an id the mirror never saw");
            return -1;
        }
        entry->revision = after;
    }

    count = markdown_core_delta_bubbled(changes, &ids);
    for (i = 0; i < count; i++) {
        eq_mirror_entry *entry = eq_mirror_find(&replay->mirror, ids[i]);
        if (!entry) {
            eq_fail(replay->context, "delta bubbled an id the mirror never saw");
            return -1;
        }
        entry->revision = after;
    }

    return 0;
}

/* --- footnote query equivalence ------------------------------------------ */

typedef struct eq_id_list {
    markdown_core_node_id *ids;
    size_t count;
    size_t capacity;
    int failed;
} eq_id_list;

static int eq_id_collect_visit(const markdown_core_node *node, void *context) {
    eq_id_list *list = (eq_id_list *)context;
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 64;
        markdown_core_node_id *grown = (markdown_core_node_id *)realloc(list->ids, capacity * sizeof(*grown));
        if (!grown) {
            list->failed = 1;
            return 1;
        }
        list->ids = grown;
        list->capacity = capacity;
    }
    list->ids[list->count++] = markdown_core_node_get_id(node);
    return 0;
}

typedef struct eq_ordinal {
    markdown_core_node_id id;
    size_t position;
} eq_ordinal;

static int eq_ordinal_compare(const void *a, const void *b) {
    markdown_core_node_id ia = ((const eq_ordinal *)a)->id;
    markdown_core_node_id ib = ((const eq_ordinal *)b)->id;
    return ia < ib ? -1 : (ia > ib ? 1 : 0);
}

/* Walk position of `id`, or SIZE_MAX for id 0 and ids outside the tree. */
static size_t eq_position_of(const eq_ordinal *ordinals, size_t count, markdown_core_node_id id) {
    size_t lo = 0;
    size_t hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ordinals[mid].id == id) {
            return ordinals[mid].position;
        }
        if (ordinals[mid].id < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return SIZE_MAX;
}

static eq_ordinal *eq_ordinals_build(const eq_id_list *list) {
    eq_ordinal *ordinals = (eq_ordinal *)malloc((list->count ? list->count : 1) * sizeof(*ordinals));
    size_t i;
    if (!ordinals) {
        return NULL;
    }
    for (i = 0; i < list->count; i++) {
        ordinals[i].id = list->ids[i];
        ordinals[i].position = i;
    }
    qsort(ordinals, list->count, sizeof(*ordinals), eq_ordinal_compare);
    return ordinals;
}

/* With footnotes enabled, every commit's numbering, resolution, and
 * back-reference answers must equal a fresh session's on the same text.
 * Node identity maps positionally: the dumps already compared equal, so both
 * trees walk the same shape and the i-th node of one corresponds to the i-th
 * node of the other. */
static int eq_check_footnote_queries(eq_replay *replay) {
    markdown_core_session *fresh = NULL;
    markdown_core_error *error = NULL;
    eq_id_list mine = {NULL, 0, 0, 0};
    eq_id_list theirs = {NULL, 0, 0, 0};
    eq_ordinal *mine_ordinals = NULL;
    eq_ordinal *theirs_ordinals = NULL;
    int result = -1;
    size_t i;

    fresh = markdown_core_session_open(replay->options, &error);
    if (!fresh || !markdown_core_session_edit(fresh, 0, 0, replay->shadow.bytes, replay->shadow.length, &error) ||
        !markdown_core_session_commit(fresh, NULL, &error)) {
        markdown_core_error_free(error);
        eq_fail(replay->context, "fresh footnote reference session failed");
        goto done;
    }
    if (ts_ast_walk(
            markdown_core_document_root(markdown_core_session_document(replay->session)),
            eq_id_collect_visit,
            &mine
        ) < 0 ||
        mine.failed ||
        ts_ast_walk(markdown_core_document_root(markdown_core_session_document(fresh)), eq_id_collect_visit, &theirs) <
            0 ||
        theirs.failed) {
        eq_fail(replay->context, "footnote walk failed to allocate");
        goto done;
    }
    if (mine.count != theirs.count) {
        eq_fail(replay->context, "footnote sessions walk different node counts");
        goto done;
    }
    mine_ordinals = eq_ordinals_build(&mine);
    theirs_ordinals = eq_ordinals_build(&theirs);
    if (!mine_ordinals || !theirs_ordinals) {
        eq_fail(replay->context, "footnote ordinal map failed to allocate");
        goto done;
    }

    for (i = 0; i < mine.count; i++) {
        markdown_core_footnote_info a;
        markdown_core_footnote_info b;
        bool found_a = markdown_core_session_footnote_info(replay->session, mine.ids[i], &a);
        bool found_b = markdown_core_session_footnote_info(fresh, theirs.ids[i], &b);
        if (found_a != found_b) {
            eq_fail(replay->context, "footnote info presence diverged from a fresh session");
            goto done;
        }
        if (!found_a) {
            continue;
        }
        if (a.number != b.number || a.reference_ordinal != b.reference_ordinal ||
            a.reference_count != b.reference_count ||
            eq_position_of(mine_ordinals, mine.count, a.definition) !=
                eq_position_of(theirs_ordinals, theirs.count, b.definition)) {
            eq_fail(replay->context, "footnote info diverged from a fresh session");
            goto done;
        }
    }

    {
        const markdown_core_node_id *a_ids;
        const markdown_core_node_id *b_ids;
        size_t a_count = markdown_core_session_footnotes(replay->session, &a_ids);
        size_t b_count = markdown_core_session_footnotes(fresh, &b_ids);
        if (a_count != b_count) {
            eq_fail(replay->context, "footnote first-use list length diverged from a fresh session");
            goto done;
        }
        for (i = 0; i < a_count; i++) {
            const markdown_core_node_id *a_refs;
            const markdown_core_node_id *b_refs;
            size_t a_refs_count;
            size_t b_refs_count;
            size_t k;
            if (eq_position_of(mine_ordinals, mine.count, a_ids[i]) !=
                eq_position_of(theirs_ordinals, theirs.count, b_ids[i])) {
                eq_fail(replay->context, "footnote first-use order diverged from a fresh session");
                goto done;
            }
            a_refs_count = markdown_core_session_footnote_references(replay->session, a_ids[i], &a_refs);
            b_refs_count = markdown_core_session_footnote_references(fresh, b_ids[i], &b_refs);
            if (a_refs_count != b_refs_count) {
                eq_fail(replay->context, "footnote back-reference count diverged from a fresh session");
                goto done;
            }
            for (k = 0; k < a_refs_count; k++) {
                if (eq_position_of(mine_ordinals, mine.count, a_refs[k]) !=
                    eq_position_of(theirs_ordinals, theirs.count, b_refs[k])) {
                    eq_fail(replay->context, "footnote back-reference order diverged from a fresh session");
                    goto done;
                }
            }
        }
    }

    result = 0;
done:
    markdown_core_session_free(fresh);
    free(mine.ids);
    free(theirs.ids);
    free(mine_ordinals);
    free(theirs_ordinals);
    return result;
}

typedef struct eq_walk_state {
    eq_replay *replay;
    size_t seen;
    int failed;
} eq_walk_state;

static int eq_walk_visit(const markdown_core_node *node, void *context) {
    eq_walk_state *state = (eq_walk_state *)context;
    eq_replay *replay = state->replay;
    markdown_core_node_id id = markdown_core_node_get_id(node);
    eq_mirror_entry *entry = eq_mirror_find(&replay->mirror, id);

    state->seen++;
    if (!entry) {
        eq_fail(replay->context, "tree holds an id the delta stream never added");
        state->failed = 1;
        return 1;
    }
    if (entry->revision != markdown_core_node_get_revision(node)) {
        eq_fail(replay->context, "node revision changed without a delta notification");
        state->failed = 1;
        return 1;
    }
    if (markdown_core_session_node_by_id(replay->session, id) != node) {
        eq_fail(replay->context, "node_by_id disagrees with the committed tree");
        state->failed = 1;
        return 1;
    }
    return 0;
}

/* Commits the session, folds the delta into the mirror, verifies the
 * mirror against a fresh walk, and compares the session dump with a one-shot
 * parse of the shadow text. */
static int eq_replay_commit(eq_replay *replay) {
    markdown_core_error *error = NULL;
    markdown_core_delta *changes = NULL;
    const markdown_core_document *document;
    const markdown_core_node *root;
    markdown_core_document *reference = NULL;
    uint8_t *session_dump = NULL;
    uint8_t *reference_dump = NULL;
    size_t session_dump_length = 0;
    size_t reference_dump_length = 0;
    eq_walk_state state;
    int result = -1;

    if (!markdown_core_session_commit(replay->session, &changes, &error)) {
        markdown_core_error_free(error);
        eq_fail(replay->context, "commit failed");
        return -1;
    }
    if (!changes) {
        eq_fail(replay->context, "commit produced no delta");
        return -1;
    }
    if (eq_apply_delta(replay, changes, markdown_core_session_revision(replay->session)) != 0) {
        goto done;
    }

    document = markdown_core_session_document(replay->session);
    root = markdown_core_document_root(document);
    state.replay = replay;
    state.seen = 0;
    state.failed = 0;
    if (ts_ast_walk(root, eq_walk_visit, &state) < 0 || state.failed) {
        if (!state.failed) {
            eq_fail(replay->context, "mirror walk failed to allocate");
        }
        goto done;
    }
    if (state.seen != replay->mirror.count) {
        eq_fail(replay->context, "mirror holds ids that are no longer in the tree");
        goto done;
    }

    if (!markdown_core_document_dump(document, &session_dump, &session_dump_length, &error)) {
        markdown_core_error_free(error);
        error = NULL;
        eq_fail(replay->context, "session dump failed");
        goto done;
    }
    reference = markdown_core_document_parse(replay->shadow.bytes, replay->shadow.length, replay->options, &error);
    if (!reference) {
        markdown_core_error_free(error);
        error = NULL;
        eq_fail(replay->context, "one-shot reference parse failed");
        goto done;
    }
    if (!markdown_core_document_dump(reference, &reference_dump, &reference_dump_length, &error)) {
        markdown_core_error_free(error);
        error = NULL;
        eq_fail(replay->context, "reference dump failed");
        goto done;
    }
    if (session_dump_length != reference_dump_length ||
        memcmp(session_dump, reference_dump, reference_dump_length) != 0) {
        eq_fail(replay->context, "session dump diverged from the one-shot parse");
        ts_print_line_diff(stderr, (const char *)reference_dump, (const char *)session_dump);
        goto done;
    }

    if (replay->options->footnotes && eq_check_footnote_queries(replay) != 0) {
        goto done;
    }

    result = 0;
done:
    markdown_core_dump_free(session_dump);
    markdown_core_dump_free(reference_dump);
    markdown_core_document_free(reference);
    markdown_core_delta_free(changes);
    return result;
}

static int eq_replay_append_commit(eq_replay *replay, const uint8_t *bytes, size_t length) {
    if (eq_replay_edit(replay, replay->shadow.length, replay->shadow.length, bytes, length) != 0) {
        return -1;
    }
    return eq_replay_commit(replay);
}

/* --- replays over one input --------------------------------------------- */

static int eq_replay_per_line(
    const char *context,
    const uint8_t *text,
    size_t length,
    const markdown_core_parse_options *options
) {
    eq_replay replay;
    size_t offset = 0;
    int result = -1;

    if (eq_replay_open(&replay, context, options) != 0) {
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
        if (eq_replay_append_commit(&replay, text + offset, line_end - offset) != 0) {
            goto done;
        }
        offset = line_end;
    }
    /* Empty inputs still must commit cleanly. */
    if (length == 0 && eq_replay_commit(&replay) != 0) {
        goto done;
    }
    result = 0;
done:
    eq_replay_close(&replay);
    return result;
}

static int eq_replay_per_byte(
    const char *context,
    const uint8_t *text,
    size_t length,
    const markdown_core_parse_options *options
) {
    eq_replay replay;
    size_t offset;
    int result = -1;

    if (eq_replay_open(&replay, context, options) != 0) {
        return -1;
    }
    for (offset = 0; offset < length; offset++) {
        if (eq_replay_append_commit(&replay, text + offset, 1) != 0) {
            goto done;
        }
    }
    result = 0;
done:
    eq_replay_close(&replay);
    return result;
}

/* Seeded random edit script: the target text is cut into chunks that arrive
 * in random order (each spliced at its correct relative position), then the
 * document suffers a garbage insertion that is edited away again.  The final
 * text is the target, and every intermediate commit is checked. */
static int eq_replay_random_edits(
    const char *context,
    const uint8_t *text,
    size_t length,
    const markdown_core_parse_options *options,
    ts_prng *prng
) {
    enum { EQ_MAX_CHUNKS = 8 };
    size_t boundaries[EQ_MAX_CHUNKS + 1];
    size_t order[EQ_MAX_CHUNKS];
    int inserted[EQ_MAX_CHUNKS] = {0};
    size_t chunk_count;
    size_t i;
    eq_replay replay;
    int result = -1;

    chunk_count = length ? 1 + (size_t)(ts_prng_next(prng) % EQ_MAX_CHUNKS) : 0;
    if (chunk_count > length) {
        chunk_count = length;
    }
    boundaries[0] = 0;
    for (i = 1; i < chunk_count; i++) {
        boundaries[i] = (size_t)(ts_prng_next(prng) % (length + 1));
    }
    boundaries[chunk_count] = length;
    /* Insertion sort keeps the boundary list ordered. */
    for (i = 1; i <= chunk_count; i++) {
        size_t j = i;
        while (j > 0 && boundaries[j - 1] > boundaries[j]) {
            size_t swap = boundaries[j - 1];
            boundaries[j - 1] = boundaries[j];
            boundaries[j] = swap;
            j--;
        }
    }
    for (i = 0; i < chunk_count; i++) {
        order[i] = i;
    }
    for (i = chunk_count; i > 1; i--) {
        size_t j = (size_t)(ts_prng_next(prng) % i);
        size_t swap = order[i - 1];
        order[i - 1] = order[j];
        order[j] = swap;
    }

    if (eq_replay_open(&replay, context, options) != 0) {
        return -1;
    }

    for (i = 0; i < chunk_count; i++) {
        size_t chunk = order[i];
        size_t position = 0;
        size_t k;
        for (k = 0; k < chunk; k++) {
            if (inserted[k]) {
                position += boundaries[k + 1] - boundaries[k];
            }
        }
        if (eq_replay_edit(
                &replay,
                position,
                position,
                text + boundaries[chunk],
                boundaries[chunk + 1] - boundaries[chunk]
            ) != 0) {
            goto done;
        }
        inserted[chunk] = 1;
        /* Commit roughly every other insertion to cover coalesced edits. */
        if ((ts_prng_next(prng) & 1) && eq_replay_commit(&replay) != 0) {
            goto done;
        }
    }
    if (eq_replay_commit(&replay) != 0) {
        goto done;
    }

    /* Garbage round-trip: insert noise, commit, edit it away, commit. */
    {
        static const uint8_t noise[] = "*[zz](x\n> ~~q~~\n";
        size_t position = replay.shadow.length ? (size_t)(ts_prng_next(prng) % replay.shadow.length) : 0;
        if (eq_replay_edit(&replay, position, position, noise, sizeof(noise) - 1) != 0 ||
            eq_replay_commit(&replay) != 0 ||
            eq_replay_edit(&replay, position, position + sizeof(noise) - 1, NULL, 0) != 0 ||
            eq_replay_commit(&replay) != 0) {
            goto done;
        }
    }

    if (replay.shadow.length != length || (length && memcmp(replay.shadow.bytes, text, length) != 0)) {
        eq_fail(context, "random edit script did not rebuild the target text");
        goto done;
    }
    result = 0;
done:
    eq_replay_close(&replay);
    return result;
}

/* --- cases ---------------------------------------------------------------- */

static int parse_option_mask(const char *mask, markdown_core_parse_options *options) {
    bool *fields[] = {
        &options->smart_punctuation,
        &options->footnotes,
        &options->strip_html_comments,
        &options->tables,
        &options->strikethrough,
        &options->autolinks,
        &options->task_lists,
        &options->formulas,
        &options->dollar_formula_delimiters,
        &options->latex_formula_delimiters,
        &options->directives
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
        if (i % EQ_SPEC_PER_BYTE_STRIDE == 0 && example->markdown_length <= EQ_PER_BYTE_LIMIT) {
            snprintf(context, sizeof(context), "spec example %d per-byte", example->example);
            eq_replay_per_byte(context, (const uint8_t *)example->markdown, example->markdown_length, &options);
        }
    }
    ts_spec_free(&file);
    return failures ? -1 : 0;
}

/* Rich corpus for the seeded edit scripts: every construct whose commit
 * pipeline has cross-block bookkeeping (link references, footnotes, tables,
 * formulas, directives, comments). */
static const char EQ_RICH_CORPUS[] = "# Title *one*\n"
                                     "\n"
                                     "A [ref][label] link, mail@example.com, `code`, $x+y$, ~~gone~~.\n"
                                     "\n"
                                     "[label]: /first \"one\"\n"
                                     "[label]: /second\n"
                                     "\n"
                                     "> quoted footnote[^fn]\n"
                                     "\n"
                                     "- [x] task\n"
                                     "  1. nested\n"
                                     "\n"
                                     "| a | b |\n"
                                     "| - | - |\n"
                                     "| 1 | 2 |\n"
                                     "\n"
                                     "```math\n"
                                     "x^2\n"
                                     "```\n"
                                     "\n"
                                     ":::note[Label]{k=1}\n"
                                     "body\n"
                                     ":::\n"
                                     "\n"
                                     "[^fn]: footnote *body*\n"
                                     "\n"
                                     "<!-- comment -->\n"
                                     "tail <span>html</span>\n";

static int case_random_edits(const char *spec_path) {
    ts_spec_file file;
    ts_prng prng;
    size_t i;
    int round;

    ts_prng_seed(&prng, EQ_RANDOM_SEED);

    {
        markdown_core_parse_options options;
        markdown_core_parse_options_init(&options);
        options.smart_punctuation = true;
        options.footnotes = true;
        options.strip_html_comments = true;
        options.tables = true;
        options.strikethrough = true;
        options.autolinks = true;
        options.task_lists = true;
        options.formulas = true;
        options.dollar_formula_delimiters = true;
        options.latex_formula_delimiters = true;
        options.directives = true;
        for (round = 0; round < 8; round++) {
            char context[64];
            snprintf(context, sizeof(context), "random rich corpus round %d", round);
            eq_replay_random_edits(
                context,
                (const uint8_t *)EQ_RICH_CORPUS,
                sizeof(EQ_RICH_CORPUS) - 1,
                &options,
                &prng
            );
        }
    }

    if (ts_spec_load(spec_path, &file) != 0) {
        eq_fail(spec_path, "spec fixture failed to load");
        return -1;
    }
    for (i = 0; i < file.count; i += EQ_SPEC_RANDOM_STRIDE) {
        ts_spec_case *example = &file.cases[i];
        char context[256];
        markdown_core_parse_options options;
        ts_ast_options_none(&options);
        snprintf(context, sizeof(context), "random spec example %d", example->example);
        eq_replay_random_edits(context, (const uint8_t *)example->markdown, example->markdown_length, &options, &prng);
    }
    ts_spec_free(&file);
    return failures ? -1 : 0;
}

/* Scripted link-reference-definition edits: winners must re-resolve exactly
 * as a one-shot parse of the edited text at every commit. */
static int case_link_ref_edits(void) {
    static const char initial[] = "[one][label] and [two][other]\n"
                                  "\n"
                                  "[label]: /first \"one\"\n"
                                  "[label]: /second \"two\"\n";
    static const char early_definition[] = "[label]: /zero\n\n";
    eq_replay replay;
    markdown_core_parse_options options;
    int result = -1;
    size_t position;

    markdown_core_parse_options_init(&options);
    if (eq_replay_open(&replay, "link_ref_edits", &options) != 0) {
        return -1;
    }

    /* Duplicate labels: the first definition wins. */
    if (eq_replay_edit(&replay, 0, 0, (const uint8_t *)initial, sizeof(initial) - 1) != 0 ||
        eq_replay_commit(&replay) != 0) {
        goto done;
    }

    /* Deleting the winning definition re-elects the later duplicate. */
    position = (size_t)(strstr((const char *)replay.shadow.bytes, "[label]: /first") - (char *)replay.shadow.bytes);
    if (eq_replay_edit(&replay, position, position + strlen("[label]: /first \"one\"\n"), NULL, 0) != 0 ||
        eq_replay_commit(&replay) != 0) {
        goto done;
    }

    /* A definition inserted earlier in the document takes the label over. */
    if (eq_replay_edit(&replay, 0, 0, (const uint8_t *)early_definition, sizeof(early_definition) - 1) != 0 ||
        eq_replay_commit(&replay) != 0) {
        goto done;
    }

    /* Editing the winning definition's destination re-resolves the links. */
    position = (size_t)(strstr((const char *)replay.shadow.bytes, "/zero") - (char *)replay.shadow.bytes);
    if (eq_replay_edit(&replay, position, position + strlen("/zero"), (const uint8_t *)"/elsewhere", 10) != 0 ||
        eq_replay_commit(&replay) != 0) {
        goto done;
    }

    /* A definition for the second label appears after its reference. */
    if (eq_replay_edit(
            &replay,
            replay.shadow.length,
            replay.shadow.length,
            (const uint8_t *)"\n[other]: /other\n",
            17
        ) != 0 ||
        eq_replay_commit(&replay) != 0) {
        goto done;
    }

    /* Removing every definition degrades the references to literal text. */
    position = (size_t)(strstr((const char *)replay.shadow.bytes, "[label]:") - (char *)replay.shadow.bytes);
    if (eq_replay_edit(&replay, position, replay.shadow.length, NULL, 0) != 0 || eq_replay_commit(&replay) != 0) {
        goto done;
    }

    result = failures ? -1 : 0;
done:
    eq_replay_close(&replay);
    return result;
}

/* Scripted footnote edits: the tree stays source-faithful (definitions never
 * move, references never degrade), so every commit must dump-equal a
 * one-shot parse while first-use ordinals cascade underneath; ordinal and
 * resolution changes surface as revision-only `changed` entries, which the
 * mirror check validates against a fresh walk. */
static int case_footnote_edits(void) {
    static const char initial[] = "alpha[^a] then beta[^b] then beta again[^b]\n"
                                  "\n"
                                  "[^b]: b body\n";
    eq_replay replay;
    markdown_core_parse_options options;
    int result = -1;
    size_t position;

    markdown_core_parse_options_init(&options);
    if (eq_replay_open(&replay, "footnote_edits", &options) != 0) {
        return -1;
    }

    /* [^a] is unresolved; [^b] is number 1 with two references. */
    if (eq_replay_edit(&replay, 0, 0, (const uint8_t *)initial, sizeof(initial) - 1) != 0 ||
        eq_replay_commit(&replay) != 0) {
        goto done;
    }

    /* A definition appearing at the end resolves [^a]; first-use order makes
     * it number 1 and shifts [^b] to number 2 with no dump change. */
    if (eq_replay_append_commit(&replay, (const uint8_t *)"\n[^a]: a body\n", 14) != 0) {
        goto done;
    }

    /* An early reference to a brand-new label shifts every later ordinal. */
    if (eq_replay_edit(&replay, 0, 0, (const uint8_t *)"first[^c]\n\n[^c]: c body\n\n", 25) != 0 ||
        eq_replay_commit(&replay) != 0) {
        goto done;
    }

    /* A duplicate definition earlier in the document takes the label over. */
    if (eq_replay_edit(&replay, 0, 0, (const uint8_t *)"[^b]: usurper\n\n", 15) != 0 ||
        eq_replay_commit(&replay) != 0) {
        goto done;
    }

    /* Deleting a definition flips its references back to unresolved; the
     * definition node itself stays only if its text stays. */
    position = (size_t)(strstr((const char *)replay.shadow.bytes, "[^a]: a body") - (char *)replay.shadow.bytes);
    if (eq_replay_edit(&replay, position, replay.shadow.length, NULL, 0) != 0 || eq_replay_commit(&replay) != 0) {
        goto done;
    }

    result = failures ? -1 : 0;
done:
    eq_replay_close(&replay);
    return result;
}

/* --- scripted boundary edits ---------------------------------------------- */

/* One edit-and-commit: the position is `offset` bytes past the first match
 * of `anchor` (or absolute when anchor is NULL), `delete_length` bytes leave,
 * `insert` arrives. */
typedef struct eq_script_step {
    const char *anchor;
    size_t offset;
    size_t delete_length;
    const char *insert;
} eq_script_step;

typedef struct eq_script {
    const char *name;
    const char *initial;
    const eq_script_step *steps;
    size_t step_count;
} eq_script;

/* Setext underlines attach to the open paragraph, so a boundary must never
 * split them; flipping one in and out exercises the kind-change path. */
static const eq_script_step EQ_SETEXT_STEPS[] = {
    {"gamma", 0, 0, "===\n"},
    {"===", 0, 4, ""},
    {"alpha", 5, 0, "\n---"},
    {"\n---", 0, 4, ""},
};

/* Removing the blank line makes `tail` a lazy continuation of the quoted
 * paragraph; restoring it splits the paragraph again. */
static const eq_script_step EQ_LAZY_STEPS[] = {
    {"\ntail", 0, 1, ""},
    {"tail", 0, 0, "\n"},
};

/* An unclosed fence swallows everything to EOF (reparse-to-EOF degradation);
 * closing it releases the suffix again. */
static const eq_script_step EQ_FENCE_STEPS[] = {
    {NULL, (size_t)-1, 0, "swallowed\n"},
    {NULL, (size_t)-1, 0, "```\n\nafter\n"},
    {"first", 0, 0, "zero\n\n"},
    {"```\ncode", 0, 3, "~~~"},
};

static const eq_script_step EQ_HTML_STEPS[] = {
    {NULL, (size_t)-1, 0, "y\n"},
    {NULL, (size_t)-1, 0, "</pre>\n\nafter\n"},
    {"x\ny", 0, 1, "q"},
};

static const eq_script_step EQ_DIRECTIVE_STEPS[] = {
    {NULL, (size_t)-1, 0, "more\n"},
    {NULL, (size_t)-1, 0, ":::\n\nafter\n"},
    {"body", 0, 4, "edited"},
};

/* Breaking and restoring the delimiter row retypes table <-> paragraph, and
 * the leading `intro` line keeps the table fused to a split-off header
 * paragraph (never a restart point). */
static const eq_script_step EQ_TABLE_STEPS[] = {
    {"tail", 0, 4, "coda"},
    {"| - |", 2, 1, "x"},
    {"| x |", 2, 1, "-"},
    {"intro", 0, 6, ""},
    {"h1", 0, 0, "intro\n"},
};

/* Tightness flips must surface as List-node changes while the items
 * transplant. */
static const eq_script_step EQ_TIGHTNESS_STEPS[] = {
    {"- b", 0, 0, "\n"},
    {"\n- b", 0, 1, ""},
    {"tail", 0, 0, "- c\n\n"},
};

/* A document with no interior clean boundary: every edit reparses the lone
 * paragraph. */
static const eq_script_step EQ_NO_BLANK_STEPS[] = {
    {"l3", 0, 2, "L3!"},
    {NULL, (size_t)-1, 0, "l5\n"},
    {NULL, 0, 3, ""},
};

/* Cross-boundary reference edits: touching the paragraphs must not disturb
 * resolution, editing the definition re-resolves both sides (full-reparse
 * fallback), and a same-text definition rewrite must stay incremental. */
static const eq_script_step EQ_CROSS_REF_STEPS[] = {
    {"here", 0, 4, "there"},
    {"/one", 0, 4, "/two"},
    {"[l]: /two", 0, 9, "[l]: /two"},
    {"tail", 0, 0, "coda "},
};

/* Restarts at byte zero re-encounter the BOM; edits after it must not. */
static const eq_script_step EQ_BOM_STEPS[] = {
    {"beta", 0, 4, "BETA"},
    {"alpha", 0, 0, "pre "},
};

/* Head-region edits before the first document child (owner-0 anchoring). */
static const eq_script_step EQ_BLANK_HEAD_STEPS[] = {
    {NULL, 1, 0, "x\n"},
    {"x\n", 0, 2, ""},
    {NULL, 0, 0, "[l]: /url\n"},
    {"[l]: /url\n", 0, 10, ""},
};

static const eq_script_step EQ_CRLF_STEPS[] = {
    {"next", 0, 4, "NEXT"},
    {NULL, (size_t)-1, 0, "\r\ntail\r\n"},
    {"second", 0, 0, "2nd "},
};

/* An insertion that begins with '\n' exactly at a clean boundary fuses with
 * a lone-CR terminator at the end of the untouched prefix: the boundary no
 * longer starts a line in the new text, so the restart must back off one
 * entry rather than feed from inside the new CRLF pair. */
static const eq_script_step EQ_CR_FUSION_STEPS[] = {
    {"---", 0, 0, "\n![a](b)"},
};

/* A block that closes in the middle of its own first line (the processing
 * instruction here) dates its end to the line before it, so a restart whose
 * first staged line closes one must see the prefix line's terminator-stripped
 * length rather than a fresh parser's zero. */
static const eq_script_step EQ_PREV_LINE_LENGTH_STEPS[] = {
    {"x?>", 0, 1, "y"},
};

/* Lengthening the line just before a resync boundary (trailing spaces on the
 * closing fence) must re-date a transplanted first suffix child that closed
 * inside its own first line: its end borrows that staged line's length. */
static const eq_script_step EQ_TRANSPLANT_END_STEPS[] = {
    {"```\n<?", 3, 0, "  "},
};

/* Deleting the whole tail at a clean boundary leaves nothing to feed. */
static const eq_script_step EQ_TAIL_DELETE_STEPS[] = {
    {"\n\nbeta", 2, (size_t)-1, ""},
    {NULL, (size_t)-1, 0, "\ngamma\n"},
    {"gamma", 0, (size_t)-1, ""},
};

/* Winner-delta commits: retargeting the only definition must re-refine the
 * dependent units in place — a quoted paragraph and a heading among them
 * (ancestor revision bubbling) — deleting it degrades every dependent to
 * literal text, and a later definition resolves the recorded misses. */
static const eq_script_step EQ_REF_RETARGET_STEPS[] = {
    {"/one", 0, 4, "/two"},
    {"tail", 0, 4, "coda"},
    {"[l]: /two\n", 0, 10, ""},
    {NULL, 0, 0, "[l]: /three\n"},
};

/* A label that resolves nowhere at first: the miss against an empty map must
 * still record the dependency, so the definition's arrival re-refines the
 * paragraph, and its departure degrades it again. */
static const eq_script_step EQ_REF_NEW_LABEL_STEPS[] = {
    {NULL, (size_t)-1, 0, "\n[x]: /url \"t\"\n"},
    {"/url", 0, 4, "/other"},
    {"\n[x]: /other \"t\"\n", 0, (size_t)-1, ""},
};

/* Editing a losing duplicate changes the definition sequence but not the
 * winner: the commit reconciles with no dependent re-runs. Deleting the
 * winner then re-elects the edited duplicate. */
static const eq_script_step EQ_REF_LOSING_DUP_STEPS[] = {
    {"/lose", 0, 5, "/altered"},
    {"[l]: /win\n", 0, 10, ""},
    {"[l]: /altered", 0, 0, "x "},
};

/* Inserting more definitions than the stale region held exhausts the vacated
 * order span and renumbers the whole map; the new [c] definition also
 * resolves a recorded miss. */
static const eq_script_step EQ_REF_RENUMBER_STEPS[] = {
    {"middle", 0, 0, "[c]: /3\n[d]: /4\n[e]: /5\n\n"},
    {"[c]: /3", 6, 1, "9"},
};

/* A dependent inside a table cell cannot be rebuilt per-unit; the commit
 * must fall back to a full reparse and still match one-shot output. */
static const eq_script_step EQ_REF_TABLE_CELL_STEPS[] = {
    {"/one", 0, 4, "/two"},
    {"[l]: /two\n", 0, 10, ""},
};

/* With footnotes enabled, a definition whose label normalizes to "^n" decides
 * whether [^n] parses as a link or as a footnote reference: the winner delta
 * must flip the node kind both ways and keep the footnote index honest. */
static const eq_script_step EQ_REF_CARET_FLIP_STEPS[] = {
    {"[ ^n]: /url\n", 0, 12, ""},
    {NULL, (size_t)-1, 0, "[ ^n]: /url\n"},
};

/* Incremental footnote-site maintenance: an interior edit between the
 * reference clusters (an empty staged run classified between prefix and
 * suffix sites), a mid-document reference and definition whose arrival and
 * departure cascade every later ordinal through the staged run, a combined
 * commit whose staged region introduces sites while suffix dependents
 * rebuild (the staged run must precede the clone runs), winner-delta
 * rebuilds of paragraphs holding references (top-level and quoted), and a
 * definition deleted from the tail cluster. The staged insert ends in a
 * plain paragraph so the resync lands right after it rather than riding an
 * open footnote definition past the dependents. */
static const eq_script_step EQ_FOOTNOTE_SITES_STEPS[] = {
    {"filler", 0, 6, "middle"},
    {"middle", 0, 0, "lead[^n]\n\n[^n]: new\n\n"},
    {"lead[^n]", 0, 21, ""},
    {"/one", 0, 4, "/two\n\nmid[^m]\n\n[^m]: m body\n\nplain"},
    {"[l]: /two\n", 0, 10, ""},
    {NULL, (size_t)-1, 0, "\n[l]: /three\n"},
    {"[^b]: second\n", 0, 13, ""},
    {"[l]: /three\n", 0, 12, "coda[^e]\n\n[^e]: e body\n\n[l]: /four\n"},
};

static const eq_script EQ_BOUNDARY_SCRIPTS[] = {
    {"setext_flip", "alpha\n\nbeta\ngamma\n", EQ_SETEXT_STEPS, sizeof(EQ_SETEXT_STEPS) / sizeof(*EQ_SETEXT_STEPS)},
    {"lazy_continuation", "> quote\n\ntail\n", EQ_LAZY_STEPS, sizeof(EQ_LAZY_STEPS) / sizeof(*EQ_LAZY_STEPS)},
    {"unclosed_fence", "first\n\n```\ncode\n", EQ_FENCE_STEPS, sizeof(EQ_FENCE_STEPS) / sizeof(*EQ_FENCE_STEPS)},
    {"unclosed_html", "first\n\n<pre>\nx\n", EQ_HTML_STEPS, sizeof(EQ_HTML_STEPS) / sizeof(*EQ_HTML_STEPS)},
    {"unclosed_directive",
     ":::note[L]\nbody\n",
     EQ_DIRECTIVE_STEPS,
     sizeof(EQ_DIRECTIVE_STEPS) / sizeof(*EQ_DIRECTIVE_STEPS)},
    {"table_delimiter",
     "intro\n| h1 | h2 |\n| - | - |\n| c1 | c2 |\n\ntail\n",
     EQ_TABLE_STEPS,
     sizeof(EQ_TABLE_STEPS) / sizeof(*EQ_TABLE_STEPS)},
    {"list_tightness",
     "- a\n- b\n\ntail\n",
     EQ_TIGHTNESS_STEPS,
     sizeof(EQ_TIGHTNESS_STEPS) / sizeof(*EQ_TIGHTNESS_STEPS)},
    {"no_blank_document",
     "l1\nl2\nl3\nl4\n",
     EQ_NO_BLANK_STEPS,
     sizeof(EQ_NO_BLANK_STEPS) / sizeof(*EQ_NO_BLANK_STEPS)},
    {"cross_boundary_refs",
     "[l]: /one\n\nsee [x][l] here\n\ntail [y][l]\n",
     EQ_CROSS_REF_STEPS,
     sizeof(EQ_CROSS_REF_STEPS) / sizeof(*EQ_CROSS_REF_STEPS)},
    {"bom_restart",
     "\xef\xbb\xbf"
     "alpha\n\nbeta\n",
     EQ_BOM_STEPS,
     sizeof(EQ_BOM_STEPS) / sizeof(*EQ_BOM_STEPS)},
    {"blank_head", "\n\n\nalpha\n", EQ_BLANK_HEAD_STEPS, sizeof(EQ_BLANK_HEAD_STEPS) / sizeof(*EQ_BLANK_HEAD_STEPS)},
    {"crlf_lines", "para\r\nsecond\r\n\r\nnext\r\n", EQ_CRLF_STEPS, sizeof(EQ_CRLF_STEPS) / sizeof(*EQ_CRLF_STEPS)},
    {"cr_fusion_head", "\r---\n", EQ_CR_FUSION_STEPS, sizeof(EQ_CR_FUSION_STEPS) / sizeof(*EQ_CR_FUSION_STEPS)},
    {"cr_fusion_interior",
     "alpha\n\nbeta\n\n\r---\n",
     EQ_CR_FUSION_STEPS,
     sizeof(EQ_CR_FUSION_STEPS) / sizeof(*EQ_CR_FUSION_STEPS)},
    {"restart_prev_line_length",
     "```\ncode\n```\n<?pi x?>\n\ntail\n",
     EQ_PREV_LINE_LENGTH_STEPS,
     sizeof(EQ_PREV_LINE_LENGTH_STEPS) / sizeof(*EQ_PREV_LINE_LENGTH_STEPS)},
    {"transplant_end_column",
     "```\ncode\n```\n<?pi x?>\n\ntail\n",
     EQ_TRANSPLANT_END_STEPS,
     sizeof(EQ_TRANSPLANT_END_STEPS) / sizeof(*EQ_TRANSPLANT_END_STEPS)},
    {"tail_delete",
     "alpha\n\nbeta\n",
     EQ_TAIL_DELETE_STEPS,
     sizeof(EQ_TAIL_DELETE_STEPS) / sizeof(*EQ_TAIL_DELETE_STEPS)},
    {"ref_retarget",
     "[l]: /one\n"
     "\n"
     "alpha [a][l]\n"
     "\n"
     "> beta [b][l]\n"
     "\n"
     "# head [h][l]\n"
     "\n"
     "tail\n",
     EQ_REF_RETARGET_STEPS,
     sizeof(EQ_REF_RETARGET_STEPS) / sizeof(*EQ_REF_RETARGET_STEPS)},
    {"ref_new_label",
     "alpha [x]\n\nbeta\n",
     EQ_REF_NEW_LABEL_STEPS,
     sizeof(EQ_REF_NEW_LABEL_STEPS) / sizeof(*EQ_REF_NEW_LABEL_STEPS)},
    {"ref_losing_dup",
     "[l]: /win\n"
     "\n"
     "see [s][l]\n"
     "\n"
     "[l]: /lose\n",
     EQ_REF_LOSING_DUP_STEPS,
     sizeof(EQ_REF_LOSING_DUP_STEPS) / sizeof(*EQ_REF_LOSING_DUP_STEPS)},
    {"ref_renumber",
     "[a]: /1\n"
     "\n"
     "middle [r][c]\n"
     "\n"
     "[b]: /2\n"
     "\n"
     "para [p][a] [q][b]\n",
     EQ_REF_RENUMBER_STEPS,
     sizeof(EQ_REF_RENUMBER_STEPS) / sizeof(*EQ_REF_RENUMBER_STEPS)},
    {"ref_table_cell",
     "[l]: /one\n"
     "\n"
     "| h |\n"
     "| - |\n"
     "| [x][l] |\n",
     EQ_REF_TABLE_CELL_STEPS,
     sizeof(EQ_REF_TABLE_CELL_STEPS) / sizeof(*EQ_REF_TABLE_CELL_STEPS)},
    {"ref_caret_flip",
     "body [^n]\n"
     "\n"
     "[ ^n]: /url\n",
     EQ_REF_CARET_FLIP_STEPS,
     sizeof(EQ_REF_CARET_FLIP_STEPS) / sizeof(*EQ_REF_CARET_FLIP_STEPS)},
    {"footnote_sites",
     "head\n"
     "\n"
     "[l]: /one\n"
     "\n"
     "alpha[^a] uses [u][l]\n"
     "\n"
     "> quote[^q] sees [q][l]\n"
     "\n"
     "filler\n"
     "\n"
     "omega[^b] and again[^b]\n"
     "\n"
     "[^a]: first\n"
     "\n"
     "[^b]: second\n"
     "\n"
     "[^q]: quoted\n",
     EQ_FOOTNOTE_SITES_STEPS,
     sizeof(EQ_FOOTNOTE_SITES_STEPS) / sizeof(*EQ_FOOTNOTE_SITES_STEPS)},
};

static int eq_run_script(const eq_script *script) {
    eq_replay replay;
    markdown_core_parse_options options;
    size_t i;
    int result = -1;

    markdown_core_parse_options_init(&options);
    if (eq_replay_open(&replay, script->name, &options) != 0) {
        return -1;
    }
    if (eq_replay_edit(&replay, 0, 0, (const uint8_t *)script->initial, strlen(script->initial)) != 0 ||
        eq_replay_commit(&replay) != 0) {
        goto done;
    }
    for (i = 0; i < script->step_count; i++) {
        const eq_script_step *step = &script->steps[i];
        size_t position;
        size_t delete_length = step->delete_length;
        if (step->anchor) {
            const char *found = strstr((const char *)replay.shadow.bytes, step->anchor);
            if (!found) {
                eq_fail(script->name, "script anchor is missing from the text");
                goto done;
            }
            position = (size_t)(found - (const char *)replay.shadow.bytes) + step->offset;
        } else {
            position = step->offset == (size_t)-1 ? replay.shadow.length : step->offset;
        }
        if (delete_length == (size_t)-1) {
            delete_length = replay.shadow.length - position;
        }
        if (eq_replay_edit(
                &replay,
                position,
                position + delete_length,
                (const uint8_t *)step->insert,
                strlen(step->insert)
            ) != 0 ||
            eq_replay_commit(&replay) != 0) {
            goto done;
        }
    }
    result = 0;
done:
    eq_replay_close(&replay);
    return result;
}

/* Adversarial boundary fixtures: constructs whose parse can reach across a
 * would-be restart point must never desynchronize an incremental commit. */
static int case_boundary_edits(void) {
    size_t i;
    for (i = 0; i < sizeof(EQ_BOUNDARY_SCRIPTS) / sizeof(*EQ_BOUNDARY_SCRIPTS); i++) {
        eq_run_script(&EQ_BOUNDARY_SCRIPTS[i]);
    }
    return failures ? -1 : 0;
}

/* --- entry point ---------------------------------------------------------- */

static const char *const EQ_CASES[] =
    {"canonical", "spec", "random_edits", "link_ref_edits", "footnote_edits", "boundary_edits"};

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
    } else if (case_name && strcmp(case_name, "random_edits") == 0 && spec_path) {
        case_random_edits(spec_path);
    } else if (case_name && strcmp(case_name, "link_ref_edits") == 0) {
        case_link_ref_edits();
    } else if (case_name && strcmp(case_name, "footnote_edits") == 0) {
        case_footnote_edits();
    } else if (case_name && strcmp(case_name, "boundary_edits") == 0) {
        case_boundary_edits();
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
