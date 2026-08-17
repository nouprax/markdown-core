#include "append_replay.h"

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

/* --- own-projection capture ----------------------------------------------- */

/* A growable byte buffer. Allocation failure is sticky and checked once at
 * the end of a capture rather than at every write, so the writers below read
 * as a field list instead of as error handling. */
typedef struct er_blob {
    uint8_t *bytes;
    size_t length;
    size_t capacity;
    bool failed;
} er_blob;

static void er_blob_release(er_blob *blob) {
    free(blob->bytes);
    memset(blob, 0, sizeof(*blob));
}

static void er_blob_put(er_blob *blob, const void *bytes, size_t length) {
    if (blob->failed || !length) {
        return;
    }
    if (length > blob->capacity - blob->length) {
        size_t capacity = blob->capacity ? blob->capacity : 256;
        uint8_t *grown;
        while (capacity - blob->length < length) {
            capacity *= 2;
        }
        grown = (uint8_t *)realloc(blob->bytes, capacity);
        if (!grown) {
            blob->failed = true;
            return;
        }
        blob->bytes = grown;
        blob->capacity = capacity;
    }
    memcpy(blob->bytes + blob->length, bytes, length);
    blob->length += length;
}

static void er_blob_u8(er_blob *blob, uint8_t value) { er_blob_put(blob, &value, sizeof(value)); }

static void er_blob_u64(er_blob *blob, uint64_t value) { er_blob_put(blob, &value, sizeof(value)); }

/* NULL-versus-empty matters: `[a](/u)` and `[a](/u "")` differ only in it, so
 * the presence byte is part of the description and not a convenience. */
static void er_blob_view(er_blob *blob, markdown_core_string view) {
    er_blob_u8(blob, (uint8_t)(view.data != NULL));
    er_blob_u64(blob, (uint64_t)view.length);
    er_blob_put(blob, view.data, view.length);
}

/* A node's own projection, WRITTEN rather than compared: kind, every typed
 * field, and literal bytes — children and positions excluded. Positions are
 * excluded because a pure positional shift never changes a node's revision;
 * children are the walk's to compare, by id, through er_child_ids_write.
 *
 * Writing rather than comparing is what lets the oracle survive a tree that
 * is mutated in place: what the head showed BEFORE a mutation is recorded as
 * bytes, and after the mutation the same writer describes what the tree
 * shows now, so equality is a memcmp. There is exactly one field list, used
 * by both sides, so the two descriptions cannot drift apart.
 *
 * Deliberately independent of the engine's own field comparison (which is
 * what the revision stamps under test are computed from): everything here
 * goes through the public accessors, the same surface a consumer reads. */
