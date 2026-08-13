#include "edit_replay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"

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

/* --- id ledger ----------------------------------------------------------- */

static er_ledger_entry *er_ledger_find(er_ledger *ledger, markdown_core_node_id id) {
    size_t lo = 0;
    size_t hi = ledger->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ledger->entries[mid].id < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < ledger->count && ledger->entries[lo].id == id) {
        return &ledger->entries[lo];
    }
    return NULL;
}

static er_ledger_entry *er_ledger_insert(er_ledger *ledger, markdown_core_node_id id) {
    size_t lo = 0;
    size_t hi = ledger->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ledger->entries[mid].id < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (ledger->count == ledger->capacity) {
        size_t capacity = ledger->capacity ? ledger->capacity * 2 : 64;
        er_ledger_entry *grown = (er_ledger_entry *)realloc(ledger->entries, capacity * sizeof(*grown));
        if (!grown) {
            return NULL;
        }
        ledger->entries = grown;
        ledger->capacity = capacity;
    }
    memmove(ledger->entries + lo + 1, ledger->entries + lo, (ledger->count - lo) * sizeof(*ledger->entries));
    ledger->count++;
    ledger->entries[lo].id = id;
    return &ledger->entries[lo];
}

/* --- own-projection comparison ------------------------------------------- */

/* NULL-versus-empty matters: `[a](/u)` and `[a](/u "")` differ only in it. */
static bool er_view_equal(markdown_core_string a, markdown_core_string b) {
    if ((a.data == NULL) != (b.data == NULL) || a.length != b.length) {
        return false;
    }
    return a.length == 0 || memcmp(a.data, b.data, a.length) == 0;
}

/* Whether two nodes' OWN projections are equal: kind, every typed field, and
 * literal bytes — children and positions excluded. Positions are excluded
 * because a pure positional shift never changes a node's revision; children
 * are the walk's to compare by id.
 *
 * Deliberately independent of the engine's own field comparison (which is
 * what the revision stamps under test are computed from): everything here
 * goes through the public accessors, the same surface a consumer reads. */
