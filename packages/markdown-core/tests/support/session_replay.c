#include "session_replay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"

static int sr_fail(sr_replay *replay, const char *message) {
    replay->report(replay->user, replay->context, message);
    return -1;
}

/* --- shadow text -------------------------------------------------------- */

static int sr_text_splice(sr_text *text, size_t start, size_t end, const uint8_t *insert, size_t insert_length) {
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

/* --- delta mirror ------------------------------------------------------- */

static sr_mirror_entry *sr_mirror_find(sr_mirror *mirror, markdown_core_node_id id) {
    size_t i;
    for (i = 0; i < mirror->count; i++) {
        if (mirror->entries[i].id == id) {
            return &mirror->entries[i];
        }
    }
    return NULL;
}

static int sr_mirror_insert(sr_mirror *mirror, markdown_core_node_id id, uint64_t revision) {
    if (mirror->count == mirror->capacity) {
        size_t capacity = mirror->capacity ? mirror->capacity * 2 : 64;
        sr_mirror_entry *grown = (sr_mirror_entry *)realloc(mirror->entries, capacity * sizeof(*grown));
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

static void sr_mirror_remove(sr_mirror *mirror, sr_mirror_entry *entry) {
    *entry = mirror->entries[mirror->count - 1];
    mirror->count--;
}

/* --- replay harness ------------------------------------------------------ */

int sr_replay_open(
    sr_replay *replay,
    const char *context,
    const markdown_core_parse_options *options,
    sr_report_fn report,
    void *user
) {
    markdown_core_error *error = NULL;
    memset(replay, 0, sizeof(*replay));
    replay->context = context;
    replay->options = options;
    replay->report = report;
    replay->user = user;
    replay->session = markdown_core_session_open(options, &error);
    if (!replay->session) {
        markdown_core_error_free(error);
        return sr_fail(replay, "session open failed");
    }
    /* The shadow buffer exists even while empty so scripted drivers can
     * strstr into it before the first edit. */
    if (sr_text_splice(&replay->shadow, 0, 0, NULL, 0) != 0) {
        return sr_fail(replay, "shadow allocation failed");
    }
    /* Revision 0 (empty document) seeds the mirror. */
    {
        const markdown_core_document *document = markdown_core_session_document(replay->session);
        const markdown_core_node *root = markdown_core_document_root(document);
        if (!root ||
            sr_mirror_insert(&replay->mirror, markdown_core_node_get_id(root), markdown_core_node_get_revision(root)) !=
                0) {
            return sr_fail(replay, "empty session has no addressable root");
        }
    }
    return 0;
}

void sr_replay_close(sr_replay *replay) {
    markdown_core_session_free(replay->session);
    free(replay->shadow.bytes);
    free(replay->mirror.entries);
    memset(replay, 0, sizeof(*replay));
}

int sr_replay_edit(sr_replay *replay, size_t start, size_t end, const uint8_t *bytes, size_t length) {
    markdown_core_error *error = NULL;
    if (!markdown_core_session_edit(replay->session, start, end, bytes, length, &error)) {
        markdown_core_error_free(error);
        return sr_fail(replay, "session edit failed");
    }
    if (sr_text_splice(&replay->shadow, start, end, bytes, length) != 0) {
        return sr_fail(replay, "shadow splice allocation failed");
    }
    return 0;
}

static int sr_delta_disjoint(sr_replay *replay, markdown_core_delta *changes) {
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
                        return sr_fail(replay, "delta arrays repeat an id");
                    }
                }
            }
        }
    }
    return 0;
}

static int sr_apply_delta(sr_replay *replay, markdown_core_delta *changes, uint64_t expected_after) {
    const markdown_core_node_id *ids;
    size_t count;
    size_t i;
    uint64_t before;
    uint64_t after;

    markdown_core_delta_revisions(changes, &before, &after);
    if (after != expected_after) {
        return sr_fail(replay, "delta revisions disagree with the session");
    }

    if (sr_delta_disjoint(replay, changes) != 0) {
        return -1;
    }

    count = markdown_core_delta_removed(changes, &ids);
    for (i = 0; i < count; i++) {
        sr_mirror_entry *entry = sr_mirror_find(&replay->mirror, ids[i]);
        if (!entry) {
            return sr_fail(replay, "delta removed an id the mirror never saw");
        }
        sr_mirror_remove(&replay->mirror, entry);
    }

    count = markdown_core_delta_added(changes, &ids);
    for (i = 0; i < count; i++) {
        if (sr_mirror_find(&replay->mirror, ids[i])) {
            return sr_fail(replay, "delta added an id that already exists");
        }
        if (sr_mirror_insert(&replay->mirror, ids[i], after) != 0) {
            return sr_fail(replay, "mirror allocation failed");
        }
    }

    count = markdown_core_delta_changed(changes, &ids);
    for (i = 0; i < count; i++) {
        sr_mirror_entry *entry = sr_mirror_find(&replay->mirror, ids[i]);
        if (!entry) {
            return sr_fail(replay, "delta changed an id the mirror never saw");
        }
        entry->revision = after;
    }

    count = markdown_core_delta_bubbled(changes, &ids);
    for (i = 0; i < count; i++) {
        sr_mirror_entry *entry = sr_mirror_find(&replay->mirror, ids[i]);
        if (!entry) {
            return sr_fail(replay, "delta bubbled an id the mirror never saw");
        }
        entry->revision = after;
    }

    return 0;
}