static void er_projection_write(const markdown_core_node *node, er_blob *out) {
    markdown_core_string v1 = {NULL, 0}, v2 = {NULL, 0}, v3 = {NULL, 0};

    er_blob_u64(out, (uint64_t)markdown_core_node_get_kind(node));
    {
        bool present = markdown_core_node_literal(node, &v1);
        er_blob_u8(out, (uint8_t)present);
        if (present) {
            er_blob_view(out, v1);
        }
    }
    {
        int32_t level = 0;
        bool present = markdown_core_node_heading_level(node, &level);
        er_blob_u8(out, (uint8_t)present);
        if (present) {
            er_blob_u64(out, (uint64_t)level);
        }
    }
    {
        markdown_core_list_flavor flavor;
        markdown_core_optional_i64 start;
        bool tight = false;
        bool present = markdown_core_node_list_properties(node, &flavor, &start, &tight);
        er_blob_u8(out, (uint8_t)present);
        if (present) {
            er_blob_u64(out, (uint64_t)flavor);
            er_blob_u8(out, (uint8_t)tight);
            er_blob_u8(out, (uint8_t)start.has_value);
            er_blob_u64(out, start.has_value ? (uint64_t)start.value : 0);
        }
    }
    {
        markdown_core_optional_bool checked;
        bool present = markdown_core_node_list_item_checked(node, &checked);
        er_blob_u8(out, (uint8_t)present);
        if (present) {
            er_blob_u8(out, (uint8_t)checked.has_value);
            er_blob_u8(out, (uint8_t)(checked.has_value ? checked.value : false));
        }
    }
    {
        markdown_core_string language = {NULL, 0};
        bool fenced = false, closed = false;
        bool present = markdown_core_node_code_block_properties(node, &v1, &language, &v3, &fenced, &closed);
        er_blob_u8(out, (uint8_t)present);
        if (present) {
            er_blob_u8(out, (uint8_t)fenced);
            er_blob_u8(out, (uint8_t)closed);
            er_blob_view(out, v1);
            er_blob_view(out, v3);
        }
    }
    {
        markdown_core_placement_mode mode;
        bool present = markdown_core_node_formula_properties(node, &mode, &v1);
        er_blob_u8(out, (uint8_t)present);
        if (present) {
            er_blob_u64(out, (uint64_t)mode);
            er_blob_view(out, v1);
        }
    }
    {
        size_t columns = 0;
        bool present = markdown_core_node_table_column_count(node, &columns);
        er_blob_u8(out, (uint8_t)present);
        if (present) {
            size_t i;
            er_blob_u64(out, (uint64_t)columns);
            for (i = 0; i < columns; i++) {
                markdown_core_table_alignment alignment;
                bool at = markdown_core_node_table_alignment_at(node, i, &alignment);
                er_blob_u8(out, (uint8_t)at);
                er_blob_u64(out, at ? (uint64_t)alignment : 0);
            }
        }
    }
    {
        bool header = false;
        bool present = markdown_core_node_table_row_is_header(node, &header);
        er_blob_u8(out, (uint8_t)present);
        if (present) {
            er_blob_u8(out, (uint8_t)header);
        }
    }
    {
        markdown_core_placement_mode mode;
        bool has_attributes = false;
        bool present = markdown_core_node_directive_properties(node, &mode, &v1, &has_attributes);
        er_blob_u8(out, (uint8_t)present);
        if (present) {
            size_t count = 0;
            size_t i;
            er_blob_u64(out, (uint64_t)mode);
            er_blob_u8(out, (uint8_t)has_attributes);
            er_blob_view(out, v1);
            markdown_core_node_directive_attribute_count(node, &count);
            er_blob_u64(out, (uint64_t)count);
            for (i = 0; i < count; i++) {
                markdown_core_string key = {NULL, 0}, value = {NULL, 0};
                bool at = markdown_core_node_directive_attribute_at(node, i, &key, &value);
                er_blob_u8(out, (uint8_t)at);
                if (at) {
                    er_blob_view(out, key);
                    er_blob_view(out, value);
                }
            }
        }
    }
    {
        bool present = markdown_core_node_reference_definition_properties(node, &v1, &v2, &v3);
        er_blob_u8(out, (uint8_t)present);
        if (present) {
            er_blob_view(out, v1);
            er_blob_view(out, v2);
            er_blob_view(out, v3);
        }
    }
    {
        markdown_core_reference_form form;
        bool present = markdown_core_node_reference_properties(node, &v1, &form);
        er_blob_u8(out, (uint8_t)present);
        if (present) {
            er_blob_u64(out, (uint64_t)form);
            er_blob_view(out, v1);
        }
    }
    {
        bool present = markdown_core_node_link_properties(node, &v1, &v2);
        er_blob_u8(out, (uint8_t)present);
        if (present) {
            er_blob_view(out, v1);
            er_blob_view(out, v2);
        }
    }
    {
        bool present = markdown_core_node_image_properties(node, &v1, &v2);
        er_blob_u8(out, (uint8_t)present);
        if (present) {
            er_blob_view(out, v1);
            er_blob_view(out, v2);
        }
    }
    {
        bool present = markdown_core_node_footnote_id(node, &v1);
        er_blob_u8(out, (uint8_t)present);
        if (present) {
            er_blob_view(out, v1);
        }
    }
    {
        bool present = markdown_core_node_cross_link_reference(node, &v1);
        er_blob_u8(out, (uint8_t)present);
        if (present) {
            er_blob_view(out, v1);
        }
    }
    {
        bool present = markdown_core_node_embed_reference(node, &v1);
        er_blob_u8(out, (uint8_t)present);
        if (present) {
            er_blob_view(out, v1);
        }
    }
}

/* The child list as ids, in order: what an unchanged node promises about its
 * children. */