static bool er_projection_equal(const markdown_core_node *a, const markdown_core_node *b) {
    markdown_core_string a1 = {NULL, 0}, a2 = {NULL, 0}, a3 = {NULL, 0};
    markdown_core_string b1 = {NULL, 0}, b2 = {NULL, 0}, b3 = {NULL, 0};

    if (markdown_core_node_get_kind(a) != markdown_core_node_get_kind(b)) {
        return false;
    }
    {
        bool ra = markdown_core_node_literal(a, &a1);
        bool rb = markdown_core_node_literal(b, &b1);
        if (ra != rb || (ra && !er_view_equal(a1, b1))) {
            return false;
        }
    }
    {
        int32_t la = 0, lb = 0;
        bool ra = markdown_core_node_heading_level(a, &la);
        bool rb = markdown_core_node_heading_level(b, &lb);
        if (ra != rb || (ra && la != lb)) {
            return false;
        }
    }
    {
        markdown_core_list_flavor fa, fb;
        markdown_core_optional_i64 sa, sb;
        bool ta = false, tb = false;
        bool ra = markdown_core_node_list_properties(a, &fa, &sa, &ta);
        bool rb = markdown_core_node_list_properties(b, &fb, &sb, &tb);
        if (ra != rb) {
            return false;
        }
        if (ra && !(fa == fb && ta == tb && sa.has_value == sb.has_value && (!sa.has_value || sa.value == sb.value))) {
            return false;
        }
    }
    {
        markdown_core_optional_bool ca, cb;
        bool ra = markdown_core_node_list_item_checked(a, &ca);
        bool rb = markdown_core_node_list_item_checked(b, &cb);
        if (ra != rb) {
            return false;
        }
        if (ra && !(ca.has_value == cb.has_value && (!ca.has_value || ca.value == cb.value))) {
            return false;
        }
    }
    {
        bool fenced_a = false, closed_a = false, fenced_b = false, closed_b = false;
        markdown_core_string lang_a = {NULL, 0}, lang_b = {NULL, 0};
        bool ra = markdown_core_node_code_block_properties(a, &a1, &lang_a, &a3, &fenced_a, &closed_a);
        bool rb = markdown_core_node_code_block_properties(b, &b1, &lang_b, &b3, &fenced_b, &closed_b);
        if (ra != rb) {
            return false;
        }
        if (ra && !(fenced_a == fenced_b && closed_a == closed_b && er_view_equal(a1, b1) && er_view_equal(a3, b3))) {
            return false;
        }
    }
    {
        markdown_core_placement_mode ma, mb;
        bool ra = markdown_core_node_formula_properties(a, &ma, &a1);
        bool rb = markdown_core_node_formula_properties(b, &mb, &b1);
        if (ra != rb || (ra && (ma != mb || !er_view_equal(a1, b1)))) {
            return false;
        }
    }
    {
        size_t ca = 0, cb = 0;
        bool ra = markdown_core_node_table_column_count(a, &ca);
        bool rb = markdown_core_node_table_column_count(b, &cb);
        if (ra != rb || (ra && ca != cb)) {
            return false;
        }
        if (ra) {
            size_t i;
            for (i = 0; i < ca; i++) {
                markdown_core_table_alignment aa, ab;
                if (!markdown_core_node_table_alignment_at(a, i, &aa) ||
                    !markdown_core_node_table_alignment_at(b, i, &ab) || aa != ab) {
                    return false;
                }
            }
        }
    }
    {
        bool ha = false, hb = false;
        bool ra = markdown_core_node_table_row_is_header(a, &ha);
        bool rb = markdown_core_node_table_row_is_header(b, &hb);
        if (ra != rb || (ra && ha != hb)) {
            return false;
        }
    }
    {
        markdown_core_placement_mode ma, mb;
        bool has_a = false, has_b = false;
        bool ra = markdown_core_node_directive_properties(a, &ma, &a1, &has_a);
        bool rb = markdown_core_node_directive_properties(b, &mb, &b1, &has_b);
        if (ra != rb) {
            return false;
        }
        if (ra) {
            size_t ca = 0, cb = 0;
            size_t i;
            if (ma != mb || has_a != has_b || !er_view_equal(a1, b1)) {
                return false;
            }
            markdown_core_node_directive_attribute_count(a, &ca);
            markdown_core_node_directive_attribute_count(b, &cb);
            if (ca != cb) {
                return false;
            }
            for (i = 0; i < ca; i++) {
                markdown_core_string ka = {NULL, 0}, va = {NULL, 0}, kb = {NULL, 0}, vb = {NULL, 0};
                if (!markdown_core_node_directive_attribute_at(a, i, &ka, &va) ||
                    !markdown_core_node_directive_attribute_at(b, i, &kb, &vb) || !er_view_equal(ka, kb) ||
                    !er_view_equal(va, vb)) {
                    return false;
                }
            }
        }
    }
    {
        bool ra = markdown_core_node_reference_definition_properties(a, &a1, &a2, &a3);
        bool rb = markdown_core_node_reference_definition_properties(b, &b1, &b2, &b3);
        if (ra != rb || (ra && !(er_view_equal(a1, b1) && er_view_equal(a2, b2) && er_view_equal(a3, b3)))) {
            return false;
        }
    }
    {
        markdown_core_reference_form fa, fb;
        bool ra = markdown_core_node_reference_properties(a, &a1, &fa);
        bool rb = markdown_core_node_reference_properties(b, &b1, &fb);
        if (ra != rb || (ra && (fa != fb || !er_view_equal(a1, b1)))) {
            return false;
        }
    }
    {
        bool ra = markdown_core_node_link_properties(a, &a1, &a2);
        bool rb = markdown_core_node_link_properties(b, &b1, &b2);
        if (ra != rb || (ra && !(er_view_equal(a1, b1) && er_view_equal(a2, b2)))) {
            return false;
        }
    }
    {
        bool ra = markdown_core_node_image_properties(a, &a1, &a2);
        bool rb = markdown_core_node_image_properties(b, &b1, &b2);
        if (ra != rb || (ra && !(er_view_equal(a1, b1) && er_view_equal(a2, b2)))) {
            return false;
        }
    }
    {
        bool ra = markdown_core_node_footnote_id(a, &a1);
        bool rb = markdown_core_node_footnote_id(b, &b1);
        if (ra != rb || (ra && !er_view_equal(a1, b1))) {
            return false;
        }
    }
    {
        bool ra = markdown_core_node_cross_link_reference(a, &a1);
        bool rb = markdown_core_node_cross_link_reference(b, &b1);
        if (ra != rb || (ra && !er_view_equal(a1, b1))) {
            return false;
        }
    }
    {
        bool ra = markdown_core_node_embed_reference(a, &a1);
        bool rb = markdown_core_node_embed_reference(b, &b1);
        if (ra != rb || (ra && !er_view_equal(a1, b1))) {
            return false;
        }
    }
    return true;
}

