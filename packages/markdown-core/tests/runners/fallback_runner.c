/* Fallback and degradation suite for the shared byte-key index.
 *
 * The hash index keeps two guarded escape hatches that normal input never
 * exercises: allocation failure falls back to the inherited pointer-sort
 * paths, and probe exhaustion grows the table once before giving up.  These
 * cases force each hatch deterministically -- with an injected allocator that
 * refuses slot-table allocations, and with keys constructed to cluster in one
 * probe window -- then check that the observable results stay identical to
 * the hash path.
 *
 *   fallback_runner --list
 *   fallback_runner --case NAME
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "markdown-core.h"
#include "markdown-core-extension-api.h"
#include "markdown-core-extensions.h"
#include "map.h"
#include "node.h"
#include "parser.h"
#include "references.h"

#include "cross_reference.h"
#include "document_internal.h"
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

/* Injected allocator.  Only the targeted shapes fail: key-index slot tables
 * are calloc(capacity >= 16, sizeof(slot)) and the sorted-fallback pointer
 * arrays are calloc(count >= 2, sizeof(pointer)); every other allocation in
 * the engine uses calloc(1, size) or byte-sized elements. */
static size_t fb_blocked_allocations;
static int fb_block_slot_tables;
static int fb_block_pointer_arrays;
static int fb_block_all_callocs;
static int fb_observe_allocations;
static size_t fb_max_slot_capacity;
static size_t fb_lane_allocations;
static size_t fb_record_allocations;
static size_t fb_backslash_payload_size;
static size_t fb_backslash_payload_allocations;

static void *fb_calloc(markdown_core_mem *mem, size_t nmemb, size_t size) {
    (void)mem;
    if (fb_observe_allocations) {
        if (size == sizeof(markdown_core_key_index_slot) && nmemb > fb_max_slot_capacity) {
            fb_max_slot_capacity = nmemb;
        }
        if (size == sizeof(markdown_core_delimiter_lane)) {
            fb_lane_allocations++;
        }
        if (size == 1 && nmemb == fb_backslash_payload_size) {
            fb_backslash_payload_allocations++;
        }
    }
    if (fb_block_all_callocs) {
        fb_blocked_allocations++;
        return NULL;
    }
    if (fb_block_slot_tables && nmemb >= 16 && size == sizeof(markdown_core_key_index_slot)) {
        fb_blocked_allocations++;
        return NULL;
    }
    if (fb_block_pointer_arrays && nmemb >= 2 && size == sizeof(void *)) {
        fb_blocked_allocations++;
        return NULL;
    }
    return calloc(nmemb, size);
}

static void *fb_realloc(markdown_core_mem *mem, void *pointer, size_t size) {
    (void)mem;
    if (fb_observe_allocations && size >= 16 * sizeof(markdown_core_delimiter_record) &&
        size % sizeof(markdown_core_delimiter_record) == 0) {
        fb_record_allocations++;
    }
    return realloc(pointer, size);
}

static void fb_free(markdown_core_mem *mem, void *pointer) {
    (void)mem;
    free(pointer);
}

static markdown_core_mem fb_failing_mem = {fb_calloc, fb_realloc, fb_free};

static void fb_observer_reset(void) {
    fb_max_slot_capacity = 0;
    fb_lane_allocations = 0;
    fb_record_allocations = 0;
    fb_backslash_payload_size = 0;
    fb_backslash_payload_allocations = 0;
}

/* Sweep allocator: counts allocations, or fails exactly the k-th one
 * (calloc and realloc share the counter). */
static unsigned long fb_sweep_count;
static unsigned long fb_sweep_fail_at; /* 0 = count only */
static int fb_sweep_fired;

static void *fb_sweep_calloc(markdown_core_mem *mem, size_t nmemb, size_t size) {
    (void)mem;
    if (++fb_sweep_count == fb_sweep_fail_at) {
        fb_sweep_fired = 1;
        return NULL;
    }
    return calloc(nmemb, size);
}

static void *fb_sweep_realloc(markdown_core_mem *mem, void *pointer, size_t size) {
    (void)mem;
    if (++fb_sweep_count == fb_sweep_fail_at) {
        fb_sweep_fired = 1;
        return NULL;
    }
    return realloc(pointer, size);
}

static markdown_core_mem fb_sweep_mem = {fb_sweep_calloc, fb_sweep_realloc, fb_free};

/* Unwind allocator: the sweep allocator plus a live-block count, so a refused
 * construction can be asked whether it gave everything back. The sweep
 * allocator deliberately does not track this — its subject is convergence
 * after a failed commit, and it runs tens of thousands of ordinals — so this
 * is a second, narrower instrument rather than a change to that one. */
static unsigned long fb_unwind_count;
static unsigned long fb_unwind_fail_at; /* 0 = count only */
static long fb_unwind_live;

static void *fb_unwind_calloc(markdown_core_mem *mem, size_t nmemb, size_t size) {
    void *block;
    (void)mem;
    if (++fb_unwind_count == fb_unwind_fail_at) {
        return NULL;
    }
    block = calloc(nmemb, size);
    if (block) {
        fb_unwind_live++;
    }
    return block;
}

static void *fb_unwind_realloc(markdown_core_mem *mem, void *pointer, size_t size) {
    void *block;
    (void)mem;
    if (++fb_unwind_count == fb_unwind_fail_at) {
        return NULL;
    }
    block = realloc(pointer, size);
    if (block && !pointer) {
        fb_unwind_live++;
    }
    return block;
}

static void fb_unwind_free(markdown_core_mem *mem, void *pointer) {
    (void)mem;
    if (pointer) {
        fb_unwind_live--;
    }
    free(pointer);
}

static markdown_core_mem fb_unwind_mem = {fb_unwind_calloc, fb_unwind_realloc, fb_unwind_free};

static markdown_core_chunk fb_chunk(const char *text) {
    markdown_core_chunk chunk;
    chunk.data = (unsigned char *)text;
    chunk.len = (markdown_core_bufsize)strlen(text);
    chunk.alloc = 0;
    return chunk;
}

static char *fb_strdup(const char *text) {
    size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1);
    if (copy) {
        memcpy(copy, text, length + 1);
    }
    return copy;
}

static int fb_expect_url(markdown_core_map *map, const char *label, const char *url, const char *context) {
    markdown_core_chunk chunk = fb_chunk(label);
    markdown_core_reference *ref = (markdown_core_reference *)markdown_core_map_lookup(map, &chunk);
    if (!url) {
        if (ref) {
            fprintf(stderr, "%s: label '%s' unexpectedly resolved\n", context, label);
            return -1;
        }
        return 0;
    }
    if (!ref) {
        fprintf(stderr, "%s: label '%s' did not resolve\n", context, label);
        return -1;
    }
    if (ref->url.len != (markdown_core_bufsize)strlen(url) || memcmp(ref->url.data, url, ref->url.len) != 0) {
        fprintf(
            stderr,
            "%s: label '%s' resolved to '%.*s', expected '%s'\n",
            context,
            label,
            (int)ref->url.len,
            ref->url.data,
            url
        );
        return -1;
    }
    return 0;
}

static void fb_create_reference(markdown_core_map *map, const char *label, const char *url) {
    markdown_core_chunk label_chunk = fb_chunk(label);
    markdown_core_chunk url_chunk = fb_chunk(url);
    markdown_core_chunk title_chunk = fb_chunk("");
    markdown_core_reference_create(map, &label_chunk, &url_chunk, &title_chunk);
}

enum { FB_UNIQUE_REFERENCES = 40 };

static void fb_populate_reference_map(markdown_core_map *map) {
    char label[32];
    char url[32];
    int i;
    for (i = 0; i < FB_UNIQUE_REFERENCES; i++) {
        snprintf(label, sizeof(label), "ref%d", i);
        snprintf(url, sizeof(url), "/u%d", i);
        fb_create_reference(map, label, url);
    }
    fb_create_reference(map, "dup", "/first");
    fb_create_reference(map, "dup", "/second");
    fb_create_reference(map, "dup", "/third");
}

static int fb_check_reference_map(markdown_core_map *map, const char *context) {
    char label[32];
    char url[32];
    int i;
    for (i = 0; i < FB_UNIQUE_REFERENCES; i++) {
        snprintf(label, sizeof(label), "ref%d", i);
        snprintf(url, sizeof(url), "/u%d", i);
        if (fb_expect_url(map, label, url, context) != 0) {
            return -1;
        }
    }
    if (fb_expect_url(map, "dup", "/first", context) != 0) {
        return -1;
    }
    return fb_expect_url(map, "missing", NULL, context);
}

/* Identical definitions resolve identically through the hash index and
 * through the allocation-failure pointer-sort fallback, including
 * first-definition-wins for duplicate labels. */
static int case_reference_sorted_fallback(void) {
    markdown_core_map *hash_map = markdown_core_reference_map_new(markdown_core_mem_default());
    markdown_core_map *fallback_map = markdown_core_reference_map_new(&fb_failing_mem);
    size_t blocked_before = fb_blocked_allocations;
    int result = -1;

    fb_populate_reference_map(hash_map);
    fb_populate_reference_map(fallback_map);

    fb_block_slot_tables = 1;
    if (fb_check_reference_map(fallback_map, "sorted fallback") != 0) {
        goto done;
    }
    fb_block_slot_tables = 0;
    if (fb_check_reference_map(hash_map, "hash path") != 0) {
        goto done;
    }

    if (!hash_map->indexed) {
        fputs("control map did not take the hash path\n", stderr);
        goto done;
    }
    if (fallback_map->indexed || !fallback_map->prepared || !fallback_map->sorted) {
        fputs("fallback map did not take the pointer-sort path\n", stderr);
        goto done;
    }
    if (fb_blocked_allocations == blocked_before) {
        fputs("injected allocator never fired\n", stderr);
        goto done;
    }
    result = 0;
done:
    fb_block_slot_tables = 0;
    markdown_core_map_free(hash_map);
    markdown_core_map_free(fallback_map);
    return result;
}