static void er_child_ids_write(const markdown_core_node *node, er_blob *out) {
    const markdown_core_node *child;
    uint64_t count = 0;
    for (child = markdown_core_node_get_first_child(node); child; child = markdown_core_node_get_next_sibling(child)) {
        count++;
    }
    er_blob_u64(out, count);
    for (child = markdown_core_node_get_first_child(node); child; child = markdown_core_node_get_next_sibling(child)) {
        er_blob_u64(out, (uint64_t)markdown_core_node_get_id(child));
    }
}

/* --- the captured head ---------------------------------------------------- */

/* What the head showed before a mutation: one record per node, holding the
 * bytes its projection and its child list wrote. Nothing here points into
 * the tree, so the oracle keeps its evidence when the mutation rewrites the
 * very nodes it is about to check — which is what a tree that grows in place
 * requires of the harness. */
typedef struct er_capture_entry {
    markdown_core_node_id id;
    uint64_t revision;
    size_t projection_offset;
    size_t projection_length;
    size_t children_offset;
    size_t children_length;
} er_capture_entry;

typedef struct er_capture {
    er_blob blob;
    er_capture_entry *entries;
    size_t count;
    size_t capacity;
    bool failed;
    uint64_t series;
    uint64_t revision;
} er_capture;

static void er_capture_release(er_capture *capture) {
    er_blob_release(&capture->blob);
    free(capture->entries);
    memset(capture, 0, sizeof(*capture));
}

static int er_capture_visit(const markdown_core_node *node, void *context) {
    er_capture *capture = (er_capture *)context;
    er_capture_entry *entry;
    if (capture->count == capture->capacity) {
        size_t capacity = capture->capacity ? capture->capacity * 2 : 64;
        er_capture_entry *grown = (er_capture_entry *)realloc(capture->entries, capacity * sizeof(*grown));
        if (!grown) {
            capture->failed = true;
            return 1;
        }
        capture->entries = grown;
        capture->capacity = capacity;
    }
    entry = &capture->entries[capture->count];
    entry->id = markdown_core_node_get_id(node);
    entry->revision = markdown_core_node_get_revision(node);
    entry->projection_offset = capture->blob.length;
    er_projection_write(node, &capture->blob);
    entry->projection_length = capture->blob.length - entry->projection_offset;
    entry->children_offset = capture->blob.length;
    er_child_ids_write(node, &capture->blob);
    entry->children_length = capture->blob.length - entry->children_offset;
    capture->count++;
    return 0;
}

static int er_capture_entry_compare(const void *left, const void *right) {
    markdown_core_node_id a = ((const er_capture_entry *)left)->id;
    markdown_core_node_id b = ((const er_capture_entry *)right)->id;
    return a < b ? -1 : (a > b ? 1 : 0);
}

/* Records the document as it is NOW. Called before every mutation. */
static int er_capture_head(er_capture *capture, const markdown_core_document *document) {
    memset(capture, 0, sizeof(*capture));
    capture->series = markdown_core_document_series(document);
    capture->revision = markdown_core_document_revision(document);
    if (ts_ast_walk(markdown_core_document_root(document), er_capture_visit, capture) < 0 || capture->failed ||
        capture->blob.failed) {
        return -1;
    }
    qsort(capture->entries, capture->count, sizeof(*capture->entries), er_capture_entry_compare);
    return 0;
}

static const er_capture_entry *er_capture_find(const er_capture *capture, markdown_core_node_id id) {
    size_t lo = 0;
    size_t hi = capture->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (capture->entries[mid].id < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < capture->count && capture->entries[lo].id == id) {
        return &capture->entries[lo];
    }
    return NULL;
}

/* --- the double walk ------------------------------------------------------ */