/* --- footnote query equivalence ------------------------------------------ */

typedef struct sr_id_list {
    markdown_core_node_id *ids;
    size_t count;
    size_t capacity;
    int failed;
} sr_id_list;

static int sr_id_collect_visit(const markdown_core_node *node, void *context) {
    sr_id_list *list = (sr_id_list *)context;
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

typedef struct sr_ordinal {
    markdown_core_node_id id;
    size_t position;
} sr_ordinal;

static int sr_ordinal_compare(const void *a, const void *b) {
    markdown_core_node_id ia = ((const sr_ordinal *)a)->id;
    markdown_core_node_id ib = ((const sr_ordinal *)b)->id;
    return ia < ib ? -1 : (ia > ib ? 1 : 0);
}

/* Walk position of `id`, or SIZE_MAX for id 0 and ids outside the tree. */
static size_t sr_position_of(const sr_ordinal *ordinals, size_t count, markdown_core_node_id id) {
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

static sr_ordinal *sr_ordinals_build(const sr_id_list *list) {
    sr_ordinal *ordinals = (sr_ordinal *)malloc((list->count ? list->count : 1) * sizeof(*ordinals));
    size_t i;
    if (!ordinals) {
        return NULL;
    }
    for (i = 0; i < list->count; i++) {
        ordinals[i].id = list->ids[i];
        ordinals[i].position = i;
    }
    qsort(ordinals, list->count, sizeof(*ordinals), sr_ordinal_compare);
    return ordinals;
}

/* With footnotes enabled, every commit's numbering, resolution, and
 * back-reference answers must equal a fresh session's on the same text.
 * Node identity maps positionally: the dumps already compared equal, so both
 * trees walk the same shape and the i-th node of one corresponds to the i-th
 * node of the other. */
static int sr_check_footnote_queries(sr_replay *replay) {
    markdown_core_session *fresh = NULL;
    markdown_core_error *error = NULL;
    sr_id_list mine = {NULL, 0, 0, 0};
    sr_id_list theirs = {NULL, 0, 0, 0};
    sr_ordinal *mine_ordinals = NULL;
    sr_ordinal *theirs_ordinals = NULL;
    int result = -1;
    size_t i;

    fresh = markdown_core_session_open(replay->options, &error);
    if (!fresh || !markdown_core_session_edit(fresh, 0, 0, replay->shadow.bytes, replay->shadow.length, &error) ||
        !markdown_core_session_commit(fresh, NULL, &error)) {
        markdown_core_error_free(error);
        sr_fail(replay, "fresh footnote reference session failed");
        goto done;
    }
    if (ts_ast_walk(
            markdown_core_document_root(markdown_core_session_document(replay->session)),
            sr_id_collect_visit,
            &mine
        ) < 0 ||
        mine.failed ||
        ts_ast_walk(markdown_core_document_root(markdown_core_session_document(fresh)), sr_id_collect_visit, &theirs) <
            0 ||
        theirs.failed) {
        sr_fail(replay, "footnote walk failed to allocate");
        goto done;
    }
    if (mine.count != theirs.count) {
        sr_fail(replay, "footnote sessions walk different node counts");
        goto done;
    }
    mine_ordinals = sr_ordinals_build(&mine);
    theirs_ordinals = sr_ordinals_build(&theirs);
    if (!mine_ordinals || !theirs_ordinals) {
        sr_fail(replay, "footnote ordinal map failed to allocate");
        goto done;
    }

    for (i = 0; i < mine.count; i++) {
        markdown_core_footnote_info a;
        markdown_core_footnote_info b;
        bool found_a = markdown_core_session_footnote_info(replay->session, mine.ids[i], &a);
        bool found_b = markdown_core_session_footnote_info(fresh, theirs.ids[i], &b);
        if (found_a != found_b) {
            sr_fail(replay, "footnote info presence diverged from a fresh session");
            goto done;
        }
        if (!found_a) {
            continue;
        }
        if (a.number != b.number || a.reference_ordinal != b.reference_ordinal ||
            a.reference_count != b.reference_count ||
            sr_position_of(mine_ordinals, mine.count, a.definition) !=
                sr_position_of(theirs_ordinals, theirs.count, b.definition)) {
            sr_fail(replay, "footnote info diverged from a fresh session");
            goto done;
        }
    }

    {
        const markdown_core_node_id *a_ids;
        const markdown_core_node_id *b_ids;
        size_t a_count = markdown_core_session_footnotes(replay->session, &a_ids);
        size_t b_count = markdown_core_session_footnotes(fresh, &b_ids);
        if (a_count != b_count) {
            sr_fail(replay, "footnote first-use list length diverged from a fresh session");
            goto done;
        }
        for (i = 0; i < a_count; i++) {
            const markdown_core_node_id *a_refs;
            const markdown_core_node_id *b_refs;
            size_t a_refs_count;
            size_t b_refs_count;
            size_t k;
            if (sr_position_of(mine_ordinals, mine.count, a_ids[i]) !=
                sr_position_of(theirs_ordinals, theirs.count, b_ids[i])) {
                sr_fail(replay, "footnote first-use order diverged from a fresh session");
                goto done;
            }
            a_refs_count = markdown_core_session_footnote_references(replay->session, a_ids[i], &a_refs);
            b_refs_count = markdown_core_session_footnote_references(fresh, b_ids[i], &b_refs);
            if (a_refs_count != b_refs_count) {
                sr_fail(replay, "footnote back-reference count diverged from a fresh session");
                goto done;
            }
            for (k = 0; k < a_refs_count; k++) {
                if (sr_position_of(mine_ordinals, mine.count, a_refs[k]) !=
                    sr_position_of(theirs_ordinals, theirs.count, b_refs[k])) {
                    sr_fail(replay, "footnote back-reference order diverged from a fresh session");
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

/* --- verified commit ----------------------------------------------------- */

typedef struct sr_walk_state {
    sr_replay *replay;
    size_t seen;
    int failed;
} sr_walk_state;

static int sr_walk_visit(const markdown_core_node *node, void *context) {
    sr_walk_state *state = (sr_walk_state *)context;
    sr_replay *replay = state->replay;
    markdown_core_node_id id = markdown_core_node_get_id(node);
    sr_mirror_entry *entry = sr_mirror_find(&replay->mirror, id);

    state->seen++;
    if (!entry) {
        sr_fail(replay, "tree holds an id the delta stream never added");
        state->failed = 1;
        return 1;
    }
    if (entry->revision != markdown_core_node_get_revision(node)) {
        sr_fail(replay, "node revision changed without a delta notification");
        state->failed = 1;
        return 1;
    }
    if (markdown_core_session_node_by_id(replay->session, id) != node) {
        sr_fail(replay, "node_by_id disagrees with the committed tree");
        state->failed = 1;
        return 1;
    }
    return 0;
}

int sr_replay_commit(sr_replay *replay) {
    markdown_core_error *error = NULL;
    markdown_core_delta *changes = NULL;
    const markdown_core_document *document;
    const markdown_core_node *root;
    markdown_core_document *reference = NULL;
    uint8_t *session_dump = NULL;
    uint8_t *reference_dump = NULL;
    size_t session_dump_length = 0;
    size_t reference_dump_length = 0;
    sr_walk_state state;
    int result = -1;

    if (!markdown_core_session_commit(replay->session, &changes, &error)) {
        markdown_core_error_free(error);
        return sr_fail(replay, "commit failed");
    }
    if (!changes) {
        return sr_fail(replay, "commit produced no delta");
    }
    if (sr_apply_delta(replay, changes, markdown_core_session_revision(replay->session)) != 0) {
        goto done;
    }

    document = markdown_core_session_document(replay->session);
    root = markdown_core_document_root(document);
    state.replay = replay;
    state.seen = 0;
    state.failed = 0;
    if (ts_ast_walk(root, sr_walk_visit, &state) < 0 || state.failed) {
        if (!state.failed) {
            sr_fail(replay, "mirror walk failed to allocate");
        }
        goto done;
    }
    if (state.seen != replay->mirror.count) {
        sr_fail(replay, "mirror holds ids that are no longer in the tree");
        goto done;
    }

    if (!markdown_core_document_dump(document, &session_dump, &session_dump_length, &error)) {
        markdown_core_error_free(error);
        error = NULL;
        sr_fail(replay, "session dump failed");
        goto done;
    }
    reference = markdown_core_document_parse(replay->shadow.bytes, replay->shadow.length, replay->options, &error);
    if (!reference) {
        markdown_core_error_free(error);
        error = NULL;
        sr_fail(replay, "one-shot reference parse failed");
        goto done;
    }
    if (!markdown_core_document_dump(reference, &reference_dump, &reference_dump_length, &error)) {
        markdown_core_error_free(error);
        error = NULL;
        sr_fail(replay, "reference dump failed");
        goto done;
    }
    if (session_dump_length != reference_dump_length ||
        memcmp(session_dump, reference_dump, reference_dump_length) != 0) {
        sr_fail(replay, "session dump diverged from the one-shot parse");
        ts_print_line_diff(stderr, (const char *)reference_dump, (const char *)session_dump);
        goto done;
    }

    if (replay->options->footnotes && sr_check_footnote_queries(replay) != 0) {
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

/* --- edit-script interpreter --------------------------------------------- */

typedef struct sr_script_cursor {
    const uint8_t *bytes;
    size_t length;
    size_t offset;
} sr_script_cursor;

/* Operand reads past the end of the script supply zeroes, so every input
 * decodes to a complete operation sequence. */
static uint8_t sr_script_u8(sr_script_cursor *cursor) {
    if (cursor->offset >= cursor->length) {
        return 0;
    }
    return cursor->bytes[cursor->offset++];
}

static uint16_t sr_script_u16(sr_script_cursor *cursor) {
    uint16_t lo = sr_script_u8(cursor);
    uint16_t hi = sr_script_u8(cursor);
    return (uint16_t)(lo | (hi << 8));
}

int sr_script_replay(const uint8_t *script, size_t length, const char *context, sr_report_fn report, void *user) {
    sr_script_cursor cursor;
    markdown_core_parse_options options;
    bool *fields[] = {
        &options.smart_punctuation,
        &options.footnotes,
        &options.strip_html_comments,
        &options.tables,
        &options.strikethrough,
        &options.autolinks,
        &options.task_lists,
        &options.formulas,
        &options.dollar_formula_delimiters,
        &options.latex_formula_delimiters,
        &options.directives
    };
    uint16_t mask;
    size_t i;
    sr_replay replay;
    int result = -1;

    cursor.bytes = script;
    cursor.length = length;
    cursor.offset = 0;

    markdown_core_parse_options_init(&options);
    mask = sr_script_u16(&cursor);
    for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        *fields[i] = (mask >> i) & 1;
    }

    if (sr_replay_open(&replay, context, &options, report, user) != 0) {
        return -1;
    }

    while (cursor.offset < cursor.length) {
        uint8_t op = sr_script_u8(&cursor);
        switch (op & 3) {
        case 0: /* insert */
        case 2: /* replace */
        {
            size_t position = (size_t)sr_script_u16(&cursor) % (replay.shadow.length + 1);
            size_t span = 0;
            size_t insert_length;
            size_t available;
            if ((op & 3) == 2) {
                span = (size_t)sr_script_u16(&cursor) % (replay.shadow.length - position + 1);
            }
            insert_length = sr_script_u8(&cursor);
            available = cursor.length - cursor.offset;
            if (insert_length > available) {
                insert_length = available;
            }
            if (sr_replay_edit(&replay, position, position + span, cursor.bytes + cursor.offset, insert_length) != 0) {
                goto done;
            }
            cursor.offset += insert_length;
            break;
        }
        case 1: /* delete */
        {
            size_t position = (size_t)sr_script_u16(&cursor) % (replay.shadow.length + 1);
            size_t span = (size_t)sr_script_u16(&cursor) % (replay.shadow.length - position + 1);
            if (sr_replay_edit(&replay, position, position + span, NULL, 0) != 0) {
                goto done;
            }
            break;
        }
        default: /* commit */
            if (sr_replay_commit(&replay) != 0) {
                goto done;
            }
            break;
        }
    }

    if (sr_replay_commit(&replay) != 0) {
        goto done;
    }
    result = 0;
done:
    sr_replay_close(&replay);
    return result;
}