/* When neither preparation path can allocate, lookups miss without crashing
 * and the map recovers once allocation succeeds again. */
static int case_map_prepare_oom(void) {
    markdown_core_map *map = markdown_core_reference_map_new(&fb_failing_mem);
    int result = -1;

    fb_populate_reference_map(map);

    fb_block_slot_tables = 1;
    fb_block_pointer_arrays = 1;
    if (fb_expect_url(map, "ref1", NULL, "prepare under OOM") != 0) {
        goto done;
    }
    if (map->prepared) {
        fputs("map reported prepared after failed preparation\n", stderr);
        goto done;
    }
    fb_block_slot_tables = 0;
    fb_block_pointer_arrays = 0;
    if (fb_expect_url(map, "ref1", "/u1", "recovery after OOM") != 0) {
        goto done;
    }
    if (!map->indexed) {
        fputs("recovered map did not take the hash path\n", stderr);
        goto done;
    }
    result = 0;
done:
    fb_block_slot_tables = 0;
    fb_block_pointer_arrays = 0;
    markdown_core_map_free(map);
    return result;
}

/* Public constructors fail cleanly instead of returning a partial object
 * when the allocator refuses every allocation, and the definition/lookup
 * entry points tolerate the resulting NULL maps. */
static int case_constructor_oom(void) {
    markdown_core_parser *parser;
    markdown_core_map *map;
    markdown_core_chunk label = fb_chunk("ref");
    int result = -1;

    fb_block_all_callocs = 1;
    parser = markdown_core_parser_new_with_mem(MARKDOWN_CORE_OPT_DEFAULT, &fb_failing_mem);
    map = markdown_core_reference_map_new(&fb_failing_mem);
    fb_block_all_callocs = 0;

    if (parser) {
        fputs("parser constructor returned an object under OOM\n", stderr);
        markdown_core_parser_free(parser);
        return -1;
    }
    if (map) {
        fputs("map constructor returned an object under OOM\n", stderr);
        markdown_core_map_free(map);
        return -1;
    }

    fb_create_reference(NULL, "ref", "/u");
    if (markdown_core_map_lookup(NULL, &label) != NULL) {
        fputs("NULL map lookup unexpectedly resolved\n", stderr);
        return -1;
    }
    result = 0;
    return result;
}

