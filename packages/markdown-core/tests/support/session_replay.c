#include "session_replay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"
#include "commit_compat.h"

/* WHAT A CONSUMER DOES. There is no engine-side id->node index: a consumer
 * that holds an id and the tree already has the node, because it meets it on
 * the walk it was doing anyway (requirement 3). These tests hold ids across an
 * edit exactly like a highlighter does, so they find nodes the same way. */
static const markdown_core_node *node_by_id(const markdown_core_node *root, markdown_core_node_id id) {
    const markdown_core_node *node = root;
    if (!root || id == 0) {
        return NULL;
    }
    for (;;) {
        if (markdown_core_node_get_id(node) == id) {
            return node;
        }
        if (markdown_core_node_get_first_child(node)) {
            node = markdown_core_node_get_first_child(node);
            continue;
        }
        while (node != root && !markdown_core_node_get_next_sibling(node)) {
            node = markdown_core_node_get_parent(node);
        }
        if (node == root) {
            return NULL;
        }
        node = markdown_core_node_get_next_sibling(node);
    }
}

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
    replay->session = markdown_core_document_open(options, &error);
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
        const markdown_core_document *document = replay->session;
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
    markdown_core_document_release(replay->session);
    free(replay->shadow.bytes);
    free(replay->mirror.entries);
    memset(replay, 0, sizeof(*replay));
}

/* The shadow IS the text now. An edit splices the harness's own buffer and
 * nothing else; the document only ever sees whole text, at commit. */
int sr_replay_edit(sr_replay *replay, size_t start, size_t end, const uint8_t *bytes, size_t length) {
    if (sr_text_splice(&replay->shadow, start, end, bytes, length) != 0) {
        return sr_fail(replay, "shadow splice allocation failed");
    }
    return 0;
}

/* THE PATH-B CONSUMER, and therefore the gate on what `diffs` promises: a
 * mirror keyed by MarkupID, maintained in ONE forward pass over the list.
 *
 * Everything it checks is a clause of 9.1 rather than a property of this
 * implementation: each node is named once; a retired row (parts zero) names
 * something the mirror already holds, and arrives before the parent whose
 * child it was; and a surviving node always follows its own children, so a
 * consumer that builds values bottom-up never reaches a parent early. */