static bool er_child_ids_equal(const markdown_core_node *a, const markdown_core_node *b) {
    const markdown_core_node *ca = markdown_core_node_get_first_child(a);
    const markdown_core_node *cb = markdown_core_node_get_first_child(b);
    while (ca && cb) {
        if (markdown_core_node_get_id(ca) != markdown_core_node_get_id(cb)) {
            return false;
        }
        ca = markdown_core_node_get_next_sibling(ca);
        cb = markdown_core_node_get_next_sibling(cb);
    }
    return !ca && !cb;
}

/* --- the double walk ------------------------------------------------------ */

typedef struct er_prev_entry {
    markdown_core_node_id id;
    const markdown_core_node *node;
} er_prev_entry;

typedef struct er_prev_index {
    er_prev_entry *entries;
    size_t count;
    size_t capacity;
    bool failed;
} er_prev_index;

static int er_prev_collect(const markdown_core_node *node, void *context) {
    er_prev_index *index = (er_prev_index *)context;
    if (index->count == index->capacity) {
        size_t capacity = index->capacity ? index->capacity * 2 : 64;
        er_prev_entry *grown = (er_prev_entry *)realloc(index->entries, capacity * sizeof(*grown));
        if (!grown) {
            index->failed = true;
            return 1;
        }
        index->entries = grown;
        index->capacity = capacity;
    }
    index->entries[index->count].id = markdown_core_node_get_id(node);
    index->entries[index->count].node = node;
    index->count++;
    return 0;
}

