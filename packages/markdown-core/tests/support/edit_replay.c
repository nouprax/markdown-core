#include "edit_replay.h"

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

static int er_fail(er_replay *replay, const char *message) {
    replay->report(replay->user, replay->context, message);
    return -1;
}

/* --- shadow text -------------------------------------------------------- */

static int er_text_splice(er_text *text, size_t start, size_t end, const uint8_t *insert, size_t insert_length) {
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

static er_mirror_entry *er_mirror_find(er_mirror *mirror, markdown_core_node_id id) {
    size_t i;
    for (i = 0; i < mirror->count; i++) {
        if (mirror->entries[i].id == id) {
            return &mirror->entries[i];
        }
    }
    return NULL;
}

static int er_mirror_insert(er_mirror *mirror, markdown_core_node_id id, uint64_t revision) {
    if (mirror->count == mirror->capacity) {
        size_t capacity = mirror->capacity ? mirror->capacity * 2 : 64;
        er_mirror_entry *grown = (er_mirror_entry *)realloc(mirror->entries, capacity * sizeof(*grown));
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

static void er_mirror_remove(er_mirror *mirror, er_mirror_entry *entry) {
    *entry = mirror->entries[mirror->count - 1];
    mirror->count--;
}

/* --- replay harness ------------------------------------------------------ */

int er_replay_open(
    er_replay *replay,
    const char *context,
    const markdown_core_parse_options *options,
    er_report_fn report,
    void *user
) {
    markdown_core_error *error = NULL;
    memset(replay, 0, sizeof(*replay));
    replay->context = context;
    replay->options = options;
    replay->report = report;
    replay->user = user;
    replay->document = markdown_core_document_new(mc_sv("", 0), options, &error);
    if (!replay->document) {
        markdown_core_error_free(error);
        return er_fail(replay, "document open failed");
    }
    /* The shadow buffer exists even while empty so scripted drivers can
     * strstr into it before the first edit. */
    if (er_text_splice(&replay->shadow, 0, 0, NULL, 0) != 0) {
        return er_fail(replay, "shadow allocation failed");
    }
    /* Revision 0 (empty document) seeds the mirror. */
    {
        const markdown_core_document *document = replay->document;
        const markdown_core_node *root = markdown_core_document_root(document);
        if (!root ||
            er_mirror_insert(&replay->mirror, markdown_core_node_get_id(root), markdown_core_node_get_revision(root)) !=
                0) {
            return er_fail(replay, "empty document has no addressable root");
        }
    }
    return 0;
}

void er_replay_close(er_replay *replay) {
    markdown_core_document_free(replay->document);
    free(replay->shadow.bytes);
    free(replay->mirror.entries);
    memset(replay, 0, sizeof(*replay));
}

/* The shadow IS the text now. An edit splices the harness's own buffer and
 * nothing else; the document only ever sees whole text, at commit. */
int er_replay_edit(er_replay *replay, size_t start, size_t end, const uint8_t *bytes, size_t length) {
    if (er_text_splice(&replay->shadow, start, end, bytes, length) != 0) {
        return er_fail(replay, "shadow splice allocation failed");
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
static int er_apply_delta(er_replay *replay, markdown_core_delta *changes, uint64_t expected_after) {
    const markdown_core_diff *diffs = NULL;
    size_t count;
    size_t i;
    size_t k;
    uint64_t before;
    uint64_t after;

    markdown_core_delta_revisions(changes, &before, &after);
    if (after != expected_after) {
        return er_fail(replay, "delta revisions disagree with the document");
    }

    count = markdown_core_delta_diffs(changes, &diffs);
    for (i = 0; i < count; i++) {
        for (k = i + 1; k < count; k++) {
            if (diffs[i].markup == diffs[k].markup) {
                return er_fail(replay, "diffs name one node twice");
            }
        }
    }

    for (i = 0; i < count; i++) {
        er_mirror_entry *entry = er_mirror_find(&replay->mirror, diffs[i].markup);
        if (diffs[i].parts == 0) {
            if (!entry) {
                return er_fail(replay, "delta retired an id the mirror never saw");
            }
            er_mirror_remove(&replay->mirror, entry);
            continue;
        }
        if (entry) {
            entry->revision = after;
        } else if (er_mirror_insert(&replay->mirror, diffs[i].markup, after) != 0) {
            return er_fail(replay, "mirror allocation failed");
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
        node = node_by_id(markdown_core_document_root(replay->document), diffs[i].markup);
        if (!node) {
            return er_fail(replay, "diffs name a surviving id the document does not have");
        }
        parent = markdown_core_node_get_parent(node);
        if (!parent) {
            continue;
        }
        parent_id = markdown_core_node_get_id(parent);
        for (k = 0; k < i; k++) {
            if (diffs[k].markup == parent_id) {
                return er_fail(replay, "a parent was emitted before its own child");
            }
        }
    }

    return 0;
}

/* --- verified commit ----------------------------------------------------- */

typedef struct er_walk_state {
    er_replay *replay;
    size_t seen;
    int failed;
} er_walk_state;

static int er_walk_visit(const markdown_core_node *node, void *context) {
    er_walk_state *state = (er_walk_state *)context;
    er_replay *replay = state->replay;
    markdown_core_node_id id = markdown_core_node_get_id(node);
    er_mirror_entry *entry = er_mirror_find(&replay->mirror, id);

    state->seen++;
    if (!entry) {
        er_fail(replay, "tree holds an id the delta stream never added");
        state->failed = 1;
        return 1;
    }
    if (entry->revision != markdown_core_node_get_revision(node)) {
        er_fail(replay, "node revision changed without a delta notification");
        state->failed = 1;
        return 1;
    }
    if (node_by_id(markdown_core_document_root(replay->document), id) != node) {
        er_fail(replay, "node_by_id disagrees with the committed tree");
        state->failed = 1;
        return 1;
    }
    return 0;
}

int er_replay_commit(er_replay *replay) {
    markdown_core_error *error = NULL;
    markdown_core_delta *changes = NULL;
    const markdown_core_document *document;
    const markdown_core_node *root;
    markdown_core_document *reference = NULL;
    uint8_t *edited_dump = NULL;
    uint8_t *reference_dump = NULL;
    size_t document_dump_length = 0;
    size_t reference_dump_length = 0;
    er_walk_state state;
    int result = -1;

    {
        markdown_core_commit out;
        memset(&out, 0, sizeof(out));
        if (!markdown_core_document_edit(
                &replay->document,
                mc_sv(replay->shadow.bytes, replay->shadow.length),
                &out,
                &error
            )) {
            replay->document = NULL;
            markdown_core_error_free(error);
            return er_fail(replay, "commit failed");
        }
        replay->document = out.document;
        changes = out.delta;
    }
    if (!changes) {
        return er_fail(replay, "commit produced no delta");
    }
    if (er_apply_delta(replay, changes, markdown_core_document_revision(replay->document)) != 0) {
        goto done;
    }

    document = replay->document;
    root = markdown_core_document_root(document);
    state.replay = replay;
    state.seen = 0;
    state.failed = 0;
    if (ts_ast_walk(root, er_walk_visit, &state) < 0 || state.failed) {
        if (!state.failed) {
            er_fail(replay, "mirror walk failed to allocate");
        }
        goto done;
    }
    if (state.seen != replay->mirror.count) {
        er_fail(replay, "mirror holds ids that are no longer in the tree");
        goto done;
    }

    if (!markdown_core_document_dump(document, &edited_dump, &document_dump_length, &error)) {
        markdown_core_error_free(error);
        error = NULL;
        er_fail(replay, "document dump failed");
        goto done;
    }
    reference = markdown_core_document_new(mc_sv(replay->shadow.bytes, replay->shadow.length), replay->options, &error);
    if (!reference) {
        markdown_core_error_free(error);
        error = NULL;
        er_fail(replay, "one-shot reference parse failed");
        goto done;
    }
    if (!markdown_core_document_dump(reference, &reference_dump, &reference_dump_length, &error)) {
        markdown_core_error_free(error);
        error = NULL;
        er_fail(replay, "reference dump failed");
        goto done;
    }
    if (document_dump_length != reference_dump_length ||
        memcmp(edited_dump, reference_dump, reference_dump_length) != 0) {
        er_fail(replay, "document dump diverged from the one-shot parse");
        ts_print_line_diff(stderr, (const char *)reference_dump, (const char *)edited_dump);
        if (getenv("SR_DEBUG_DUMPS")) {
            size_t di;
            fprintf(stderr, "=== text (%zu) ===\n", replay->shadow.length);
            for (di = 0; di < replay->shadow.length; di++) {
                fprintf(stderr, "%02x%s", replay->shadow.bytes[di], (di + 1) % 24 ? " " : "\n");
            }
            fprintf(
                stderr,
                "\n=== reference ===\n%s\n=== document ===\n%s\n",
                (const char *)reference_dump,
                (const char *)edited_dump
            );
        }
        goto done;
    }

    result = 0;
done:
    markdown_core_dump_free(edited_dump);
    markdown_core_dump_free(reference_dump);
    markdown_core_document_free(reference);
    markdown_core_delta_free(changes);
    return result;
}

/* --- edit-script interpreter --------------------------------------------- */

typedef struct er_script_cursor {
    const uint8_t *bytes;
    size_t length;
    size_t offset;
} er_script_cursor;

/* Operand reads past the end of the script supply zeroes, so every input
 * decodes to a complete operation sequence. */
static uint8_t er_script_u8(er_script_cursor *cursor) {
    if (cursor->offset >= cursor->length) {
        return 0;
    }
    return cursor->bytes[cursor->offset++];
}

static uint16_t er_script_u16(er_script_cursor *cursor) {
    uint16_t lo = er_script_u8(cursor);
    uint16_t hi = er_script_u8(cursor);
    return (uint16_t)(lo | (hi << 8));
}

int er_script_replay(const uint8_t *script, size_t length, const char *context, er_report_fn report, void *user) {
    er_script_cursor cursor;
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
    er_replay replay;
    int result = -1;

    cursor.bytes = script;
    cursor.length = length;
    cursor.offset = 0;

    markdown_core_parse_options_init(&options);
    mask = er_script_u16(&cursor);
    for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        *fields[i] = (mask >> i) & 1;
    }

    if (er_replay_open(&replay, context, &options, report, user) != 0) {
        return -1;
    }

    while (cursor.offset < cursor.length) {
        uint8_t op = er_script_u8(&cursor);
        switch (op & 3) {
        case 0: /* insert */
        case 2: /* replace */
        {
            size_t position = (size_t)er_script_u16(&cursor) % (replay.shadow.length + 1);
            size_t span = 0;
            size_t insert_length;
            size_t available;
            if ((op & 3) == 2) {
                span = (size_t)er_script_u16(&cursor) % (replay.shadow.length - position + 1);
            }
            insert_length = er_script_u8(&cursor);
            available = cursor.length - cursor.offset;
            if (insert_length > available) {
                insert_length = available;
            }
            if (er_replay_edit(&replay, position, position + span, cursor.bytes + cursor.offset, insert_length) != 0) {
                goto done;
            }
            cursor.offset += insert_length;
            break;
        }
        case 1: /* delete */
        {
            size_t position = (size_t)er_script_u16(&cursor) % (replay.shadow.length + 1);
            size_t span = (size_t)er_script_u16(&cursor) % (replay.shadow.length - position + 1);
            if (er_replay_edit(&replay, position, position + span, NULL, 0) != 0) {
                goto done;
            }
            break;
        }
        default: /* commit */
            if (er_replay_commit(&replay) != 0) {
                goto done;
            }
            break;
        }
    }

    if (er_replay_commit(&replay) != 0) {
        goto done;
    }
    result = 0;
done:
    er_replay_close(&replay);
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