static int sr_apply_delta(sr_replay *replay, markdown_core_delta *changes, uint64_t expected_after) {
    const markdown_core_diff *diffs = NULL;
    size_t count;
    size_t i;
    size_t k;
    uint64_t before;
    uint64_t after;

    markdown_core_delta_revisions(changes, &before, &after);
    if (after != expected_after) {
        return sr_fail(replay, "delta revisions disagree with the session");
    }

    count = markdown_core_delta_diffs(changes, &diffs);
    for (i = 0; i < count; i++) {
        for (k = i + 1; k < count; k++) {
            if (diffs[i].markup == diffs[k].markup) {
                return sr_fail(replay, "diffs name one node twice");
            }
        }
    }

    for (i = 0; i < count; i++) {
        sr_mirror_entry *entry = sr_mirror_find(&replay->mirror, diffs[i].markup);
        if (diffs[i].parts == 0) {
            if (!entry) {
                return sr_fail(replay, "delta retired an id the mirror never saw");
            }
            sr_mirror_remove(&replay->mirror, entry);
            continue;
        }
        if (entry) {
            entry->revision = after;
        } else if (sr_mirror_insert(&replay->mirror, diffs[i].markup, after) != 0) {
            return sr_fail(replay, "mirror allocation failed");
        }
    }

    /* Children before parents: a surviving row whose canonical parent is also
     * in the list must come first. Checked against the committed tree, which
     * is the only place the parent relation lives. */
    for (i = 0; i < count; i++) {
        const markdown_core_node *node;
        const markdown_core_node *parent;
        markdown_core_node_id parent_id;
        if (diffs[i].parts == 0) {
            continue;
        }
        node = node_by_id(markdown_core_document_root(replay->session), diffs[i].markup);
        if (!node) {
            return sr_fail(replay, "diffs name a surviving id the document does not have");
        }
        parent = markdown_core_node_get_parent(node);
        if (!parent) {
            continue;
        }
        parent_id = markdown_core_node_get_id(parent);
        for (k = 0; k < i; k++) {
            if (diffs[k].markup == parent_id) {
                return sr_fail(replay, "a parent was emitted before its own child");
            }
        }
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
    markdown_core_document *fresh = NULL;
    markdown_core_error *error = NULL;
    sr_id_list mine = {NULL, 0, 0, 0};
    sr_id_list theirs = {NULL, 0, 0, 0};
    sr_ordinal *mine_ordinals = NULL;
    sr_ordinal *theirs_ordinals = NULL;
    int result = -1;
    size_t i;

    fresh = markdown_core_document_open(replay->options, &error);
    markdown_core_document_free(fresh);
    fresh = markdown_core_document_new(mc_sv(replay->shadow.bytes, replay->shadow.length), replay->options, &error);
    if (!fresh) {
        markdown_core_error_free(error);
        sr_fail(replay, "fresh footnote reference session failed");
        goto done;
    }
    if (ts_ast_walk(markdown_core_document_root(replay->session), sr_id_collect_visit, &mine) < 0 || mine.failed ||
        ts_ast_walk(markdown_core_document_root(fresh), sr_id_collect_visit, &theirs) < 0 || theirs.failed) {
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
        bool found_a = markdown_core_document_footnote_info(
            replay->session,
            node_by_id(markdown_core_document_root(replay->session), mine.ids[i]),
            &a
        );
        bool found_b = markdown_core_document_footnote_info(
            fresh,
            node_by_id(markdown_core_document_root(fresh), theirs.ids[i]),
            &b
        );
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
        size_t a_count = markdown_core_document_footnotes(replay->session, &a_ids);
        size_t b_count = markdown_core_document_footnotes(fresh, &b_ids);
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
            a_refs_count = markdown_core_document_footnote_references(replay->session, a_ids[i], &a_refs);
            b_refs_count = markdown_core_document_footnote_references(fresh, b_ids[i], &b_refs);
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
    markdown_core_document_release(fresh);
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
    if (node_by_id(markdown_core_document_root(replay->session), id) != node) {
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

    {
        markdown_core_commit out;
        memset(&out, 0, sizeof(out));
        if (!markdown_core_document_edit(
                &replay->session,
                mc_sv(replay->shadow.bytes, replay->shadow.length),
                &out,
                &error
            )) {
            replay->session = NULL;
            markdown_core_error_free(error);
            return sr_fail(replay, "commit failed");
        }
        replay->session = out.document;
        changes = out.delta;
    }
    if (!changes) {
        return sr_fail(replay, "commit produced no delta");
    }
    if (sr_apply_delta(replay, changes, markdown_core_document_revision(replay->session)) != 0) {
        goto done;
    }

    document = replay->session;
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
    reference = markdown_core_document_new(mc_sv(replay->shadow.bytes, replay->shadow.length), replay->options, &error);
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
        if (getenv("SR_DEBUG_DUMPS")) {
            size_t di;
            fprintf(stderr, "=== text (%zu) ===\n", replay->shadow.length);
            for (di = 0; di < replay->shadow.length; di++) {
                fprintf(stderr, "%02x%s", replay->shadow.bytes[di], (di + 1) % 24 ? " " : "\n");
            }
            fprintf(
                stderr,
                "\n=== reference ===\n%s\n=== session ===\n%s\n",
                (const char *)reference_dump,
                (const char *)session_dump
            );
        }
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
        &options.tables,
        &options.strikethrough,
        &options.autolinks,
        &options.task_lists,
        &options.formulas,
        &options.directives,
        &options.cross_links,
        &options.embeds
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

bool mc_doc_open(mc_doc *doc, const markdown_core_parse_options *options, markdown_core_error **error) {
    memset(doc, 0, sizeof(*doc));
    doc->document = markdown_core_document_new(mc_sv(NULL, 0), options, error);
    return doc->document != NULL;
}

bool mc_doc_edit(mc_doc *doc, size_t start, size_t end, const void *bytes, size_t length) {
    size_t tail;
    if (start > end || end > doc->length) {
        return false;
    }
    if (doc->length - (end - start) + length + 1 > doc->capacity) {
        size_t want = (doc->length - (end - start) + length + 1) * 2;
        char *grown = (char *)realloc(doc->text, want);
        if (!grown) {
            return false;
        }
        doc->text = grown;
        doc->capacity = want;
    }
    tail = doc->length - end;
    memmove(doc->text + start + length, doc->text + end, tail);
    if (length) {
        memcpy(doc->text + start, bytes, length);
    }
    doc->length = start + length + tail;
    doc->text[doc->length] = 0;
    return true;
}

bool mc_doc_commit(mc_doc *doc, markdown_core_delta **delta, markdown_core_error **error) {
    markdown_core_commit commit;
    memset(&commit, 0, sizeof(commit));
    if (!markdown_core_document_edit(&doc->document, mc_sv(doc->text, doc->length), &commit, error)) {
        return false;
    }
    doc->document = commit.document;
    if (delta) {
        *delta = commit.delta;
    } else {
        markdown_core_delta_free(commit.delta);
    }
    return true;
}

void mc_doc_close(mc_doc *doc) {
    markdown_core_document_free(doc->document);
    free(doc->text);
    memset(doc, 0, sizeof(*doc));
}