static char *fb_parse_directive_attributes(const char *input, markdown_core_mem *mem) {
    markdown_core_extension *extension = markdown_core_extension_find("directive");
    markdown_core_parser *parser;
    markdown_core_node *document;
    markdown_core_iter *iter;
    markdown_core_event_type event;
    char *result = NULL;

    if (!extension) {
        fputs("directive extension is not registered\n", stderr);
        return NULL;
    }
    parser = markdown_core_parser_new_with_mem(MARKDOWN_CORE_OPT_DIRECTIVE, mem);
    if (!parser) {
        return NULL;
    }
    if (!markdown_core_parser_attach_extension(parser, extension)) {
        markdown_core_parser_free(parser);
        return NULL;
    }
    markdown_core_parser_feed(parser, input, strlen(input));
    document = markdown_core_parser_finish(parser);
    markdown_core_parser_free(parser);
    if (!document) {
        return NULL;
    }

    iter = markdown_core_iter_new(document);
    while ((event = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        if (event == MARKDOWN_CORE_EVENT_ENTER) {
            const char *json = markdown_core_extensions_get_directive_attributes(markdown_core_iter_get_node(iter));
            if (json) {
                result = fb_strdup(json);
                break;
            }
        }
    }
    markdown_core_iter_free(iter);
    markdown_core_node_free(document);
    return result;
}

static int fb_compare_directive_paths(const char *input, const char *expected, const char *context) {
    char *control;
    char *fallback;
    size_t blocked_before;
    int result = -1;

    control = fb_parse_directive_attributes(input, markdown_core_mem_default());
    if (!control) {
        fprintf(stderr, "%s: control parse produced no attributes\n", context);
        return -1;
    }
    blocked_before = fb_blocked_allocations;
    fb_block_slot_tables = 1;
    fallback = fb_parse_directive_attributes(input, &fb_failing_mem);
    fb_block_slot_tables = 0;
    if (!fallback) {
        fprintf(stderr, "%s: fallback parse produced no attributes\n", context);
        goto done;
    }
    if (fb_blocked_allocations == blocked_before) {
        fprintf(stderr, "%s: injected allocator never fired\n", context);
        goto done;
    }
    if (strcmp(control, fallback) != 0) {
        fprintf(stderr, "%s: paths disagree\n  hash:   %s\n  sorted: %s\n", context, control, fallback);
        goto done;
    }
    if (expected && strcmp(control, expected) != 0) {
        fprintf(stderr, "%s: unexpected attributes\n  actual:   %s\n  expected: %s\n", context, control, expected);
        goto done;
    }
    result = 0;
done:
    free(control);
    free(fallback);
    return result;
}

/* Directive attribute normalization keeps first-position/last-value-wins
 * through both the hash index and the pointer-sort fallback, for small and
 * large duplicate-heavy inputs. */
static int case_directive_sorted_fallback(void) {
    enum { FB_ATTRIBUTE_COUNT = 2050, FB_UNIQUE_KEYS = 64 };
    char *input;
    char *expected;
    size_t input_length = 0;
    size_t expected_length = 0;
    size_t i;
    int result = -1;

    if (fb_compare_directive_paths(":x{a=1 b=2 a=3 c=4 b=5}\n", "{\"a\":\"3\",\"b\":\"5\",\"c\":\"4\"}", "small") !=
        0) {
        return -1;
    }

    /* The large case forces several transactional hash growths before the
     * injected slot-table failure enters the sorted fallback. */
    input = (char *)malloc(FB_ATTRIBUTE_COUNT * 24 + 16);
    expected = (char *)malloc(FB_UNIQUE_KEYS * 24 + 16);
    if (!input || !expected) {
        free(input);
        free(expected);
        return -1;
    }
    input_length += (size_t)snprintf(input + input_length, 8, ":x{");
    for (i = 0; i < FB_ATTRIBUTE_COUNT; i++) {
        input_length += (size_t)snprintf(input + input_length, 24, "%sk%zu=v%zu", i ? " " : "", i % FB_UNIQUE_KEYS, i);
    }
    input_length += (size_t)snprintf(input + input_length, 8, "}\n");
    expected_length += (size_t)snprintf(expected + expected_length, 8, "{");
    for (i = 0; i < FB_UNIQUE_KEYS; i++) {
        size_t last = i + FB_UNIQUE_KEYS * ((FB_ATTRIBUTE_COUNT - 1 - i) / FB_UNIQUE_KEYS);
        expected_length +=
            (size_t)snprintf(expected + expected_length, 32, "%s\"k%zu\":\"v%zu\"", i ? "," : "", i, last);
    }
    expected_length += (size_t)snprintf(expected + expected_length, 8, "}");

    result = fb_compare_directive_paths(input, expected, "large");
    free(input);
    free(expected);
    return result;
}

/* v2 definition-map semantics through one lookup path: definitions may be
 * added after lookups (no freeze), duplicate labels keep every definition
 * with the minimum document order winning, and owner-tagged definitions
 * retract with winner re-election. */
static int fb_check_map_v2_path(markdown_core_mem *mem, const char *context) {
    markdown_core_map *map = markdown_core_reference_map_new(mem);
    int result = -1;

    if (!map) {
        fprintf(stderr, "%s: map constructor failed\n", context);
        return -1;
    }

    /* Winner is the first (minimum document order) definition. */
    fb_create_reference(map, "a", "/a1");
    fb_create_reference(map, "b", "/b1");
    if (fb_expect_url(map, "a", "/a1", context) != 0) {
        goto done;
    }

    /* Inserts after a lookup land without a freeze; the winner stays the
     * older definition, and brand-new labels resolve. */
    fb_create_reference(map, "a", "/a2");
    fb_create_reference(map, "c", "/c1");
    if (fb_expect_url(map, "a", "/a1", context) != 0 || fb_expect_url(map, "b", "/b1", context) != 0 ||
        fb_expect_url(map, "c", "/c1", context) != 0) {
        goto done;
    }

    /* Retracting the winning definition's owner re-elects the survivor;
     * retracting a label's only definition removes the label. */
    map->pending_owner = 7;
    fb_create_reference(map, "d", "/d-owned");
    fb_create_reference(map, "only", "/only-owned");
    map->pending_owner = 0;
    fb_create_reference(map, "d", "/d-survivor");
    if (fb_expect_url(map, "d", "/d-owned", context) != 0 || fb_expect_url(map, "only", "/only-owned", context) != 0) {
        goto done;
    }
    markdown_core_map_remove_owned(map, 7);
    if (fb_expect_url(map, "d", "/d-survivor", context) != 0 || fb_expect_url(map, "only", NULL, context) != 0 ||
        fb_expect_url(map, "a", "/a1", context) != 0) {
        goto done;
    }
    result = 0;
done:
    markdown_core_map_free(map);
    return result;
}

/* Backward-shift deletion keeps the probe runs of the shared byte-key index
 * intact across interleaved removals and re-inserts. */
static int fb_check_key_index_remove(void) {
    markdown_core_key_index index;
    char keys[10][8];
    size_t i;
    int result = -1;

    if (!markdown_core_key_index_init(&index, markdown_core_mem_default())) {
        fputs("key index initialization failed\n", stderr);
        return -1;
    }
    for (i = 0; i < 10; i++) {
        snprintf(keys[i], sizeof(keys[i]), "k%zu", i);
        if (!markdown_core_key_index_insert(
                &index,
                (const unsigned char *)keys[i],
                (markdown_core_bufsize)strlen(keys[i]),
                (void *)(uintptr_t)(i + 1),
                0,
                NULL
            )) {
            fprintf(stderr, "key index insert %zu failed\n", i);
            goto done;
        }
    }
    if (markdown_core_key_index_remove(&index, (const unsigned char *)"absent", 6) != 0) {
        fputs("removing an absent key reported success\n", stderr);
        goto done;
    }
    /* Remove every even key, then verify the odd ones still resolve through
     * the shifted probe runs. */
    for (i = 0; i < 10; i += 2) {
        if (!markdown_core_key_index_remove(
                &index,
                (const unsigned char *)keys[i],
                (markdown_core_bufsize)strlen(keys[i])
            )) {
            fprintf(stderr, "key index remove %zu failed\n", i);
            goto done;
        }
    }
    if (index.size != 5) {
        fprintf(stderr, "expected 5 keys after removal, found %zu\n", index.size);
        goto done;
    }
    for (i = 0; i < 10; i++) {
        void *value = markdown_core_key_index_lookup(
            &index,
            (const unsigned char *)keys[i],
            (markdown_core_bufsize)strlen(keys[i])
        );
        void *expected = (i % 2 == 0) ? NULL : (void *)(uintptr_t)(i + 1);
        if (value != expected) {
            fprintf(stderr, "lookup %zu after removal returned the wrong value\n", i);
            goto done;
        }
    }
    /* Removed keys can come back. */
    if (!markdown_core_key_index_insert(
            &index,
            (const unsigned char *)keys[0],
            (markdown_core_bufsize)strlen(keys[0]),
            (void *)(uintptr_t)99,
            0,
            NULL
        ) ||
        markdown_core_key_index_lookup(
            &index,
            (const unsigned char *)keys[0],
            (markdown_core_bufsize)strlen(keys[0])
        ) != (void *)(uintptr_t)99) {
        fputs("re-insert after removal failed\n", stderr);
        goto done;
    }
    result = 0;
done:
    markdown_core_key_index_free(&index);
    return result;
}

/* v2 map semantics resolve identically through the hash index and through
 * the allocation-failure pointer-sort fallback, and the byte-key index
 * survives removals. */
static int case_reference_map_v2(void) {
    size_t blocked_before;

    if (fb_check_map_v2_path(markdown_core_mem_default(), "v2 hash path") != 0) {
        return -1;
    }

    blocked_before = fb_blocked_allocations;
    fb_block_slot_tables = 1;
    if (fb_check_map_v2_path(&fb_failing_mem, "v2 sorted fallback") != 0) {
        fb_block_slot_tables = 0;
        return -1;
    }
    fb_block_slot_tables = 0;
    if (fb_blocked_allocations == blocked_before) {
        fputs("injected allocator never fired\n", stderr);
        return -1;
    }

    return fb_check_key_index_remove();
}

/* Mirrors hash_key in core/map.c.  If that hash ever changes, the keys found
 * below stop clustering, the capacity assertions fail loudly, and this case
 * must be retuned together with the hash. */
static uint64_t fb_hash(const char *key) {
    uint64_t hash = UINT64_C(1469598103934665603);
    const unsigned char *cursor;
    for (cursor = (const unsigned char *)key; *cursor; cursor++) {
        hash ^= *cursor;
        hash *= UINT64_C(1099511628211);
    }
    hash ^= hash >> 33;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    hash ^= hash >> 33;
    hash *= UINT64_C(0xc4ceb9fe1a85ec53);
    hash ^= hash >> 33;
    return hash ? hash : 1;
}

/* Probe exhaustion below the load-factor bound grows the table once instead
 * of failing: 64 keys homing on one slot of a 128-slot table exhaust the
 * probe limit for a 65th, and one doubling disperses the constructed cluster
 * because the keys split evenly across both candidate homes at 256. The
 * normal insertion path grows from the shared minimum capacity to 128; the
 * test does not inject a cardinality-specific initialization hint. */
static int case_key_index_probe_growth(void) {
    enum { FB_WINDOW = 7, FB_HALF = 32, FB_CLUSTER = 2 * FB_HALF };
    char keys[FB_CLUSTER + 1][24];
    size_t low = 0, high = 0, total = 0;
    unsigned long candidate = 0;
    markdown_core_key_index index;
    size_t i;
    int result = -1;

    while (total < FB_CLUSTER && candidate < 100000000UL) {
        char name[24];
        uint64_t hash;
        snprintf(name, sizeof(name), "p%lu", candidate++);
        hash = fb_hash(name);
        if ((hash & 127) != FB_WINDOW) {
            continue;
        }
        if ((hash & 255) == FB_WINDOW && low < FB_HALF) {
            snprintf(keys[total++], sizeof(keys[0]), "%s", name);
            low++;
        } else if ((hash & 255) == FB_WINDOW + 128 && high < FB_HALF) {
            snprintf(keys[total++], sizeof(keys[0]), "%s", name);
            high++;
        }
    }
    while (total == FB_CLUSTER && candidate < 200000000UL) {
        char name[24];
        snprintf(name, sizeof(name), "p%lu", candidate++);
        if ((fb_hash(name) & 127) == FB_WINDOW) {
            snprintf(keys[total++], sizeof(keys[0]), "%s", name);
            break;
        }
    }
    if (total != FB_CLUSTER + 1) {
        fputs("could not construct clustered keys; retune with core/map.c hash\n", stderr);
        return -1;
    }

    if (!markdown_core_key_index_init(&index, markdown_core_mem_default())) {
        fputs("index initialization failed\n", stderr);
        return -1;
    }
    if (index.capacity != 16) {
        fprintf(stderr, "unexpected initial capacity %zu\n", index.capacity);
        goto done;
    }
    for (i = 0; i < FB_CLUSTER; i++) {
        if (!markdown_core_key_index_insert(
                &index,
                (const unsigned char *)keys[i],
                (markdown_core_bufsize)strlen(keys[i]),
                (void *)(uintptr_t)(i + 1),
                0,
                NULL
            )) {
            fprintf(stderr, "cluster insert %zu failed\n", i);
            goto done;
        }
    }
    if (index.capacity != 128 || index.size != FB_CLUSTER) {
        fputs("cluster did not fill the table as constructed; retune with core/map.c hash\n", stderr);
        goto done;
    }
    if (!markdown_core_key_index_insert(
            &index,
            (const unsigned char *)keys[FB_CLUSTER],
            (markdown_core_bufsize)strlen(keys[FB_CLUSTER]),
            (void *)(uintptr_t)(FB_CLUSTER + 1),
            0,
            NULL
        )) {
        fputs("probe-exhausted insert failed instead of growing\n", stderr);
        goto done;
    }
    if (index.capacity != 256 || index.size != FB_CLUSTER + 1) {
        fprintf(
            stderr,
            "expected one growth to capacity 256, found capacity %zu size %zu\n",
            index.capacity,
            index.size
        );
        goto done;
    }
    for (i = 0; i < FB_CLUSTER + 1; i++) {
        void *value = markdown_core_key_index_lookup(
            &index,
            (const unsigned char *)keys[i],
            (markdown_core_bufsize)strlen(keys[i])
        );
        if (value != (void *)(uintptr_t)(i + 1)) {
            fprintf(stderr, "lookup %zu returned the wrong value after growth\n", i);
            goto done;
        }
    }
    if (markdown_core_key_index_lookup(&index, (const unsigned char *)"absent", 6) != NULL) {
        fputs("absent key unexpectedly resolved\n", stderr);
        goto done;
    }
    result = 0;
done:
    markdown_core_key_index_free(&index);
    return result;
}

static markdown_core_map *fb_skewed_reference_map(int unique_at_head) {
    enum { FB_SKEW_TOTAL = 16384, FB_SKEW_UNIQUE = 1024 };
    markdown_core_map *map = markdown_core_reference_map_new(markdown_core_mem_default());
    size_t phase;
    size_t i;

    if (!map) {
        return NULL;
    }
    for (phase = 0; phase < 2; phase++) {
        int add_unique = unique_at_head ? phase == 1 : phase == 0;
        size_t count = add_unique ? FB_SKEW_UNIQUE : FB_SKEW_TOTAL - FB_SKEW_UNIQUE;
        for (i = 0; i < count; i++) {
            char label[32];
            if (add_unique) {
                snprintf(label, sizeof(label), "unique-%zu", i);
            } else {
                snprintf(label, sizeof(label), "duplicate");
            }
            fb_create_reference(map, label, "/u");
        }
    }
    return map;
}

/* The index footprint follows the number of distinct keys regardless of
 * whether a unique or duplicate-heavy region appears at the list head. This
 * defeats the former first-1024 sampling heuristic with both prefix/tail
 * orientations. */
static int case_key_index_skewed_cardinality(void) {
    enum { FB_MAX_UNIQUE_CAPACITY = 8192 };
    markdown_core_map *unique_head = fb_skewed_reference_map(1);
    markdown_core_map *duplicate_head = fb_skewed_reference_map(0);
    int result = -1;

    if (!unique_head || !duplicate_head || !markdown_core_map_ensure_index(unique_head) ||
        !markdown_core_map_ensure_index(duplicate_head)) {
        fputs("skewed reference index construction failed\n", stderr);
        goto done;
    }
    if (unique_head->index.size != 1025 || duplicate_head->index.size != 1025) {
        fprintf(
            stderr,
            "skewed maps lost unique keys: %zu / %zu\n",
            unique_head->index.size,
            duplicate_head->index.size
        );
        goto done;
    }
    if (unique_head->index.capacity > FB_MAX_UNIQUE_CAPACITY ||
        duplicate_head->index.capacity > FB_MAX_UNIQUE_CAPACITY) {
        fprintf(
            stderr,
            "index capacity followed occurrence order instead of unique keys: %zu / %zu\n",
            unique_head->index.capacity,
            duplicate_head->index.capacity
        );
        goto done;
    }
    result = 0;
done:
    markdown_core_map_free(unique_head);
    markdown_core_map_free(duplicate_head);
    return result;
}

/* A unique prefix followed by a much longer duplicate tail used to size the
 * directive index from the total occurrence count. Observe the actual slot
 * allocation so the same space invariant is pinned for the second caller of
 * the shared index. */
static int case_directive_skewed_cardinality(void) {
    enum { FB_ATTRIBUTE_COUNT = 8192, FB_UNIQUE_PREFIX = 1024, FB_MAX_UNIQUE_CAPACITY = 8192 };
    char *input = (char *)malloc(FB_ATTRIBUTE_COUNT * 24 + 16);
    char *attributes = NULL;
    size_t written = 0;
    size_t i;
    int result = -1;

    if (!input) {
        return -1;
    }
    written += (size_t)snprintf(input + written, 8, ":x{");
    for (i = 0; i < FB_ATTRIBUTE_COUNT; i++) {
        size_t key = i < FB_UNIQUE_PREFIX ? i : 0;
        written += (size_t)
            snprintf(input + written, FB_ATTRIBUTE_COUNT * 24 + 16 - written, "%sk%zu=v%zu", i ? " " : "", key, i);
    }
    written += (size_t)snprintf(input + written, FB_ATTRIBUTE_COUNT * 24 + 16 - written, "}\n");

    fb_observer_reset();
    fb_observe_allocations = 1;
    attributes = fb_parse_directive_attributes(input, &fb_failing_mem);
    fb_observe_allocations = 0;
    if (!attributes) {
        fputs("skewed directive parse failed\n", stderr);
        goto done;
    }
    if (fb_max_slot_capacity > FB_MAX_UNIQUE_CAPACITY) {
        fprintf(
            stderr,
            "directive index capacity followed occurrences instead of unique keys: %zu\n",
            fb_max_slot_capacity
        );
        goto done;
    }
    result = 0;
done:
    fb_observe_allocations = 0;
    free(attributes);
    free(input);
    return result;
}

/* Every inline unit starts from an empty delimiter topology but retains the
 * parser-owned lane and record storage. Hundreds of singleton-delimiter
 * paragraphs across two documents must therefore allocate each scratch
 * buffer exactly once, not once per paragraph or document. */
static int case_delimiter_scratch_reuse(void) {
    enum { FB_PARAGRAPHS = 256 };
    char *input = (char *)malloc(FB_PARAGRAPHS * 4 + 1);
    markdown_core_parser *parser = NULL;
    markdown_core_node *first = NULL;
    markdown_core_node *second = NULL;
    size_t i;
    int result = -1;

    if (!input) {
        return -1;
    }
    for (i = 0; i < FB_PARAGRAPHS; i++) {
        memcpy(input + i * 4, "*x\n\n", 4);
    }
    input[FB_PARAGRAPHS * 4] = '\0';

    parser = markdown_core_parser_new_with_mem(MARKDOWN_CORE_OPT_DEFAULT, &fb_failing_mem);
    if (!parser) {
        goto done;
    }
    fb_observer_reset();
    fb_observe_allocations = 1;
    markdown_core_parser_feed(parser, input, FB_PARAGRAPHS * 4);
    first = markdown_core_parser_finish(parser);
    markdown_core_parser_feed(parser, input, FB_PARAGRAPHS * 4);
    second = markdown_core_parser_finish(parser);
    fb_observe_allocations = 0;

    if (!first || !second) {
        fputs("delimiter scratch reuse parse failed\n", stderr);
        goto done;
    }
    if (fb_lane_allocations != 1 || fb_record_allocations != 1) {
        fprintf(
            stderr,
            "delimiter scratch allocated %zu lane tables and %zu record arenas\n",
            fb_lane_allocations,
            fb_record_allocations
        );
        goto done;
    }
    if (parser->inline_delimiters.count || parser->inline_delimiters.tail || parser->inline_delimiters.capacity < 16 ||
        parser->inline_delimiters.lane_capacity < 4) {
        fputs("parser did not retain an empty reusable delimiter arena\n", stderr);
        goto done;
    }
    result = 0;
done:
    fb_observe_allocations = 0;
    markdown_core_node_free(first);
    markdown_core_node_free(second);
    if (parser) {
        markdown_core_parser_free(parser);
    }
    free(input);
    return result;
}

/* A complete backslash-pair run has an output already present in its source
 * prefix. Pin the allocation-free payload invariant as well as exact output
 * and scope, for a run long enough that the former bulk special case would
 * allocate a distinctive 2049-byte buffer. */
static int case_backslash_run_borrowed_payload(void) {
    enum { FB_RUN_BYTES = 4096, FB_OUTPUT_BYTES = FB_RUN_BYTES / 2 };
    char *input = (char *)malloc(FB_RUN_BYTES + 2);
    markdown_core_parser *parser = NULL;
    markdown_core_node *document = NULL;
    markdown_core_node *paragraph;
    markdown_core_node *text;
    size_t i;
    int result = -1;

    if (!input) {
        return -1;
    }
    memset(input, '\\', FB_RUN_BYTES);
    input[FB_RUN_BYTES] = '\n';
    input[FB_RUN_BYTES + 1] = '\0';

    parser = markdown_core_parser_new_with_mem(MARKDOWN_CORE_OPT_DEFAULT, &fb_failing_mem);
    if (!parser) {
        goto done;
    }
    for (i = 2; i <= 5; i++) {
        size_t expected_slashes = i / 2 + i % 2;
        size_t j;
        memset(input, '\\', i);
        input[i] = 'a';
        input[i + 1] = '\n';
        input[i + 2] = '\0';
        markdown_core_parser_feed(parser, input, i + 2);
        document = markdown_core_parser_finish(parser);
        paragraph = document ? document->first_child : NULL;
        text = paragraph ? paragraph->first_child : NULL;
        if (!text || text->type != MARKDOWN_CORE_NODE_TEXT || text->next ||
            text->as.literal.len != (markdown_core_bufsize)(expected_slashes + 1) ||
            text->end_column != (int32_t)(i + 1)) {
            fprintf(stderr, "backslash run length %zu produced the wrong node or scope\n", i);
            goto done;
        }
        for (j = 0; j < expected_slashes; j++) {
            if (text->as.literal.data[j] != '\\') {
                fprintf(stderr, "backslash run length %zu produced the wrong escape bytes\n", i);
                goto done;
            }
        }
        if (text->as.literal.data[expected_slashes] != 'a') {
            fprintf(stderr, "backslash run length %zu lost its odd/even suffix\n", i);
            goto done;
        }
        markdown_core_node_free(document);
        document = NULL;
    }

    memset(input, '\\', FB_RUN_BYTES);
    input[FB_RUN_BYTES] = '\n';
    input[FB_RUN_BYTES + 1] = '\0';
    fb_observer_reset();
    fb_backslash_payload_size = FB_OUTPUT_BYTES + 1;
    fb_observe_allocations = 1;
    markdown_core_parser_feed(parser, input, FB_RUN_BYTES + 1);
    document = markdown_core_parser_finish(parser);
    fb_observe_allocations = 0;

    paragraph = document ? document->first_child : NULL;
    text = paragraph ? paragraph->first_child : NULL;
    if (!text || text->type != MARKDOWN_CORE_NODE_TEXT || text->next || text->as.literal.len != FB_OUTPUT_BYTES ||
        text->start_column != 1 || text->end_column != FB_RUN_BYTES) {
        fputs("backslash run produced the wrong node, payload length, or scope\n", stderr);
        goto done;
    }
    for (i = 0; i < FB_OUTPUT_BYTES; i++) {
        if (text->as.literal.data[i] != '\\') {
            fputs("backslash run produced the wrong literal bytes\n", stderr);
            goto done;
        }
    }
    if (fb_backslash_payload_allocations != 0) {
        fprintf(stderr, "backslash run allocated %zu transformed payload buffers\n", fb_backslash_payload_allocations);
        goto done;
    }
    result = 0;
done:
    fb_observe_allocations = 0;
    markdown_core_node_free(document);
    if (parser) {
        markdown_core_parser_free(parser);
    }
    free(input);
    return result;
}

/* Full-feature corpus for the allocation-failure sweep. */
static const char FB_SWEEP_CORPUS[] =
    "# Heading *one*\n"
    "\n"
    "Paragraph with **strong**, _em_, `code`, [link](/url \"title\"), ![img](/i.png),\n"
    "a [ref][label], an <https://example.com/auto> autolink, www.example.com,\n"
    "mail@example.com, https://example.com/bare, ~~gone~~, &amp; entity,\n"
    "backslashes \\\\\\\\\\\\ and \\* escaped.\n"
    "\n"
    "[label]: /dest \"tt\"\n"
    "[label]: /dup\n"
    "\n"
    "> quote with footnote[^fn] and $x+y$ inline formula\n"
    "\n"
    "- item one\n"
    "- item two\n"
    "  1. nested\n"
    "\n"
    "- [ ] task open\n"
    "- [x] task done\n"
    "\n"
    "| a | b |\n"
    "| - | :-: |\n"
    "| 1 | 2 |\n"
    "\n"
    "```info string\n"
    "code block\n"
    "```\n"
    "\n"
    "$$\n"
    "x^2\n"
    "$$\n"
    "\n"
    ":::note[Label]{k=1 k=2 other=\"v\"}\n"
    "directive body\n"
    ":::\n"
    "\n"
    ":inline{a=1 b=2 a=3}\n"
    "\n"
    // Shorthand and repeated classes: `class` accumulates rather than
    // replacing, and that merge allocates, so the sweep must reach it.
    ":shorthand{#one .red .blue class=green k=v}\n"
    "\n"
    // An inline directive carrying a label: the label opener emits the
    // directive before the bracket, and that allocation must be swept too.
    ":cite[a *b*]{k=v} tail\n"
    "\n"
    "Cross [[folder/note#heading]] and embed ![[asset.png|preview]].\n"
    "\n"
    "[^fn]: footnote *body*\n"
    "\n"
    "<!-- comment -->\n"
    "text after <span>html</span>\n";

static const char *FB_SWEEP_EXTENSIONS[] =
    {"table", "strikethrough", "autolink", "tasklist", "formula", "directive", "cross_link", "embed"};

static markdown_core_node *fb_sweep_parse(markdown_core_mem *mem) {
    int options = MARKDOWN_CORE_OPT_DIRECTIVE | MARKDOWN_CORE_OPT_FOOTNOTES | MARKDOWN_CORE_OPT_SMART;
    markdown_core_parser *parser = markdown_core_parser_new_with_mem(options, mem);
    markdown_core_node *root;
    size_t i;

    if (!parser) {
        return NULL;
    }
    for (i = 0; i < sizeof(FB_SWEEP_EXTENSIONS) / sizeof(FB_SWEEP_EXTENSIONS[0]); i++) {
        markdown_core_extension *extension = markdown_core_extension_find(FB_SWEEP_EXTENSIONS[i]);
        if (!extension || !markdown_core_parser_attach_extension(parser, extension)) {
            markdown_core_parser_free(parser);
            return NULL;
        }
    }
    markdown_core_parser_feed(parser, FB_SWEEP_CORPUS, strlen(FB_SWEEP_CORPUS));
    root = markdown_core_parser_finish(parser);
    markdown_core_parser_free(parser);
    return root;
}

/* Allocation-free comparison: the sweep allocator is still armed while
 * comparing, so the comparator must not allocate (public literal accessors
 * do). */
static int fb_chunk_equal(const markdown_core_chunk *a, const markdown_core_chunk *b) {
    if (a->len != b->len) {
        return 0;
    }
    if (a->len == 0) {
        return 1;
    }
    if (!a->data || !b->data) {
        return a->data == b->data;
    }
    return memcmp(a->data, b->data, (size_t)a->len) == 0;
}

static int fb_node_payload_equal(markdown_core_node *a, markdown_core_node *b) {
    markdown_core_node_type type = markdown_core_node_get_type(a);
    if (type == MARKDOWN_CORE_NODE_TEXT || type == MARKDOWN_CORE_NODE_CODE || type == MARKDOWN_CORE_NODE_HTML ||
        type == MARKDOWN_CORE_NODE_CODE_BLOCK || type == MARKDOWN_CORE_NODE_HTML_BLOCK) {
        return fb_chunk_equal(&a->as.literal, &b->as.literal);
    }
    if (type == MARKDOWN_CORE_NODE_LINK || type == MARKDOWN_CORE_NODE_IMAGE) {
        return fb_chunk_equal(&a->as.link.url, &b->as.link.url) && fb_chunk_equal(&a->as.link.title, &b->as.link.title);
    }
    if (type == MARKDOWN_CORE_NODE_CROSS_LINK || type == MARKDOWN_CORE_NODE_EMBED) {
        return fb_chunk_equal(markdown_core_cross_reference_value(a), markdown_core_cross_reference_value(b));
    }
    return 1;
}

static int fb_node_has_literal(markdown_core_node *node) {
    markdown_core_node_type type = markdown_core_node_get_type(node);
    return type == MARKDOWN_CORE_NODE_TEXT || type == MARKDOWN_CORE_NODE_CODE || type == MARKDOWN_CORE_NODE_HTML ||
           type == MARKDOWN_CORE_NODE_CODE_BLOCK || type == MARKDOWN_CORE_NODE_HTML_BLOCK;
}

static void fb_describe_node(const char *side, markdown_core_node *node) {
    if (!node) {
        fprintf(stderr, "  %s: (missing)\n", side);
        return;
    }
    if (fb_node_has_literal(node) && node->as.literal.data && node->as.literal.len >= 0) {
        fprintf(
            stderr,
            "  %s: type=%d literal='%.*s' (len %d)\n",
            side,
            (int)markdown_core_node_get_type(node),
            (int)(node->as.literal.len < 40 ? node->as.literal.len : 40),
            (const char *)node->as.literal.data,
            (int)node->as.literal.len
        );
    } else {
        fprintf(stderr, "  %s: type=%d\n", side, (int)markdown_core_node_get_type(node));
    }
}

static int fb_tree_equal(markdown_core_node *a, markdown_core_node *b) {
    while (a || b) {
        if (!a || !b || markdown_core_node_get_type(a) != markdown_core_node_get_type(b) ||
            !fb_node_payload_equal(a, b)) {
            fb_describe_node("control", a);
            fb_describe_node("sweep", b);
            return 0;
        }
        if (!fb_tree_equal(markdown_core_node_first_child(a), markdown_core_node_first_child(b))) {
            return 0;
        }
        a = markdown_core_node_next(a);
        b = markdown_core_node_next(b);
    }
    return 1;
}

/* Injects an allocation failure at every single allocation the corpus parse
 * performs.  The unified contract: each injected failure must either surface
 * as a failed parse (NULL document) or leave the output byte-identical to
 * the control -- lossless fallbacks are the only path allowed to succeed. */
static int case_oom_sweep(void) {
    markdown_core_node *control;
    unsigned long total;
    unsigned long k;
    int result = -1;

    control = fb_sweep_parse(markdown_core_mem_default());
    if (!control) {
        fputs("control parse failed\n", stderr);
        return -1;
    }

    fb_sweep_count = 0;
    fb_sweep_fail_at = 0;
    {
        markdown_core_node *counted = fb_sweep_parse(&fb_sweep_mem);
        if (!counted || !fb_tree_equal(control, counted)) {
            fputs("counting parse diverged from control\n", stderr);
            if (counted) {
                markdown_core_node_free(counted);
            }
            goto done;
        }
        markdown_core_node_free(counted);
    }
    total = fb_sweep_count;
    if (total == 0 || total > 200000UL) {
        fprintf(stderr, "implausible allocation count %lu\n", total);
        goto done;
    }

    for (k = 1; k <= total; k++) {
        markdown_core_node *doc;
        fb_sweep_count = 0;
        fb_sweep_fail_at = k;
        fb_sweep_fired = 0;
        doc = fb_sweep_parse(&fb_sweep_mem);
        if (doc) {
            if (fb_sweep_fired && !fb_tree_equal(control, doc)) {
                fprintf(stderr, "allocation %lu / %lu: lossy document reported as success\n", k, total);
                markdown_core_node_free(doc);
                goto done;
            }
            markdown_core_node_free(doc);
        }
    }
    result = 0;
done:
    fb_sweep_fail_at = 0;
    markdown_core_node_free(control);
    return result;
}

/* Session OOM sweep: every allocation of an open + three edit/commit stages
 * fails exactly once. A failed commit must be transactional — the session
 * stays at its previous revision with its previous view — and a retry must
 * converge on the control result, footnote index included. Stage 3's
 * definition retargets the recorded [s][x] miss, so its commit rebuilds a
 * dependent paragraph that carries footnote sites (the clone-run merge). */

static const char FB_SESSION_STAGE1[] = "alpha[^a] sees [s][x] and beta[^b]\n\n[^b]: b body\n";
static const char FB_SESSION_STAGE2[] = "# zero[^b]\n\n[^a]: a body\n\n";
static const char FB_SESSION_STAGE3[] = "[x]: /url\n\n";

/* Dumps through the facade (plain malloc, uncounted by the sweep). Returns
 * NULL only on dump failure. */
static uint8_t *fb_session_dump(markdown_core_document *session, size_t *length) {
    uint8_t *dump = NULL;
    markdown_core_error *error = NULL;
    if (!markdown_core_document_dump(session, &dump, length, &error)) {
        markdown_core_error_free(error);
        return NULL;
    }
    return dump;
}

typedef struct {
    markdown_core_node_id id;
    size_t ordinal;
    markdown_core_node_type type;
} fb_tree_entry;

typedef struct {
    fb_tree_entry *items;
    size_t count;
} fb_tree_snapshot;

typedef struct {
    size_t ordinal;
    markdown_core_node_type type;
    uint32_t parts;
} fb_delta_mark;

typedef struct {
    uint64_t before;
    uint64_t after;
    fb_delta_mark *marks;
    size_t count;
} fb_delta_snapshot;

static void fb_tree_snapshot_release(fb_tree_snapshot *snapshot) {
    free(snapshot->items);
    memset(snapshot, 0, sizeof(*snapshot));
}

static int fb_tree_snapshot_capture(const markdown_core_document *session, fb_tree_snapshot *snapshot) {
    const markdown_core_node *root = session->root;
    const markdown_core_node *node = root;
    size_t count = 0;
    size_t ordinal = 0;

    while (node) {
        count++;
        if (node->first_child) {
            node = node->first_child;
            continue;
        }
        while (node != root && !node->next) {
            node = node->parent;
        }
        node = node == root ? NULL : node->next;
    }
    snapshot->items = (fb_tree_entry *)malloc(count * sizeof(*snapshot->items));
    if (!snapshot->items) {
        return -1;
    }
    snapshot->count = count;
    node = root;
    while (node) {
        snapshot->items[ordinal].id = node->id;
        snapshot->items[ordinal].ordinal = ordinal;
        snapshot->items[ordinal].type = (markdown_core_node_type)node->type;
        ordinal++;
        if (node->first_child) {
            node = node->first_child;
            continue;
        }
        while (node != root && !node->next) {
            node = node->parent;
        }
        node = node == root ? NULL : node->next;
    }
    return 0;
}

static const fb_tree_entry *fb_tree_snapshot_find(const fb_tree_snapshot *snapshot, markdown_core_node_id id) {
    size_t i;
    for (i = 0; i < snapshot->count; i++) {
        if (snapshot->items[i].id == id) {
            return &snapshot->items[i];
        }
    }
    return NULL;
}

static void fb_delta_snapshot_release(fb_delta_snapshot *snapshot) {
    free(snapshot->marks);
    memset(snapshot, 0, sizeof(*snapshot));
}

static int fb_delta_mark_compare(const void *lhs, const void *rhs) {
    const fb_delta_mark *a = (const fb_delta_mark *)lhs;
    const fb_delta_mark *b = (const fb_delta_mark *)rhs;
    if (a->ordinal != b->ordinal) {
        return a->ordinal < b->ordinal ? -1 : 1;
    }
    if (a->type != b->type) {
        return a->type < b->type ? -1 : 1;
    }
    if (a->parts != b->parts) {
        return a->parts < b->parts ? -1 : 1;
    }
    return 0;
}

static int fb_delta_snapshot_capture(
    fb_delta_snapshot *snapshot,
    const markdown_core_document *session,
    const fb_tree_snapshot *before_tree,
    const markdown_core_delta *changes
) {
    fb_tree_snapshot after_tree = {0};
    size_t i;

    if (fb_tree_snapshot_capture(session, &after_tree) != 0) {
        return -1;
    }
    markdown_core_delta_revisions(changes, &snapshot->before, &snapshot->after);
    {
        const markdown_core_diff *diffs = NULL;
        size_t count = markdown_core_delta_diffs(changes, &diffs);
        size_t k;
        if (count) {
            snapshot->marks = (fb_delta_mark *)malloc(count * sizeof(*snapshot->marks));
            if (!snapshot->marks) {
                fb_tree_snapshot_release(&after_tree);
                fb_delta_snapshot_release(snapshot);
                return -1;
            }
            for (k = 0; k < count; k++) {
                // A retired row names a node of the PREVIOUS tree; every other
                // row names one of this tree.
                const fb_tree_snapshot *tree = diffs[k].parts == 0 ? before_tree : &after_tree;
                const fb_tree_entry *entry = fb_tree_snapshot_find(tree, diffs[k].markup);
                if (!entry) {
                    fb_tree_snapshot_release(&after_tree);
                    fb_delta_snapshot_release(snapshot);
                    return -1;
                }
                snapshot->marks[k].ordinal = entry->ordinal;
                snapshot->marks[k].type = entry->type;
                snapshot->marks[k].parts = diffs[k].parts;
            }
            // Raw ids are session-local and an OOM retry may consume (but
            // never reuse) them, so compare structural effects in canonical
            // order rather than ids. The list's own order is gated by the
            // replay mirror, which walks it forward.
            qsort(snapshot->marks, count, sizeof(*snapshot->marks), fb_delta_mark_compare);
        }
        snapshot->count = count;
    }
    fb_tree_snapshot_release(&after_tree);
    return 0;
}

static int fb_delta_matches(
    const markdown_core_delta *changes,
    const markdown_core_document *session,
    const fb_tree_snapshot *before_tree,
    const fb_delta_snapshot *expected
) {
    fb_delta_snapshot actual = {0};
    int matches = 1;
    size_t i;

    if (fb_delta_snapshot_capture(&actual, session, before_tree, changes) != 0) {
        return 0;
    }
    matches = actual.before == expected->before && actual.after == expected->after;
    if (!matches) {
        fprintf(
            stderr,
            "delta revisions %llu..%llu != control %llu..%llu\n",
            (unsigned long long)actual.before,
            (unsigned long long)actual.after,
            (unsigned long long)expected->before,
            (unsigned long long)expected->after
        );
    }
    if (matches && actual.count != expected->count) {
        fprintf(stderr, "diffs count %zu != control %zu\n", actual.count, expected->count);
        matches = 0;
    }
    for (i = 0; matches && i < actual.count; i++) {
        if (actual.marks[i].ordinal != expected->marks[i].ordinal ||
            actual.marks[i].type != expected->marks[i].type || actual.marks[i].parts != expected->marks[i].parts) {
            fprintf(
                stderr,
                "diffs item %zu mark (%zu,%d,%u) != control (%zu,%d,%u)\n",
                i,
                actual.marks[i].ordinal,
                (int)actual.marks[i].type,
                actual.marks[i].parts,
                expected->marks[i].ordinal,
                (int)expected->marks[i].type,
                expected->marks[i].parts
            );
            matches = 0;
        }
    }
    fb_delta_snapshot_release(&actual);
    return matches;
}

/* One scripted run: a failed step is retried once (the injector fires at
 * most one failure per run). `stage_dumps[i]` receives the dump after stage
 * i's commit; failed commits are checked against the last committed dump.
 * When delta snapshots are requested, every failed commit must leave the
 * output null and every successful fallback/retry must match the control. */
static int fb_session_run(
    markdown_core_mem *mem,
    bool pooled,
    uint8_t **stage_dumps,
    size_t *stage_lengths,
    fb_delta_snapshot *captured_deltas,
    const fb_delta_snapshot *expected_deltas
) {
    markdown_core_document *session = markdown_core_document_open_with_mem(NULL, mem, pooled, NULL);
    const char *stages[3] = {FB_SESSION_STAGE1, FB_SESSION_STAGE2, FB_SESSION_STAGE3};
    size_t inserts[3] = {0, 0, 0};
    char accumulated[sizeof(FB_SESSION_STAGE1) + sizeof(FB_SESSION_STAGE2) + sizeof(FB_SESSION_STAGE3)];
    size_t accumulated_length = 0;
    uint8_t *committed_dump = NULL;
    size_t committed_length = 0;
    bool delta_aware = captured_deltas || expected_deltas;
    int stage;
    int result = -1;

    if (!session) {
        return 1; /* clean constructor failure */
    }
    accumulated[0] = 0;
    committed_dump = fb_session_dump(session, &committed_length);
    if (!committed_dump) {
        goto done;
    }

    for (stage = 0; stage < 3; stage++) {
        // The caller owns the text. Each stage prepends, so the accumulated
        // string is stage3 + stage2 + stage1, and the document is handed the
        // whole of it.
        size_t length = strlen(stages[stage]);
        (void)inserts;
        memmove(accumulated + length, accumulated, accumulated_length);
        memcpy(accumulated, stages[stage], length);
        accumulated_length += length;
        accumulated[accumulated_length] = 0;
        {
            uint64_t revision_before = markdown_core_document_revision(session);
            fb_tree_snapshot before_tree = {0};
            markdown_core_delta *changes = NULL;
            if (delta_aware && fb_tree_snapshot_capture(session, &before_tree) != 0) {
                fputs("could not capture pre-commit tree\n", stderr);
                goto done;
            }
            if (!mc_edit(&session, mc_sv(accumulated, accumulated_length), delta_aware ? &changes : NULL, NULL)) {
                uint8_t *view = NULL;
                size_t view_length = 0;
                if (changes) {
                    fputs("failed commit exposed a partial delta\n", stderr);
                    markdown_core_delta_free(changes);
                    fb_tree_snapshot_release(&before_tree);
                    goto done;
                }
                if (markdown_core_document_revision(session) != revision_before) {
                    fputs("failed commit advanced the revision\n", stderr);
                    fb_tree_snapshot_release(&before_tree);
                    goto done;
                }
                view = fb_session_dump(session, &view_length);
                if (!view || view_length != committed_length || memcmp(view, committed_dump, view_length) != 0) {
                    fputs("failed commit disturbed the committed view\n", stderr);
                    free(view);
                    fb_tree_snapshot_release(&before_tree);
                    goto done;
                }
                free(view);
                if (!mc_edit(&session, mc_sv(accumulated, accumulated_length), delta_aware ? &changes : NULL, NULL)) {
                    if (changes) {
                        fputs("failed commit retry exposed a partial delta\n", stderr);
                        markdown_core_delta_free(changes);
                    }
                    fputs("commit retry failed\n", stderr);
                    fb_tree_snapshot_release(&before_tree);
                    goto done;
                }
            }
            if (markdown_core_document_revision(session) != revision_before + 1) {
                fputs("commit did not advance the revision by one\n", stderr);
                markdown_core_delta_free(changes);
                fb_tree_snapshot_release(&before_tree);
                goto done;
            }
            if (delta_aware && !changes) {
                fputs("successful commit did not return a delta\n", stderr);
                fb_tree_snapshot_release(&before_tree);
                goto done;
            }
            if (changes && expected_deltas &&
                !fb_delta_matches(changes, session, &before_tree, &expected_deltas[stage])) {
                fprintf(stderr, "stage %d delta diverged from control\n", stage + 1);
                markdown_core_delta_free(changes);
                fb_tree_snapshot_release(&before_tree);
                goto done;
            }
            if (changes && captured_deltas &&
                fb_delta_snapshot_capture(&captured_deltas[stage], session, &before_tree, changes) != 0) {
                fputs("could not capture control delta\n", stderr);
                markdown_core_delta_free(changes);
                fb_tree_snapshot_release(&before_tree);
                goto done;
            }
            markdown_core_delta_free(changes);
            fb_tree_snapshot_release(&before_tree);
        }
        free(committed_dump);
        committed_dump = fb_session_dump(session, &committed_length);
        if (!committed_dump) {
            goto done;
        }
        if (stage_dumps) {
            stage_dumps[stage] = (uint8_t *)malloc(committed_length ? committed_length : 1);
            if (!stage_dumps[stage]) {
                goto done;
            }
            memcpy(stage_dumps[stage], committed_dump, committed_length);
            stage_lengths[stage] = committed_length;
        } else {
            /* Sweep run: converge on the recorded control dumps. */
        }
    }

    /* The footnote index must be coherent after retries: [^b] is referenced
     * (number 1) and [^a] resolves after stage 2. */
    {
        const markdown_core_node_id *ids = NULL;
        if (markdown_core_document_footnotes(session, &ids) != 2) {
            fputs("footnote index diverged after retries\n", stderr);
            goto done;
        }
    }

    result = 0;
done:
    free(committed_dump);
    markdown_core_document_release(session);
    return result;
}

static int fb_session_sweep(bool pooled, bool delta_aware) {
    uint8_t *control_dumps[3] = {NULL, NULL, NULL};
    size_t control_lengths[3] = {0, 0, 0};
    fb_delta_snapshot control_deltas[3] = {0};
    uint8_t *counted_dumps[3] = {NULL, NULL, NULL};
    size_t counted_lengths[3] = {0, 0, 0};
    unsigned long total;
    unsigned long k;
    size_t i;
    int result = -1;

    if (fb_session_run(
            markdown_core_mem_default(),
            pooled,
            control_dumps,
            control_lengths,
            delta_aware ? control_deltas : NULL,
            NULL
        ) != 0) {
        fputs("control session run failed\n", stderr);
        goto done;
    }

    fb_sweep_count = 0;
    fb_sweep_fail_at = 0;
    if (fb_session_run(
            &fb_sweep_mem,
            pooled,
            counted_dumps,
            counted_lengths,
            NULL,
            delta_aware ? control_deltas : NULL
        ) != 0 ||
        counted_lengths[2] != control_lengths[2] ||
        memcmp(counted_dumps[2], control_dumps[2], control_lengths[2]) != 0) {
        fputs("counting session run diverged from control\n", stderr);
        goto done;
    }
    total = fb_sweep_count;
    if (total == 0 || total > 200000UL) {
        fprintf(stderr, "implausible session allocation count %lu\n", total);
        goto done;
    }

    for (k = 1; k <= total; k++) {
        uint8_t *final_dumps[3] = {NULL, NULL, NULL};
        size_t final_lengths[3] = {0, 0, 0};
        int run;
        fb_sweep_count = 0;
        fb_sweep_fail_at = k;
        fb_sweep_fired = 0;
        run = fb_session_run(
            &fb_sweep_mem,
            pooled,
            final_dumps,
            final_lengths,
            NULL,
            delta_aware ? control_deltas : NULL
        );
        if (run < 0) {
            fprintf(stderr, "allocation %lu / %lu: session script broke\n", k, total);
            free(final_dumps[0]);
            free(final_dumps[1]);
            free(final_dumps[2]);
            goto done;
        }
        if (run == 0 && (final_lengths[2] != control_lengths[2] ||
                         memcmp(final_dumps[2], control_dumps[2], control_lengths[2]) != 0)) {
            fprintf(stderr, "allocation %lu / %lu: retried session diverged from control\n", k, total);
            free(final_dumps[0]);
            free(final_dumps[1]);
            free(final_dumps[2]);
            goto done;
        }
        free(final_dumps[0]);
        free(final_dumps[1]);
        free(final_dumps[2]);
    }
    result = 0;
done:
    fb_sweep_fail_at = 0;
    free(control_dumps[0]);
    free(control_dumps[1]);
    free(control_dumps[2]);
    free(counted_dumps[0]);
    free(counted_dumps[1]);
    free(counted_dumps[2]);
    for (i = 0; i < 3; i++) {
        fb_delta_snapshot_release(&control_deltas[i]);
    }
    return result;
}

/* Review regression (#35): with smart punctuation off, '.' and '-' produce
 * Text nodes backed by immortal static string literals, not the block's
 * content buffer. A seam that covers such a line must transplant them
 * without rebasing; a blind rebase pointed them into arbitrary staged
 * memory. The incremental dump must match a fresh session's. */
static int case_seam_static_literal(void) {
    static const char initial[] = "\n.\nold\n\nnext\n";
    markdown_core_parse_options options;
    markdown_core_document *session = NULL;
    markdown_core_document *fresh = NULL;
    uint8_t *incremental_dump = NULL;
    size_t incremental_length = 0;
    uint8_t *fresh_dump = NULL;
    size_t fresh_length = 0;
    int result = -1;

    markdown_core_parse_options_init(&options);
    options.smart_punctuation = false;

    session = markdown_core_document_new(mc_sv(initial, sizeof(initial) - 1), &options, NULL);
    if (!session) {
        fputs("FAILED: seam_static_literal: initial commit failed\n", stderr);
        goto done;
    }
    /* Replace "old" (bytes 3..6) so the seam covers the '.' line. The caller
     * splices its own string; the document is handed the result. */
    {
        markdown_core_commit out;
        char edited[sizeof(initial)];
        memcpy(edited, initial, sizeof(initial) - 1);
        memcpy(edited + 3, "new", 3);
        memset(&out, 0, sizeof(out));
        if (!markdown_core_document_edit(&session, mc_sv(edited, sizeof(initial) - 1), &out, NULL)) {
            fputs("FAILED: seam_static_literal: edit commit failed\n", stderr);
            goto done;
        }
        session = out.document;
        markdown_core_delta_free(out.delta);
    }
    incremental_dump = fb_session_dump(session, &incremental_length);

    fresh = markdown_core_document_new(mc_sv("\n.\nnew\n\nnext\n", sizeof(initial) - 1), &options, NULL);
    if (!fresh) {
        fputs("FAILED: seam_static_literal: fresh commit failed\n", stderr);
        goto done;
    }
    fresh_dump = fb_session_dump(fresh, &fresh_length);

    if (!incremental_dump || !fresh_dump || incremental_length != fresh_length ||
        memcmp(incremental_dump, fresh_dump, fresh_length) != 0) {
        fputs("FAILED: seam_static_literal: incremental dump diverged from a fresh session\n", stderr);
        goto done;
    }
    result = 0;
done:
    free(incremental_dump);
    free(fresh_dump);
    markdown_core_document_release(session);
    markdown_core_document_release(fresh);
    return result;
}

static int case_session_oom_sweep(void) { return fb_session_sweep(false, false); }

static int case_session_oom_sweep_delta(void) { return fb_session_sweep(false, true); }

/* The pooled sweep drives the same script through a session arena over the
 * injected allocator, so every base refill — slab, passthrough block, the
 * arena itself — fails in turn; transactionality and retry convergence must
 * hold exactly as they do against direct allocation. */
static int case_session_oom_sweep_pooled(void) { return fb_session_sweep(true, false); }

/* A constructor that refuses returns everything it took, or it is a leak with
 * no handle left to free it by. markdown_core_document_open_with_mem builds in
 * stages — the session record, then the arena, then the first source — and
 * each new stage adds a return that has to unwind the stages before it. The
 * sweeps above cannot see this: they measure convergence after a failed
 * commit, and a session that never opened has no state to converge.
 *
 * Scope, stated because it is narrower than the rule and was measured rather
 * than assumed: this counts blocks from the injected allocator, and the
 * session record itself is a system allocation made before any allocator is
 * chosen. Deleting the arena release from the unwind fails this case;
 * deleting only the free of the session record does not. That half is held by
 * LeakSanitizer instead — the CI sanitizer job runs with detect_leaks=1, and
 * ASan builds force the unpooled path, which is the path whose only leak is
 * the record. Between the two, both halves are pinned; neither alone. */
static int fb_open_unwind(bool pooled) {
    unsigned long total;
    unsigned long k;
    markdown_core_document *session;
    int failed = 0;

    fb_unwind_count = 0;
    fb_unwind_fail_at = 0;
    fb_unwind_live = 0;
    session = markdown_core_document_open_with_mem(NULL, &fb_unwind_mem, pooled, NULL);
    if (!session) {
        fprintf(stderr, "open_unwind: control open failed (pooled=%d)\n", (int)pooled);
        return -1;
    }
    total = fb_unwind_count;
    markdown_core_document_release(session);
    if (fb_unwind_live != 0) {
        fprintf(stderr, "open_unwind: a successful open/free left %ld blocks live\n", fb_unwind_live);
        return -1;
    }
    if (total == 0 || total > 4096) {
        fprintf(stderr, "open_unwind: implausible open allocation count %lu\n", total);
        return -1;
    }

    for (k = 1; k <= total; k++) {
        markdown_core_error *error = NULL;
        fb_unwind_count = 0;
        fb_unwind_fail_at = k;
        fb_unwind_live = 0;
        session = markdown_core_document_open_with_mem(NULL, &fb_unwind_mem, pooled, &error);
        if (session) {
            markdown_core_document_release(session);
        }
        markdown_core_error_free(error);
        if (fb_unwind_live != 0) {
            fprintf(
                stderr,
                "FAILED: open_unwind: allocation %lu / %lu (pooled=%d) refused the open and leaked %ld block(s)\n",
                k,
                total,
                (int)pooled,
                fb_unwind_live
            );
            failed = 1;
        }
    }
    fb_unwind_fail_at = 0;
    return failed ? -1 : 0;
}

static int case_session_open_unwind(void) {
    int failed = 0;
    if (fb_open_unwind(false) != 0) {
        failed = 1;
    }
    if (fb_open_unwind(true) != 0) {
        failed = 1;
    }
    return failed ? -1 : 0;
}

static const markdown_core_node *fb_child_of_type(const markdown_core_node *parent, markdown_core_node_type type) {
    const markdown_core_node *child;

    for (child = parent ? parent->first_child : NULL; child; child = child->next) {
        if (child->type == type) {
            return child;
        }
    }
    return NULL;
}

/* A reference carries no destination. It names a label; which definition that
 * label resolves to is an answer, and the destination is stated once at that
 * definition, where the source writes it. So this asks the answer first and
 * reads the winning definition second — which is also why retargeting a
 * definition leaves every reference node untouched. */
static int fb_reference_destination_is(
    const markdown_core_document *session,
    const markdown_core_node *node,
    const char *expected
) {
    markdown_core_reference_info info;
    const markdown_core_node *definition;
    markdown_core_string label;
    markdown_core_string destination;
    markdown_core_string title;
    size_t length = strlen(expected);

    if (!node || !markdown_core_document_reference_info(session, node_by_id(markdown_core_document_root(session), markdown_core_node_get_id(node)), &info) ||
        info.definition == 0) {
        return 0;
    }
    definition = node_by_id(markdown_core_document_root(session), info.definition);
    return definition && markdown_core_node_reference_definition_properties(definition, &label, &destination, &title) &&
           destination.length == length && memcmp(destination.data, expected, length) == 0;
}

/* A CHUNK BOUNDARY MUST NOT CHANGE THE DOCUMENT (8.2: streaming is ordinary
 * editing, and there is no finalize step). The feed interface buffers a
 * partial line across calls, so every split point is a state the parser can
 * legitimately be handed — including the one that lands BETWEEN a CR and its
 * LF, where the parser must remember that the previous buffer ended with a
 * carriage return and not open a second line.
 *
 * That seam is not hypothetical. This branch has already been bitten by it
 * once, in the substrate work, where a cross-run CR completion patched a
 * unit's length and not its terminator bits.
 *
 * `test_feed_across_line_ending` in the api suite asserts the paragraph count
 * for one such split. This gates the general property — every split of a
 * CRLF-bearing document reproduces the single-feed tree exactly — and it lives
 * here rather than there because the api label is not in the coverage preset,
 * so a gate written there is invisible to the ratchet that is supposed to keep
 * this path covered. */
static char *fb_feed_shape(const char *text, size_t length, size_t split) {
    markdown_core_parser *parser = markdown_core_parser_new(MARKDOWN_CORE_OPT_DEFAULT);
    markdown_core_node *document;
    markdown_core_iter *iter;
    markdown_core_event_type event;
    char shape[4096];
    size_t used = 0;
    if (!parser) {
        return NULL;
    }
    shape[0] = 0;
    if (split) {
        markdown_core_parser_feed(parser, text, split);
    }
    if (split < length) {
        markdown_core_parser_feed(parser, text + split, length - split);
    }
    document = markdown_core_parser_finish(parser);
    markdown_core_parser_free(parser);
    if (!document) {
        return NULL;
    }
    iter = markdown_core_iter_new(document);
    while ((event = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        markdown_core_node *node = markdown_core_iter_get_node(iter);
        char line[128];
        const char *literal;
        if (event != MARKDOWN_CORE_EVENT_ENTER) {
            continue;
        }
        literal = markdown_core_node_get_literal(node);
        snprintf(
            line,
            sizeof(line),
            "%s|%s\n",
            markdown_core_node_get_type_string(node),
            literal ? literal : ""
        );
        if (used + strlen(line) + 1 >= sizeof(shape)) {
            /* The corpus is fixed and small; a truncated shape would compare
             * equal for the wrong reason, so fail loudly instead. */
            markdown_core_iter_free(iter);
            markdown_core_node_free(document);
            fprintf(stderr, "feed_chunk_seam: shape buffer too small\n");
            return NULL;
        }
        memcpy(shape + used, line, strlen(line) + 1);
        used += strlen(line);
    }
    markdown_core_iter_free(iter);
    markdown_core_node_free(document);
    return fb_strdup(shape);
}

static int case_feed_chunk_seam(void) {
    static const char TEXT[] = "# head\r\n\r\npara one\r\nlazy two\r\n\r\n- a\r\n- b\r\n\r\n```\r\ncode\r\n```\r\n";
    const size_t length = sizeof(TEXT) - 1;
    char *whole = fb_feed_shape(TEXT, length, length);
    size_t split;
    int failed = 0;

    if (!whole) {
        fprintf(stderr, "FAILED: feed_chunk_seam: single-feed parse failed\n");
        return -1;
    }
    for (split = 0; split <= length && !failed; split++) {
        char *part = fb_feed_shape(TEXT, length, split);
        if (!part) {
            fprintf(stderr, "FAILED: feed_chunk_seam: parse failed at split %zu\n", split);
            failed = 1;
            break;
        }
        if (strcmp(part, whole) != 0) {
            fprintf(
                stderr,
                "FAILED: feed_chunk_seam: split %zu of %zu changes the document\n  one feed: %s  two feeds: %s",
                split,
                length,
                whole,
                part
            );
            failed = 1;
        }
        free(part);
    }
    free(whole);
    return failed ? -1 : 0;
}

typedef struct fb_case_entry {
    const char *name;
    int (*run)(void);
} fb_case_entry;

static const fb_case_entry FB_CASES[] = {
    {"reference_sorted_fallback", case_reference_sorted_fallback},
    {"reference_map_v2", case_reference_map_v2},
    {"directive_sorted_fallback", case_directive_sorted_fallback},
    {"key_index_probe_growth", case_key_index_probe_growth},
    {"key_index_skewed_cardinality", case_key_index_skewed_cardinality},
    {"directive_skewed_cardinality", case_directive_skewed_cardinality},
    {"delimiter_scratch_reuse", case_delimiter_scratch_reuse},
    {"backslash_run_borrowed_payload", case_backslash_run_borrowed_payload},
    {"map_prepare_oom", case_map_prepare_oom},
    {"constructor_oom", case_constructor_oom},
    {"oom_sweep", case_oom_sweep},
    {"seam_static_literal", case_seam_static_literal},
    {"session_oom_sweep", case_session_oom_sweep},
    {"session_oom_sweep_delta", case_session_oom_sweep_delta},
    {"session_oom_sweep_pooled", case_session_oom_sweep_pooled},
    {"session_open_unwind", case_session_open_unwind},
    {"feed_chunk_seam", case_feed_chunk_seam},
};

int main(int argc, char **argv) {
    const char *case_name = NULL;
    int list_only = 0;
    size_t i;

    for (i = 1; i < (size_t)argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            list_only = 1;
        } else if (strcmp(argv[i], "--case") == 0 && i + 1 < (size_t)argc) {
            case_name = argv[++i];
        } else {
            fputs("usage: fallback_runner [--list] [--case NAME]\n", stderr);
            return 2;
        }
    }

    if (list_only) {
        for (i = 0; i < sizeof(FB_CASES) / sizeof(FB_CASES[0]); i++) {
            puts(FB_CASES[i].name);
        }
        return 0;
    }
    if (!case_name) {
        fputs("usage: fallback_runner [--list] [--case NAME]\n", stderr);
        return 2;
    }
    for (i = 0; i < sizeof(FB_CASES) / sizeof(FB_CASES[0]); i++) {
        if (strcmp(FB_CASES[i].name, case_name) == 0) {
            int failed = FB_CASES[i].run() != 0;
            printf("%s %s\n", case_name, failed ? "[FAILED]" : "[PASSED]");
            return failed;
        }
    }
    fprintf(stderr, "unknown case: %s\n", case_name);
    return 2;
}