typedef struct er_walk_state {
    er_replay *replay;
    const er_capture *before;
    er_blob scratch;
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
            const er_capture_entry *before = er_capture_find(state->before, id);
            if (!before) {
                er_fail(replay, "a live ledger id is missing from the captured head");
                return 1;
            }
            state->scratch.length = 0;
            er_projection_write(node, &state->scratch);
            if (state->scratch.failed) {
                er_fail(replay, "projection capture allocation failed");
                return 1;
            }
            if (state->scratch.length != before->projection_length ||
                memcmp(
                    state->scratch.bytes,
                    state->before->blob.bytes + before->projection_offset,
                    before->projection_length
                ) != 0) {
                er_fail(replay, "a node's projection changed without a revision bump");
                return 1;
            }
            state->scratch.length = 0;
            er_child_ids_write(node, &state->scratch);
            if (state->scratch.failed) {
                er_fail(replay, "child list capture allocation failed");
                return 1;
            }
            if (state->scratch.length != before->children_length ||
                memcmp(
                    state->scratch.bytes,
                    state->before->blob.bytes + before->children_offset,
                    before->children_length
                ) != 0) {
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

/* THE CONVERSE OF THE (id, revision) PROMISE: a node whose own projection,
 * child-id list and every descendant are as the captured head showed them
 * is an unchanged subtree, and its revision must be the one the head
 * carried — "the document revision at which the node last changed" is a
 * claim about the last change, and a revision that moves for nothing is a
 * consumer re-decoding a subtree that did not. Postorder, so a node knows
 * its children's verdicts; iterative, so depth costs no stack. */
typedef struct er_post_frame {
    const markdown_core_node *node;
    bool children_unchanged;
} er_post_frame;

static int er_verify_no_spurious_bumps(
    er_replay *replay,
    const markdown_core_document *document,
    const er_capture *before
) {
    er_post_frame *stack = NULL;
    size_t depth = 0;
    size_t capacity = 0;
    er_blob scratch = {NULL, 0, 0, false};
    const markdown_core_node *node = markdown_core_document_root(document);
    int result = 0;

    if (!node) {
        return 0;
    }
    for (;;) {
        /* Descend to the leftmost leaf, opening a frame per level. */
        for (;;) {
            if (depth == capacity) {
                size_t grown_capacity = capacity ? capacity * 2 : 64;
                er_post_frame *grown = (er_post_frame *)realloc(stack, grown_capacity * sizeof(*grown));
                if (!grown) {
                    result = er_fail(replay, "postorder walk failed to allocate");
                    goto done;
                }
                stack = grown;
                capacity = grown_capacity;
            }
            stack[depth].node = node;
            stack[depth].children_unchanged = true;
            depth++;
            if (!markdown_core_node_get_first_child(node)) {
                break;
            }
            node = markdown_core_node_get_first_child(node);
        }
        /* Finish frames until one has a next sibling to descend into. */
        for (;;) {
            er_post_frame *frame = &stack[depth - 1];
            const er_capture_entry *entry;
            bool unchanged = frame->children_unchanged;
            node = frame->node;
            entry = er_capture_find(before, markdown_core_node_get_id(node));
            if (!entry) {
                unchanged = false;
            }
            if (unchanged) {
                scratch.length = 0;
                er_projection_write(node, &scratch);
                if (scratch.failed) {
                    result = er_fail(replay, "projection capture allocation failed");
                    goto done;
                }
                unchanged =
                    scratch.length == entry->projection_length &&
                    memcmp(scratch.bytes, before->blob.bytes + entry->projection_offset, entry->projection_length) == 0;
            }
            if (unchanged) {
                scratch.length = 0;
                er_child_ids_write(node, &scratch);
                if (scratch.failed) {
                    result = er_fail(replay, "child list capture allocation failed");
                    goto done;
                }
                unchanged =
                    scratch.length == entry->children_length &&
                    memcmp(scratch.bytes, before->blob.bytes + entry->children_offset, entry->children_length) == 0;
            }
            if (unchanged && markdown_core_node_get_revision(node) != entry->revision) {
                result = er_fail(replay, "a node's revision moved without a change under it");
                goto done;
            }
            depth--;
            if (depth == 0) {
                goto done;
            }
            stack[depth - 1].children_unchanged = stack[depth - 1].children_unchanged && unchanged;
            if (markdown_core_node_get_next_sibling(node)) {
                node = markdown_core_node_get_next_sibling(node);
                break;
            }
        }
    }
done:
    er_blob_release(&scratch);
    free(stack);
    return result;
}

/* Walks `document`'s tree against the ledger. `previous` holds the
 * predecessor's nodes (empty for the seeding walk); afterwards, live ledger
 * entries the walk did not meet are retired. */
static int er_verify_tree(er_replay *replay, const markdown_core_document *document, const er_capture *before) {
    er_walk_state state;
    size_t i;
    int walked;

    replay->ledger.walk++;
    memset(&state, 0, sizeof(state));
    state.replay = replay;
    state.before = before;
    state.successor_revision = markdown_core_document_revision(document);
    walked = ts_ast_walk(markdown_core_document_root(document), er_walk_visit, &state);
    er_blob_release(&state.scratch);
    if (walked < 0) {
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
    er_capture empty;
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

/* The oracle behind the mutation: adopt the successor, walk it against the
 * captured head and the ledger, and require the dump to equal a one-shot
 * parse of the shadow bytes. `before` was recorded from the head while it was
 * still the head; `previous` is released here and never read. */
static int er_verify_successor(
    er_replay *replay,
    markdown_core_document *previous,
    markdown_core_document *successor,
    const er_capture *before
) {
    markdown_core_error *error = NULL;
    markdown_core_document *reference = NULL;
    uint8_t *edited_dump = NULL;
    uint8_t *reference_dump = NULL;
    size_t document_dump_length = 0;
    size_t reference_dump_length = 0;
    int result = -1;

    /* The successor is adopted first so every failure path below leaves the
     * replay closeable; the predecessor stays alive until the double walk
     * has compared against it, and is released at `done`. */
    replay->document = successor;

    if (markdown_core_document_series(successor) != before->series) {
        er_fail(replay, "a mutation changed the series");
        goto done;
    }
    if (markdown_core_document_revision(successor) != before->revision + 1) {
        er_fail(replay, "the document revision did not advance by exactly one");
        goto done;
    }

    if (er_verify_tree(replay, successor, before) != 0) {
        goto done;
    }
    if (er_verify_no_spurious_bumps(replay, successor, before) != 0) {
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
    return result;
}

/* Appends `length` bytes through the REAL append mutation — any split is
 * legal, mid-UTF-8 and mid-line included — and runs the oracle: the walk
 * against the head as it was, plus dump equality against a one-shot parse of
 * the shadow bytes.
 *
 * The head is captured BEFORE the mutation, never read after it. That is
 * what the oracle needs to keep proving anything once the engine grows its
 * tree in place: a successor that shares its predecessor's node objects
 * would otherwise be compared against itself, and every check would pass by
 * construction. */
int er_replay_append(er_replay *replay, const uint8_t *bytes, size_t length) {
    markdown_core_error *error = NULL;
    markdown_core_document *previous = replay->document;
    markdown_core_document *successor;
    er_capture before;
    int result;

    if (er_text_splice(&replay->shadow, replay->shadow.length, replay->shadow.length, bytes, length) != 0) {
        return er_fail(replay, "shadow append allocation failed");
    }
    if (er_capture_head(&before, previous) != 0) {
        er_capture_release(&before);
        return er_fail(replay, "head capture failed to allocate");
    }
    successor = markdown_core_document_append(previous, mc_sv(bytes, length), &error);
    if (!successor) {
        er_capture_release(&before);
        markdown_core_document_free(previous);
        replay->document = NULL;
        markdown_core_error_free(error);
        return er_fail(replay, "append failed");
    }
    result = er_verify_successor(replay, previous, successor, &before);
    er_capture_release(&before);
    return result;
}

/* --- append-script interpreter ------------------------------------------ */

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

    /* Chunks until the script runs out: len8 then that many literal bytes.
     * The fuzzer owns the chunk boundaries, so every adversarial split —
     * mid-UTF-8, mid-CRLF, mid-line — is reachable by construction; a zero
     * length is the empty append, a mutation like any other. */
    while (cursor.offset < cursor.length) {
        size_t chunk_length = er_script_u8(&cursor);
        size_t available = cursor.length - cursor.offset;
        if (chunk_length > available) {
            chunk_length = available;
        }
        if (er_replay_append(&replay, cursor.bytes + cursor.offset, chunk_length) != 0) {
            goto done;
        }
        cursor.offset += chunk_length;
    }

    /* One final empty append: the tail state must survive a mutation that
     * adds nothing. */
    if (er_replay_append(&replay, (const uint8_t *)"", 0) != 0) {
        goto done;
    }
    result = 0;
done:
    er_replay_close(&replay);
    return result;
}