static int er_prev_entry_compare(const void *left, const void *right) {
    markdown_core_node_id a = ((const er_prev_entry *)left)->id;
    markdown_core_node_id b = ((const er_prev_entry *)right)->id;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static const markdown_core_node *er_prev_find(const er_prev_index *index, markdown_core_node_id id) {
    size_t lo = 0;
    size_t hi = index->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (index->entries[mid].id < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < index->count && index->entries[lo].id == id) {
        return index->entries[lo].node;
    }
    return NULL;
}

typedef struct er_walk_state {
    er_replay *replay;
    const er_prev_index *previous;
    uint64_t successor_revision;
    int failed;
} er_walk_state;

/* One visit carries the whole per-node contract. The subtree form of the
 * (id, revision) promise — equal pair means an equal subtree — follows from
 * these node-local checks by induction: an unchanged parent pins its child id
 * list, the child-below-parent revision bound forces each child's revision to
 * be an old value, the two-value rule then forces it to be the child's own
 * last sighting, and the child's own visit compares its projection. */
static int er_walk_visit(const markdown_core_node *node, void *context) {
    er_walk_state *state = (er_walk_state *)context;
    er_replay *replay = state->replay;
    er_ledger *ledger = &replay->ledger;
    markdown_core_node_id id = markdown_core_node_get_id(node);
    uint64_t revision = markdown_core_node_get_revision(node);
    const markdown_core_node *parent = markdown_core_node_get_parent(node);
    er_ledger_entry *entry;

    state->failed = 1; /* every early return below is a failure */
    if (id == 0) {
        er_fail(replay, "a tree node has no id");
        return 1;
    }
    if (parent && revision > markdown_core_node_get_revision(parent)) {
        er_fail(replay, "a child's revision exceeds its parent's");
        return 1;
    }
    entry = er_ledger_find(ledger, id);
    if (entry && entry->seen == ledger->walk) {
        er_fail(replay, "one id appears twice in one tree");
        return 1;
    }
    if (!entry) {
        if (revision != state->successor_revision) {
            er_fail(replay, "a minted node does not carry the new document revision");
            return 1;
        }
        entry = er_ledger_insert(ledger, id);
        if (!entry) {
            er_fail(replay, "ledger allocation failed");
            return 1;
        }
    } else if (!entry->alive) {
        er_fail(replay, "a retired id came back");
        return 1;
    } else {
        if (revision < entry->revision) {
            er_fail(replay, "a node's revision went backwards");
            return 1;
        }
        if (revision != entry->revision && revision != state->successor_revision) {
            er_fail(replay, "a changed node does not carry the new document revision");
            return 1;
        }
        if (revision == entry->revision) {
            const markdown_core_node *before = er_prev_find(state->previous, id);
            if (!before) {
                er_fail(replay, "a live ledger id is missing from the predecessor tree");
                return 1;
            }
            if (!er_projection_equal(before, node)) {
                er_fail(replay, "a node's projection changed without a revision bump");
                return 1;
            }
            if (!er_child_ids_equal(before, node)) {
                er_fail(replay, "a node's child list changed without a revision bump");
                return 1;
            }
        }
    }
    entry->revision = revision;
    entry->seen = ledger->walk;
    entry->alive = true;
    state->failed = 0;
    return 0;
}

/* Walks `document`'s tree against the ledger. `previous` holds the
 * predecessor's nodes (empty for the seeding walk); afterwards, live ledger
 * entries the walk did not meet are retired. */
static int er_verify_tree(er_replay *replay, const markdown_core_document *document, const er_prev_index *previous) {
    er_walk_state state;
    size_t i;

    replay->ledger.walk++;
    state.replay = replay;
    state.previous = previous;
    state.successor_revision = markdown_core_document_revision(document);
    state.failed = 0;
    if (ts_ast_walk(markdown_core_document_root(document), er_walk_visit, &state) < 0) {
        return er_fail(replay, "identity walk failed to allocate");
    }
    if (state.failed) {
        return -1;
    }
    for (i = 0; i < replay->ledger.count; i++) {
        er_ledger_entry *entry = &replay->ledger.entries[i];
        if (entry->alive && entry->seen != replay->ledger.walk) {
            entry->alive = false;
        }
    }
    return 0;
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
    er_prev_index empty;
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
    /* The empty document seeds the ledger: every node it has is a mint. */
    memset(&empty, 0, sizeof(empty));
    return er_verify_tree(replay, replay->document, &empty);
}

void er_replay_close(er_replay *replay) {
    markdown_core_document_free(replay->document);
    free(replay->shadow.bytes);
    free(replay->ledger.entries);
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

int er_replay_commit(er_replay *replay) {
    markdown_core_error *error = NULL;
    markdown_core_document *previous = replay->document;
    markdown_core_document *successor;
    markdown_core_document *reference = NULL;
    uint8_t *edited_dump = NULL;
    uint8_t *reference_dump = NULL;
    size_t document_dump_length = 0;
    size_t reference_dump_length = 0;
    er_prev_index index;
    int result = -1;

    successor = markdown_core_document_edit(previous, mc_sv(replay->shadow.bytes, replay->shadow.length), &error);
    if (!successor) {
        markdown_core_document_free(previous);
        replay->document = NULL;
        markdown_core_error_free(error);
        return er_fail(replay, "commit failed");
    }
    /* The successor is adopted first so every failure path below leaves the
     * replay closeable; the predecessor stays alive until the double walk
     * has compared against it, and is released at `done`. */
    replay->document = successor;

    memset(&index, 0, sizeof(index));
    if (markdown_core_document_series(successor) != markdown_core_document_series(previous)) {
        er_fail(replay, "an edit changed the series");
        goto done;
    }
    if (markdown_core_document_revision(successor) <= markdown_core_document_revision(previous)) {
        er_fail(replay, "the document revision did not advance");
        goto done;
    }

    if (ts_ast_walk(markdown_core_document_root(previous), er_prev_collect, &index) < 0 || index.failed) {
        er_fail(replay, "predecessor index failed to allocate");
        goto done;
    }
    qsort(index.entries, index.count, sizeof(*index.entries), er_prev_entry_compare);
    if (er_verify_tree(replay, successor, &index) != 0) {
        goto done;
    }

    if (!markdown_core_document_dump(successor, &edited_dump, &document_dump_length, &error)) {
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
    markdown_core_document_free(previous);
    free(index.entries);
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
        er_replay_close(&replay);
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
