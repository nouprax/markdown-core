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
#include "session_internal.h"

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
    int options = MARKDOWN_CORE_OPT_DIRECTIVE | MARKDOWN_CORE_OPT_FOOTNOTES | MARKDOWN_CORE_OPT_SMART |
                  MARKDOWN_CORE_OPT_STRIP_HTML_COMMENTS;
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
static uint8_t *fb_session_dump(markdown_core_session *session, size_t *length) {
    uint8_t *dump = NULL;
    markdown_core_error *error = NULL;
    if (!markdown_core_document_dump(markdown_core_session_document(session), &dump, length, &error)) {
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
} fb_delta_mark;

enum { FB_DELTA_ADDED, FB_DELTA_REMOVED, FB_DELTA_CHANGED, FB_DELTA_BUBBLED, FB_DELTA_ARRAY_COUNT };

typedef struct {
    uint64_t before;
    uint64_t after;
    fb_delta_mark *marks[FB_DELTA_ARRAY_COUNT];
    size_t counts[FB_DELTA_ARRAY_COUNT];
} fb_delta_snapshot;

static void fb_tree_snapshot_release(fb_tree_snapshot *snapshot) {
    free(snapshot->items);
    memset(snapshot, 0, sizeof(*snapshot));
}

static int fb_tree_snapshot_capture(const markdown_core_session *session, fb_tree_snapshot *snapshot) {
    const markdown_core_node *root = session->view.root;
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

static size_t fb_delta_array(const markdown_core_delta *changes, size_t index, const markdown_core_node_id **ids) {
    switch (index) {
    case FB_DELTA_ADDED:
        return markdown_core_delta_added(changes, ids);
    case FB_DELTA_REMOVED:
        return markdown_core_delta_removed(changes, ids);
    case FB_DELTA_CHANGED:
        return markdown_core_delta_changed(changes, ids);
    default:
        return markdown_core_delta_bubbled(changes, ids);
    }
}

static int fb_delta_contains(const markdown_core_delta *changes, size_t index, markdown_core_node_id expected) {
    const markdown_core_node_id *ids = NULL;
    size_t count = fb_delta_array(changes, index, &ids);
    size_t i;

    for (i = 0; i < count; i++) {
        if (ids[i] == expected) {
            return 1;
        }
    }
    return 0;
}

static void fb_delta_snapshot_release(fb_delta_snapshot *snapshot) {
    size_t i;
    for (i = 0; i < FB_DELTA_ARRAY_COUNT; i++) {
        free(snapshot->marks[i]);
    }
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
    return 0;
}

static int fb_delta_snapshot_capture(
    fb_delta_snapshot *snapshot,
    const markdown_core_session *session,
    const fb_tree_snapshot *before_tree,
    const markdown_core_delta *changes
) {
    fb_tree_snapshot after_tree = {0};
    size_t i;

    if (fb_tree_snapshot_capture(session, &after_tree) != 0) {
        return -1;
    }
    markdown_core_delta_revisions(changes, &snapshot->before, &snapshot->after);
    for (i = 0; i < FB_DELTA_ARRAY_COUNT; i++) {
        const markdown_core_node_id *ids = NULL;
        size_t count = fb_delta_array(changes, i, &ids);
        const fb_tree_snapshot *tree = i == FB_DELTA_REMOVED ? before_tree : &after_tree;
        size_t k;
        if (count) {
            snapshot->marks[i] = (fb_delta_mark *)malloc(count * sizeof(*snapshot->marks[i]));
            if (!snapshot->marks[i]) {
                fb_tree_snapshot_release(&after_tree);
                fb_delta_snapshot_release(snapshot);
                return -1;
            }
            for (k = 0; k < count; k++) {
                const fb_tree_entry *entry = fb_tree_snapshot_find(tree, ids[k]);
                if (!entry) {
                    fb_tree_snapshot_release(&after_tree);
                    fb_delta_snapshot_release(snapshot);
                    return -1;
                }
                snapshot->marks[i][k].ordinal = entry->ordinal;
                snapshot->marks[i][k].type = entry->type;
            }
            // Delta arrays are semantic sets. Raw ids are session-local, and
            // an OOM retry may consume (but never reuse) ids or alter emission
            // order, so compare their structural effects in canonical order.
            qsort(snapshot->marks[i], count, sizeof(*snapshot->marks[i]), fb_delta_mark_compare);
        }
        snapshot->counts[i] = count;
    }
    fb_tree_snapshot_release(&after_tree);
    return 0;
}

static int fb_delta_matches(
    const markdown_core_delta *changes,
    const markdown_core_session *session,
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
    for (i = 0; i < FB_DELTA_ARRAY_COUNT; i++) {
        size_t k;
        if (actual.counts[i] != expected->counts[i]) {
            fprintf(stderr, "delta array %zu count %zu != control %zu\n", i, actual.counts[i], expected->counts[i]);
            matches = 0;
            break;
        }
        for (k = 0; k < actual.counts[i]; k++) {
            if (actual.marks[i][k].ordinal != expected->marks[i][k].ordinal ||
                actual.marks[i][k].type != expected->marks[i][k].type) {
                fprintf(
                    stderr,
                    "delta array %zu item %zu mark (%zu,%d) != control (%zu,%d)\n",
                    i,
                    k,
                    actual.marks[i][k].ordinal,
                    (int)actual.marks[i][k].type,
                    expected->marks[i][k].ordinal,
                    (int)expected->marks[i][k].type
                );
                matches = 0;
                break;
            }
        }
        if (!matches) {
            break;
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
    markdown_core_session *session = markdown_core_session_open_with_mem(NULL, mem, pooled, NULL);
    const char *stages[3] = {FB_SESSION_STAGE1, FB_SESSION_STAGE2, FB_SESSION_STAGE3};
    size_t inserts[3] = {0, 0, 0};
    uint8_t *committed_dump = NULL;
    size_t committed_length = 0;
    bool delta_aware = captured_deltas || expected_deltas;
    int stage;
    int result = -1;

    if (!session) {
        return 1; /* clean constructor failure */
    }
    committed_dump = fb_session_dump(session, &committed_length);
    if (!committed_dump) {
        goto done;
    }

    for (stage = 0; stage < 3; stage++) {
        size_t length = strlen(stages[stage]);
        if (!markdown_core_session_edit(
                session,
                inserts[stage],
                inserts[stage],
                (const uint8_t *)stages[stage],
                length,
                NULL
            ) &&
            !markdown_core_session_edit(
                session,
                inserts[stage],
                inserts[stage],
                (const uint8_t *)stages[stage],
                length,
                NULL
            )) {
            fputs("session edit failed twice\n", stderr);
            goto done;
        }
        {
            uint64_t revision_before = markdown_core_session_revision(session);
            fb_tree_snapshot before_tree = {0};
            markdown_core_delta *changes = NULL;
            if (delta_aware && fb_tree_snapshot_capture(session, &before_tree) != 0) {
                fputs("could not capture pre-commit tree\n", stderr);
                goto done;
            }
            if (!markdown_core_session_commit(session, delta_aware ? &changes : NULL, NULL)) {
                uint8_t *view = NULL;
                size_t view_length = 0;
                if (changes) {
                    fputs("failed commit exposed a partial delta\n", stderr);
                    markdown_core_delta_free(changes);
                    fb_tree_snapshot_release(&before_tree);
                    goto done;
                }
                if (markdown_core_session_revision(session) != revision_before) {
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
                if (!markdown_core_session_commit(session, delta_aware ? &changes : NULL, NULL)) {
                    if (changes) {
                        fputs("failed commit retry exposed a partial delta\n", stderr);
                        markdown_core_delta_free(changes);
                    }
                    fputs("commit retry failed\n", stderr);
                    fb_tree_snapshot_release(&before_tree);
                    goto done;
                }
            }
            if (markdown_core_session_revision(session) != revision_before + 1) {
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
        if (markdown_core_session_footnotes(session, &ids) != 2) {
            fputs("footnote index diverged after retries\n", stderr);
            goto done;
        }
    }

    result = 0;
done:
    free(committed_dump);
    markdown_core_session_free(session);
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
    markdown_core_session *session = NULL;
    markdown_core_session *fresh = NULL;
    uint8_t *incremental_dump = NULL;
    size_t incremental_length = 0;
    uint8_t *fresh_dump = NULL;
    size_t fresh_length = 0;
    int result = -1;

    markdown_core_parse_options_init(&options);
    options.smart_punctuation = false;

    session = markdown_core_session_open(&options, NULL);
    if (!session || !markdown_core_session_edit(session, 0, 0, (const uint8_t *)initial, sizeof(initial) - 1, NULL) ||
        !markdown_core_session_commit(session, NULL, NULL)) {
        fputs("FAILED: seam_static_literal: initial commit failed\n", stderr);
        goto done;
    }
    /* Replace "old" (bytes 3..6) so the seam covers the '.' line. */
    if (!markdown_core_session_edit(session, 3, 6, (const uint8_t *)"new", 3, NULL) ||
        !markdown_core_session_commit(session, NULL, NULL)) {
        fputs("FAILED: seam_static_literal: edit commit failed\n", stderr);
        goto done;
    }
    incremental_dump = fb_session_dump(session, &incremental_length);

    fresh = markdown_core_session_open(&options, NULL);
    if (!fresh ||
        !markdown_core_session_edit(fresh, 0, 0, (const uint8_t *)"\n.\nnew\n\nnext\n", sizeof(initial) - 1, NULL) ||
        !markdown_core_session_commit(fresh, NULL, NULL)) {
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
    markdown_core_session_free(session);
    markdown_core_session_free(fresh);
    return result;
}

static int case_session_oom_sweep(void) { return fb_session_sweep(false, false); }

static int case_session_oom_sweep_delta(void) { return fb_session_sweep(false, true); }

/* The pooled sweep drives the same script through a session arena over the
 * injected allocator, so every base refill — slab, passthrough block, the
 * arena itself — fails in turn; transactionality and retry convergence must
 * hold exactly as they do against direct allocation. */
static int case_session_oom_sweep_pooled(void) { return fb_session_sweep(true, false); }

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
    const markdown_core_session *session,
    const markdown_core_node *node,
    const char *expected
) {
    markdown_core_reference_info info;
    const markdown_core_node *definition;
    markdown_core_string_view label;
    markdown_core_string_view destination;
    markdown_core_string_view title;
    size_t length = strlen(expected);

    if (!node || !markdown_core_session_reference_info(session, markdown_core_node_get_id(node), &info) ||
        info.definition == 0) {
        return 0;
    }
    definition = markdown_core_session_node_by_id(session, info.definition);
    return definition && markdown_core_node_reference_definition_properties(definition, &label, &destination, &title) &&
           destination.length == length && memcmp(destination.data, expected, length) == 0;
}

static const markdown_core_lookup_record *fb_lookup_record_find(
    const markdown_core_lookup_table *table,
    markdown_core_node_id id
) {
    size_t slot;

    for (slot = 0; slot < table->capacity; slot++) {
        if (table->keys[slot] == id) {
            return &table->records[slot];
        }
    }
    return NULL;
}

/* Verifies both directions of one persistent lookup-index relation:
 * posting(label)[position] == (unit, ordinal), and
 * record(unit).positions[ordinal] == position. Expected units are a set:
 * every one must occur exactly once, with no extra or duplicate entry. */
static int fb_lookup_posting_matches(
    const markdown_core_session *session,
    const char *label,
    const markdown_core_node_id *expected_units,
    size_t expected_count
) {
    const markdown_core_lookup_table *table = &session->lookups;
    const markdown_core_lookup_posting *posting =
        markdown_core_lookup_postings_find(table, (const unsigned char *)label);
    size_t position;
    size_t unit_index;

    if (!posting || !posting->label || (posting->count && !posting->items) ||
        strcmp((const char *)posting->label, label) != 0 || posting->count != expected_count) {
        return 0;
    }
    for (position = 0; position < posting->count; position++) {
        const markdown_core_lookup_posting_entry *entry = &posting->items[position];
        const markdown_core_lookup_record *record = fb_lookup_record_find(table, entry->unit);
        size_t expected_matches = 0;

        for (unit_index = 0; unit_index < expected_count; unit_index++) {
            if (expected_units[unit_index] == entry->unit) {
                expected_matches++;
            }
        }
        if (expected_matches != 1 || !record || !record->labels || !record->positions ||
            entry->ordinal >= record->count || !record->labels[entry->ordinal] ||
            strcmp((const char *)record->labels[entry->ordinal], label) != 0 ||
            record->positions[entry->ordinal] != position) {
            return 0;
        }
    }
    for (unit_index = 0; unit_index < expected_count; unit_index++) {
        const markdown_core_lookup_record *record = fb_lookup_record_find(table, expected_units[unit_index]);
        size_t posting_matches = 0;
        size_t label_matches = 0;
        size_t ordinal = 0;

        for (position = 0; position < posting->count; position++) {
            if (posting->items[position].unit == expected_units[unit_index]) {
                posting_matches++;
            }
        }
        if (!record || !record->labels || !record->positions) {
            return 0;
        }
        for (size_t candidate = 0; candidate < record->count; candidate++) {
            if (record->labels[candidate] && strcmp((const char *)record->labels[candidate], label) == 0) {
                label_matches++;
                ordinal = candidate;
            }
        }
        if (posting_matches != 1 || label_matches != 1 || record->positions[ordinal] >= posting->count ||
            posting->items[record->positions[ordinal]].unit != expected_units[unit_index] ||
            posting->items[record->positions[ordinal]].ordinal != ordinal) {
            return 0;
        }
    }
    return 1;
}

/* A block directive label is an inline ownership domain of the semantic
 * DirectiveBlock, not an excuse to rebuild the document. Retargeting a
 * definition used only by that label must keep the owner and its block
 * content live, rebuild the label in place, and finish through the same
 * incremental commit inventory as core Paragraph/Heading owners. */
static int case_block_directive_label_lookup_locality(void) {
    static const char source[] = "[reference]: /a\n"
                                 "\n"
                                 ":::note[See [reference]]\n"
                                 "Body untouched\n"
                                 ":::\n"
                                 "\n"
                                 "Tail untouched\n";
    static const char *const destinations[] = {"/b", "/a"};
    const char *destination_at = strstr(source, "/a");
    markdown_core_session *session = NULL;
    markdown_core_delta *changes = NULL;
    const markdown_core_node *root;
    const markdown_core_node *directive;
    const markdown_core_node *label;
    const markdown_core_node *label_first;
    const markdown_core_node *link;
    const markdown_core_node *body;
    const markdown_core_node *tail;
    markdown_core_node_id root_id;
    markdown_core_node_id definition_id = 0;
    markdown_core_node_id directive_id;
    markdown_core_node_id label_id;
    markdown_core_node_id link_id;
    markdown_core_node_id body_id;
    markdown_core_node_id tail_id;
    uint64_t directive_revision;
    uint64_t label_revision;
    uint64_t link_revision;
    uint64_t body_revision;
    uint64_t tail_revision;
    uintptr_t directive_address;
    uintptr_t label_address;
    uintptr_t body_address;
    size_t full_before;
    size_t restarted_before;
    size_t reflowed_before;
    const markdown_core_node_id *ids = NULL;
    size_t count;
    size_t round;
    int result = -1;

    if (!destination_at) {
        fputs("FAILED: block_directive_label_lookup_locality: source URL missing\n", stderr);
        return -1;
    }
    session = markdown_core_session_open(NULL, NULL);
    if (!session || !markdown_core_session_edit(session, 0, 0, (const uint8_t *)source, sizeof(source) - 1, NULL) ||
        !markdown_core_session_commit(session, NULL, NULL)) {
        fputs("FAILED: block_directive_label_lookup_locality: baseline commit failed\n", stderr);
        goto done;
    }

    root = markdown_core_document_root(markdown_core_session_document(session));
    // The leading `[reference]: /a` is a ReferenceDefinition node now, where
    // it used to be consumed and leave the directive as the first child.
    directive = root ? root->first_child : NULL;
    while (directive && directive->type == MARKDOWN_CORE_NODE_REFERENCE_DEFINITION) {
        definition_id = markdown_core_node_get_id(directive);
        directive = directive->next;
    }
    label = markdown_core_node_directive_label(directive);
    label_first = markdown_core_node_get_first_child(label);
    link = label_first ? label_first->next : NULL;
    body = markdown_core_node_get_next_sibling(label);
    tail = directive ? directive->next : NULL;
    if (!root || !directive || markdown_core_node_get_kind(directive) != MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK || !label ||
        markdown_core_node_get_kind(label) != MARKDOWN_CORE_KIND_DIRECTIVE_LABEL ||
        markdown_core_node_get_parent(label) != directive || !label_first ||
        label_first->type != MARKDOWN_CORE_NODE_TEXT || label_first->parent != label || !link ||
        link->type != MARKDOWN_CORE_NODE_LINK_REFERENCE || link->parent != label || link->next || !body ||
        markdown_core_node_get_next_sibling(label) != body || body->type != MARKDOWN_CORE_NODE_PARAGRAPH ||
        body->parent != directive || !tail || tail->type != MARKDOWN_CORE_NODE_PARAGRAPH || tail->next ||
        !fb_reference_destination_is(session, link, "/a")) {
        fputs("FAILED: block_directive_label_lookup_locality: unexpected baseline tree\n", stderr);
        goto done;
    }

    root_id = markdown_core_node_get_id(root);
    directive_id = markdown_core_node_get_id(directive);
    label_id = markdown_core_node_get_id(label);
    link_id = markdown_core_node_get_id(link);
    body_id = markdown_core_node_get_id(body);
    tail_id = markdown_core_node_get_id(tail);
    directive_revision = markdown_core_node_get_revision(directive);
    label_revision = markdown_core_node_get_revision(label);
    link_revision = markdown_core_node_get_revision(link);
    body_revision = markdown_core_node_get_revision(body);
    tail_revision = markdown_core_node_get_revision(tail);
    directive_address = (uintptr_t)directive;
    label_address = (uintptr_t)label;
    body_address = (uintptr_t)body;
    if (!fb_lookup_posting_matches(session, "reference", &label_id, 1)) {
        fputs("FAILED: block_directive_label_lookup_locality: baseline label posting is incoherent\n", stderr);
        goto done;
    }

    for (round = 0; round < sizeof(destinations) / sizeof(*destinations); round++) {
        full_before = session->full_commits;
        restarted_before = session->restarted_commits;
        reflowed_before = session->reflowed_commits;
        if (!markdown_core_session_edit(
                session,
                (size_t)(destination_at - source),
                (size_t)(destination_at - source) + strlen(destinations[round]),
                (const uint8_t *)destinations[round],
                strlen(destinations[round]),
                NULL
            ) ||
            !markdown_core_session_commit(session, &changes, NULL) || !changes) {
            fprintf(stderr, "FAILED: block_directive_label_lookup_locality: retarget %zu commit failed\n", round + 1);
            goto done;
        }
        /* The trailing paragraph is the explicit restart boundary contract:
         * this edit must both avoid full fallback and reflow there. */
        if (session->full_commits != full_before || session->restarted_commits != restarted_before + 1 ||
            session->reflowed_commits != reflowed_before + 1) {
            fprintf(
                stderr,
                "FAILED: block_directive_label_lookup_locality: retarget %zu expected "
                "full/restarted/reflowed %zu/%zu/%zu, got %zu/%zu/%zu\n",
                round + 1,
                full_before,
                restarted_before + 1,
                reflowed_before + 1,
                session->full_commits,
                session->restarted_commits,
                session->reflowed_commits
            );
            goto done;
        }

        directive = markdown_core_session_node_by_id(session, directive_id);
        label = markdown_core_session_node_by_id(session, label_id);
        link = markdown_core_session_node_by_id(session, link_id);
        body = markdown_core_session_node_by_id(session, body_id);
        tail = markdown_core_session_node_by_id(session, tail_id);
        if (!directive) {
            fputs("FAILED: block_directive_label_lookup_locality: semantic owner id disappeared\n", stderr);
            goto done;
        }
        if ((uintptr_t)directive != directive_address) {
            fputs("FAILED: block_directive_label_lookup_locality: semantic owner address changed\n", stderr);
            goto done;
        }
        if (!label || (uintptr_t)label != label_address || markdown_core_node_directive_label(directive) != label ||
            markdown_core_node_get_parent(label) != directive) {
            fputs("FAILED: block_directive_label_lookup_locality: DirectiveLabel identity diverged\n", stderr);
            goto done;
        }
        /* Retargeting the definition moves an answer, not a node. The
         * reference inside the label never carried the destination, so it is
         * byte-identical across the commit and nothing above it bubbles —
         * which is what makes a definition edit local to the definition.
         * This scenario still pins the locality it was written for: the
         * label's lookup posting must survive the rebuild, checked below. */
        if (markdown_core_node_get_revision(directive) != directive_revision) {
            fputs("FAILED: block_directive_label_lookup_locality: semantic owner revision moved\n", stderr);
            goto done;
        }
        if (markdown_core_node_get_revision(label) != label_revision) {
            fputs("FAILED: block_directive_label_lookup_locality: DirectiveLabel revision moved\n", stderr);
            goto done;
        }
        if (!link || link->parent != label || link->next ||
            !fb_reference_destination_is(session, link, destinations[round]) ||
            markdown_core_node_get_revision(link) != link_revision) {
            fprintf(
                stderr,
                "FAILED: block_directive_label_lookup_locality: label Link identity diverged "
                "(present=%d parent=%d next=%d destination=%d revision=%llu previous=%llu)\n",
                link != NULL,
                link && link->parent == label,
                link && link->next != NULL,
                link && fb_reference_destination_is(session, link, destinations[round]),
                (unsigned long long)markdown_core_node_get_revision(link),
                (unsigned long long)link_revision
            );
            goto done;
        }
        if (!body || (uintptr_t)body != body_address || body->parent != directive ||
            markdown_core_node_get_next_sibling(label) != body ||
            markdown_core_node_get_revision(body) != body_revision) {
            fputs("FAILED: block_directive_label_lookup_locality: block body identity diverged\n", stderr);
            goto done;
        }
        if (!tail || markdown_core_node_get_revision(tail) != tail_revision) {
            fputs("FAILED: block_directive_label_lookup_locality: clean tail identity diverged\n", stderr);
            goto done;
        }
        if (!fb_lookup_posting_matches(session, "reference", &label_id, 1)) {
            fputs("FAILED: block_directive_label_lookup_locality: rebuilt label posting is incoherent\n", stderr);
            goto done;
        }

        count = markdown_core_delta_added(changes, &ids);
        if (count != 0) {
            fputs("FAILED: block_directive_label_lookup_locality: retarget added nodes\n", stderr);
            goto done;
        }
        count = markdown_core_delta_removed(changes, &ids);
        if (count != 0) {
            fputs("FAILED: block_directive_label_lookup_locality: retarget removed nodes\n", stderr);
            goto done;
        }
        count = markdown_core_delta_changed(changes, &ids);
        if (count != 1 || !fb_delta_contains(changes, FB_DELTA_CHANGED, definition_id) ||
            fb_delta_contains(changes, FB_DELTA_CHANGED, link_id) ||
            fb_delta_contains(changes, FB_DELTA_CHANGED, label_id) ||
            fb_delta_contains(changes, FB_DELTA_CHANGED, directive_id) ||
            fb_delta_contains(changes, FB_DELTA_CHANGED, body_id) ||
            fb_delta_contains(changes, FB_DELTA_CHANGED, tail_id)) {
            fputs("FAILED: block_directive_label_lookup_locality: changed delta is not the definition\n", stderr);
            goto done;
        }
        /* The definition is a document child, so the only ancestor above it
         * is the document. The directive and its label are no longer on the
         * path from the change to the root — they were only ever there
         * because the destination lived inside the label. */
        count = markdown_core_delta_bubbled(changes, &ids);
        if (count != 1 || !fb_delta_contains(changes, FB_DELTA_BUBBLED, root_id) ||
            fb_delta_contains(changes, FB_DELTA_BUBBLED, label_id) ||
            fb_delta_contains(changes, FB_DELTA_BUBBLED, directive_id) ||
            fb_delta_contains(changes, FB_DELTA_BUBBLED, body_id) ||
            fb_delta_contains(changes, FB_DELTA_BUBBLED, tail_id)) {
            fputs("FAILED: block_directive_label_lookup_locality: ancestor bubble is not the document\n", stderr);
            goto done;
        }
        directive_revision = markdown_core_node_get_revision(directive);
        label_revision = markdown_core_node_get_revision(label);
        link_revision = markdown_core_node_get_revision(link);
        markdown_core_delta_free(changes);
        changes = NULL;
    }

    result = 0;
done:
    markdown_core_delta_free(changes);
    markdown_core_session_free(session);
    return result;
}

static char *fb_reference_fanout_source(size_t dependent_count, size_t *length_out, size_t *url_offset_out) {
    static const char prefix[] = "[x]: /aaaa\n\n";
    static const char use[] = "uses [u][x] here\n\n";
    static const char tail[] = "Tail untouched\n";
    const size_t prefix_length = sizeof(prefix) - 1;
    const size_t use_length = sizeof(use) - 1;
    const size_t tail_length = sizeof(tail) - 1;
    size_t length;
    char *source;
    char *at;
    size_t i;

    if (dependent_count > (SIZE_MAX - prefix_length - tail_length - 1) / use_length) {
        return NULL;
    }
    length = prefix_length + dependent_count * use_length + tail_length;
    source = (char *)malloc(length + 1);
    if (!source) {
        return NULL;
    }
    at = source;
    memcpy(at, prefix, prefix_length);
    at += prefix_length;
    for (i = 0; i < dependent_count; i++) {
        memcpy(at, use, use_length);
        at += use_length;
    }
    memcpy(at, tail, tail_length);
    at += tail_length;
    *at = '\0';
    *length_out = length;
    *url_offset_out = (size_t)(strstr(source, "/aaaa") - source);
    return source;
}

/* Cardinality is not a semantic or lifecycle distinction. The same
 * definition-retarget operation must use the same incremental ownership
 * mechanism below, at, and above the removed 64-dependent benchmark route.
 * A second retarget also proves every rebuilt lookup posting was reinstalled. */
static int fb_reference_fanout_scenario(size_t dependent_count) {
    static const char *const destinations[] = {"/bbbb", "/aaaa"};
    markdown_core_session *session = NULL;
    markdown_core_delta *changes = NULL;
    markdown_core_node_id *link_ids = NULL;
    markdown_core_node_id *paragraph_ids = NULL;
    uint64_t *link_revisions = NULL;
    markdown_core_node_id root_id;
    markdown_core_node_id definition_id = 0;
    markdown_core_node_id tail_id;
    uint64_t tail_revision;
    char *source = NULL;
    size_t source_length = 0;
    size_t url_offset = 0;
    const markdown_core_node *root;
    const markdown_core_node *node;
    size_t i;
    size_t round;
    int result = -1;

    source = fb_reference_fanout_source(dependent_count, &source_length, &url_offset);
    link_ids = (markdown_core_node_id *)malloc(dependent_count * sizeof(*link_ids));
    paragraph_ids = (markdown_core_node_id *)malloc(dependent_count * sizeof(*paragraph_ids));
    link_revisions = (uint64_t *)malloc(dependent_count * sizeof(*link_revisions));
    session = markdown_core_session_open(NULL, NULL);
    if (!source || !link_ids || !paragraph_ids || !link_revisions || !session ||
        !markdown_core_session_edit(session, 0, 0, (const uint8_t *)source, source_length, NULL) ||
        !markdown_core_session_commit(session, NULL, NULL)) {
        fprintf(stderr, "FAILED: reference_fanout_uniform_routing: %zu: baseline commit failed\n", dependent_count);
        goto done;
    }

    root = markdown_core_document_root(markdown_core_session_document(session));
    root_id = markdown_core_node_get_id(root);
    node = root ? root->first_child : NULL;
    // The leading `[x]: /aaaa` is a ReferenceDefinition node now, where it
    // used to be consumed and leave the document's children starting at the
    // first paragraph.
    while (node && node->type == MARKDOWN_CORE_NODE_REFERENCE_DEFINITION) {
        definition_id = markdown_core_node_get_id(node);
        node = node->next;
    }
    for (i = 0; i < dependent_count; i++) {
        const markdown_core_node *link;
        if (!node || node->type != MARKDOWN_CORE_NODE_PARAGRAPH) {
            fprintf(
                stderr,
                "FAILED: reference_fanout_uniform_routing: %zu: dependent paragraph %zu missing\n",
                dependent_count,
                i
            );
            goto done;
        }
        link = fb_child_of_type(node, MARKDOWN_CORE_NODE_LINK_REFERENCE);
        if (!link || !fb_reference_destination_is(session, link, "/aaaa")) {
            fprintf(
                stderr,
                "FAILED: reference_fanout_uniform_routing: %zu: dependent Link %zu missing\n",
                dependent_count,
                i
            );
            goto done;
        }
        paragraph_ids[i] = markdown_core_node_get_id(node);
        link_ids[i] = markdown_core_node_get_id(link);
        link_revisions[i] = markdown_core_node_get_revision(link);
        node = node->next;
    }
    if (!node || node->type != MARKDOWN_CORE_NODE_PARAGRAPH || node->next ||
        fb_child_of_type(node, MARKDOWN_CORE_NODE_LINK_REFERENCE)) {
        fprintf(stderr, "FAILED: reference_fanout_uniform_routing: %zu: tail shape diverged\n", dependent_count);
        goto done;
    }
    tail_id = markdown_core_node_get_id(node);
    tail_revision = markdown_core_node_get_revision(node);
    if (!fb_lookup_posting_matches(session, "x", paragraph_ids, dependent_count)) {
        fprintf(
            stderr,
            "FAILED: reference_fanout_uniform_routing: %zu: baseline posting is incoherent\n",
            dependent_count
        );
        goto done;
    }

    for (round = 0; round < sizeof(destinations) / sizeof(*destinations); round++) {
        const markdown_core_node_id *ids = NULL;
        size_t full_before = session->full_commits;
        size_t restarted_before = session->restarted_commits;
        size_t reflowed_before = session->reflowed_commits;
        size_t count;

        if (!markdown_core_session_edit(
                session,
                url_offset,
                url_offset + strlen(destinations[round]),
                (const uint8_t *)destinations[round],
                strlen(destinations[round]),
                NULL
            ) ||
            !markdown_core_session_commit(session, &changes, NULL) || !changes) {
            fprintf(
                stderr,
                "FAILED: reference_fanout_uniform_routing: %zu: retarget %zu failed\n",
                dependent_count,
                round + 1
            );
            goto done;
        }
        if (session->full_commits != full_before || session->restarted_commits != restarted_before + 1 ||
            session->reflowed_commits != reflowed_before + 1) {
            fprintf(
                stderr,
                "FAILED: reference_fanout_uniform_routing: %zu: retarget %zu expected "
                "full/restarted/reflowed %zu/%zu/%zu, got %zu/%zu/%zu\n",
                dependent_count,
                round + 1,
                full_before,
                restarted_before + 1,
                reflowed_before + 1,
                session->full_commits,
                session->restarted_commits,
                session->reflowed_commits
            );
            goto done;
        }
        count = markdown_core_delta_added(changes, &ids);
        if (count != 0) {
            fprintf(
                stderr,
                "FAILED: reference_fanout_uniform_routing: %zu: retarget %zu added nodes\n",
                dependent_count,
                round + 1
            );
            goto done;
        }
        count = markdown_core_delta_removed(changes, &ids);
        if (count != 0) {
            fprintf(
                stderr,
                "FAILED: reference_fanout_uniform_routing: %zu: retarget %zu removed nodes\n",
                dependent_count,
                round + 1
            );
            goto done;
        }
        /* Retargeting a definition changes the definition and nothing else.
         * The references never carried the destination, so their nodes are
         * byte-identical before and after and no fanout is routed to them —
         * which is the property this whole change exists to produce, and what
         * this scenario now pins. Only the definition is `changed`, and only
         * the root bubbles above it. */
        count = markdown_core_delta_changed(changes, &ids);
        if (count != 1 || !fb_delta_contains(changes, FB_DELTA_CHANGED, definition_id)) {
            fprintf(
                stderr,
                "FAILED: reference_fanout_uniform_routing: %zu: retarget %zu changed %zu nodes\n",
                dependent_count,
                round + 1,
                count
            );
            goto done;
        }
        count = markdown_core_delta_bubbled(changes, &ids);
        if (count != 1 || !fb_delta_contains(changes, FB_DELTA_BUBBLED, root_id)) {
            fprintf(
                stderr,
                "FAILED: reference_fanout_uniform_routing: %zu: retarget %zu bubbled %zu nodes\n",
                dependent_count,
                round + 1,
                count
            );
            goto done;
        }
        for (i = 0; i < dependent_count; i++) {
            const markdown_core_node *link = markdown_core_session_node_by_id(session, link_ids[i]);
            const markdown_core_node *paragraph = markdown_core_session_node_by_id(session, paragraph_ids[i]);
            /* The answer moved; the node did not. */
            if (!link || !paragraph || link->parent != paragraph ||
                !fb_reference_destination_is(session, link, destinations[round]) ||
                markdown_core_node_get_revision(link) != link_revisions[i] ||
                fb_delta_contains(changes, FB_DELTA_CHANGED, link_ids[i]) ||
                fb_delta_contains(changes, FB_DELTA_BUBBLED, paragraph_ids[i])) {
                fprintf(
                    stderr,
                    "FAILED: reference_fanout_uniform_routing: %zu: retarget %zu dependent %zu diverged\n",
                    dependent_count,
                    round + 1,
                    i
                );
                goto done;
            }
            link_revisions[i] = markdown_core_node_get_revision(link);
        }
        if (!fb_lookup_posting_matches(session, "x", paragraph_ids, dependent_count)) {
            fprintf(
                stderr,
                "FAILED: reference_fanout_uniform_routing: %zu: retarget %zu posting is incoherent\n",
                dependent_count,
                round + 1
            );
            goto done;
        }
        node = markdown_core_session_node_by_id(session, tail_id);
        if (!node || markdown_core_node_get_revision(node) != tail_revision ||
            fb_delta_contains(changes, FB_DELTA_CHANGED, tail_id) ||
            fb_delta_contains(changes, FB_DELTA_BUBBLED, tail_id)) {
            fprintf(
                stderr,
                "FAILED: reference_fanout_uniform_routing: %zu: retarget %zu disturbed tail\n",
                dependent_count,
                round + 1
            );
            goto done;
        }
        markdown_core_delta_free(changes);
        changes = NULL;
    }

    result = 0;
done:
    markdown_core_delta_free(changes);
    markdown_core_session_free(session);
    free(source);
    free(link_ids);
    free(paragraph_ids);
    free(link_revisions);
    return result;
}

static int case_reference_fanout_uniform_routing(void) {
    static const size_t dependent_counts[] = {1, 63, 64, 65, 129};
    size_t i;

    for (i = 0; i < sizeof(dependent_counts) / sizeof(*dependent_counts); i++) {
        if (fb_reference_fanout_scenario(dependent_counts[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

/* One same-length in-place edit for a restart-locality scenario. */
typedef struct {
    size_t lo;
    size_t hi;
    const char *insert;
} rl_edit;

/* Opens a session over `initial`, applies each edit as its own commit, and
 * pins the uniform-routing inventory: only the empty-session bootstrap is a
 * full commit. The whole-text insert is an incremental restart with no
 * boundary; every later local edit is an incremental, reflowed restart. No
 * input size or cardinality changes that mechanism. */
static int rl_cluster_scenario(const char *name, const char *initial, const rl_edit *edits, size_t edit_count) {
    markdown_core_parse_options options;
    markdown_core_session *session;
    size_t i;
    int result = -1;

    markdown_core_parse_options_init(&options);
    session = markdown_core_session_open(&options, NULL);
    if (!session) {
        fprintf(stderr, "FAILED: restart_locality_counters: %s: session open failed\n", name);
        return -1;
    }
    if (!markdown_core_session_edit(session, 0, 0, (const uint8_t *)initial, strlen(initial), NULL) ||
        !markdown_core_session_commit(session, NULL, NULL)) {
        fprintf(stderr, "FAILED: restart_locality_counters: %s: initial commit failed\n", name);
        goto done;
    }
    for (i = 0; i < edit_count; i++) {
        if (!markdown_core_session_edit(
                session,
                edits[i].lo,
                edits[i].hi,
                (const uint8_t *)edits[i].insert,
                strlen(edits[i].insert),
                NULL
            ) ||
            !markdown_core_session_commit(session, NULL, NULL)) {
            fprintf(stderr, "FAILED: restart_locality_counters: %s: edit %zu failed\n", name, i);
            goto done;
        }
    }
    if (session->full_commits != 1 || session->restarted_commits != edit_count + 1 ||
        session->reflowed_commits != edit_count) {
        fprintf(
            stderr,
            "FAILED: restart_locality_counters: %s: expected 1 full / %zu restarted / %zu reflowed, "
            "got %zu / %zu / %zu\n",
            name,
            edit_count + 1,
            edit_count,
            session->full_commits,
            session->restarted_commits,
            session->reflowed_commits
        );
        goto done;
    }
    result = 0;
done:
    markdown_core_session_free(session);
    return result;
}

/* Uniform incremental routing is shape-independent: the initial whole-text
 * insert restarts without a boundary, while a later head-definition edit
 * reflows at a sentinel clean entry. Neither operation routes by benchmark
 * shape or cardinality; the only full commit is empty-session bootstrap. */
static int case_restart_locality_counters(void) {
    static const char initial[] = "[a]: /a1\n"
                                  "[b]: /b1\n"
                                  "\n"
                                  "[c]: /c1\n"
                                  "\n"
                                  "uses [u][a] here\n";
    static const char retarget[] = "/c9";
    markdown_core_parse_options options;
    markdown_core_session *session;
    int result = -1;

    markdown_core_parse_options_init(&options);
    session = markdown_core_session_open(&options, NULL);
    if (!session) {
        fprintf(stderr, "FAILED: restart_locality_counters: session open failed\n");
        return -1;
    }
    if (!markdown_core_session_edit(session, 0, 0, (const uint8_t *)initial, sizeof(initial) - 1, NULL) ||
        !markdown_core_session_commit(session, NULL, NULL)) {
        fprintf(stderr, "FAILED: restart_locality_counters: initial commit failed\n");
        goto done;
    }
    /* Opening a session bootstraps the empty document through the full
     * lifecycle. The whole-text insert then uses the shared incremental
     * mechanism; with no committed suffix it has no reflow boundary. */
    if (session->full_commits != 1 || session->restarted_commits != 1 || session->reflowed_commits != 0) {
        fprintf(
            stderr,
            "FAILED: restart_locality_counters: whole-text insert expected 1 full / 1 restarted / "
            "0 reflowed, got %zu / %zu / %zu\n",
            session->full_commits,
            session->restarted_commits,
            session->reflowed_commits
        );
        goto done;
    }
    /* Retarget the last head definition: [c]'s destination at bytes 24..27. */
    if (!markdown_core_session_edit(session, 24, 27, (const uint8_t *)retarget, sizeof(retarget) - 1, NULL) ||
        !markdown_core_session_commit(session, NULL, NULL)) {
        fprintf(stderr, "FAILED: restart_locality_counters: retarget commit failed\n");
        goto done;
    }
    if (session->full_commits != 1) {
        fprintf(stderr, "FAILED: restart_locality_counters: a head-cluster edit fell back to a full reparse\n");
        goto done;
    }
    if (session->restarted_commits != 2 || session->reflowed_commits != 1) {
        fprintf(
            stderr,
            "FAILED: restart_locality_counters: expected 2 restarted / 1 reflowed, got %zu / %zu\n",
            session->restarted_commits,
            session->reflowed_commits
        );
        goto done;
    }

    /* Blank-separated footnote definitions stay open across their blanks, so
     * interior and tail body edits restart at sealing anchors and reflow at
     * the next sealing line — two fed lines per commit, never a full
     * reparse. */
    {
        static const rl_edit edits[] = {
            {17, 20, "TWO"},   /* interior body: reflow at [^c] */
            {28, 33, "THREE"}, /* tail body: reflow at the tail paragraph */
        };
        if (rl_cluster_scenario(
                "footnote cluster",
                "[^a]: one\n"
                "\n"
                "[^b]: two\n"
                "\n"
                "[^c]: three\n"
                "\n"
                "tail para\n",
                edits,
                sizeof(edits) / sizeof(*edits)
            ) != 0) {
            goto done;
        }
    }

    /* Blank-separated top-level quotes restart cleanly (the resolved half of
     * the reflow-delay pair): a front edit reflows at the second quote. */
    {
        static const rl_edit edits[] = {
            {4, 7, "ONE"},
        };
        if (rl_cluster_scenario(
                "quote cluster",
                "> q one\n"
                "\n"
                "> q two\n"
                "\n"
                "> q three\n",
                edits,
                sizeof(edits) / sizeof(*edits)
            ) != 0) {
            goto done;
        }
    }
    result = 0;
done:
    markdown_core_session_free(session);
    return result;
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
    {"block_directive_label_lookup_locality", case_block_directive_label_lookup_locality},
    {"reference_fanout_uniform_routing", case_reference_fanout_uniform_routing},
    {"restart_locality_counters", case_restart_locality_counters},
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
