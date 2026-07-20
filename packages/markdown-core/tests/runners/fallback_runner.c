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
#include "references.h"

#include "session_internal.h"

/* Injected allocator.  Only the targeted shapes fail: key-index slot tables
 * are calloc(capacity >= 16, sizeof(slot)) and the sorted-fallback pointer
 * arrays are calloc(count >= 2, sizeof(pointer)); every other allocation in
 * the engine uses calloc(1, size) or byte-sized elements. */
static size_t fb_blocked_allocations;
static int fb_block_slot_tables;
static int fb_block_pointer_arrays;
static int fb_block_all_callocs;

static void *fb_calloc(markdown_core_mem *mem, size_t nmemb, size_t size) {
    (void)mem;
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
    return realloc(pointer, size);
}

static void fb_free(markdown_core_mem *mem, void *pointer) {
    (void)mem;
    free(pointer);
}

static markdown_core_mem fb_failing_mem = {fb_calloc, fb_realloc, fb_free};

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
    chunk.len = (bufsize_t)strlen(text);
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
    if (ref->url.len != (bufsize_t)strlen(url) || memcmp(ref->url.data, url, ref->url.len) != 0) {
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
 * through both the hash index and the pointer-sort fallback, for the small
 * direct route and for the sampled-capacity route. */
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

    /* Above 1024 attributes the capacity estimate samples; the injected
     * failure covers the sample-index failure route as well. */
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

    result = fb_compare_directive_paths(input, expected, "sampled");
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

    if (!markdown_core_key_index_init(&index, markdown_core_mem_default(), 8)) {
        fputs("key index initialization failed\n", stderr);
        return -1;
    }
    for (i = 0; i < 10; i++) {
        snprintf(keys[i], sizeof(keys[i]), "k%zu", i);
        if (!markdown_core_key_index_insert(
                &index,
                (const unsigned char *)keys[i],
                (bufsize_t)strlen(keys[i]),
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
        if (!markdown_core_key_index_remove(&index, (const unsigned char *)keys[i], (bufsize_t)strlen(keys[i]))) {
            fprintf(stderr, "key index remove %zu failed\n", i);
            goto done;
        }
    }
    if (index.size != 5) {
        fprintf(stderr, "expected 5 keys after removal, found %zu\n", index.size);
        goto done;
    }
    for (i = 0; i < 10; i++) {
        void *value =
            markdown_core_key_index_lookup(&index, (const unsigned char *)keys[i], (bufsize_t)strlen(keys[i]));
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
            (bufsize_t)strlen(keys[0]),
            (void *)(uintptr_t)99,
            0,
            NULL
        ) ||
        markdown_core_key_index_lookup(&index, (const unsigned char *)keys[0], (bufsize_t)strlen(keys[0])) !=
            (void *)(uintptr_t)99) {
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
 * of failing: 64 keys homing on one slot of a 256-slot table exhaust the
 * probe limit for a 65th, and one doubling disperses the constructed cluster
 * because the keys split evenly across both candidate homes at 512. */
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
        if ((hash & 255) != FB_WINDOW) {
            continue;
        }
        if ((hash & 511) == FB_WINDOW && low < FB_HALF) {
            snprintf(keys[total++], sizeof(keys[0]), "%s", name);
            low++;
        } else if ((hash & 511) == FB_WINDOW + 256 && high < FB_HALF) {
            snprintf(keys[total++], sizeof(keys[0]), "%s", name);
            high++;
        }
    }
    while (total == FB_CLUSTER && candidate < 200000000UL) {
        char name[24];
        snprintf(name, sizeof(name), "p%lu", candidate++);
        if ((fb_hash(name) & 255) == FB_WINDOW) {
            snprintf(keys[total++], sizeof(keys[0]), "%s", name);
            break;
        }
    }
    if (total != FB_CLUSTER + 1) {
        fputs("could not construct clustered keys; retune with core/map.c hash\n", stderr);
        return -1;
    }

    /* expected_size 128 selects the 256-slot table the cluster targets. */
    if (!markdown_core_key_index_init(&index, markdown_core_mem_default(), 128)) {
        fputs("index initialization failed\n", stderr);
        return -1;
    }
    if (index.capacity != 256) {
        fprintf(stderr, "unexpected initial capacity %zu\n", index.capacity);
        goto done;
    }
    for (i = 0; i < FB_CLUSTER; i++) {
        if (!markdown_core_key_index_insert(
                &index,
                (const unsigned char *)keys[i],
                (bufsize_t)strlen(keys[i]),
                (void *)(uintptr_t)(i + 1),
                0,
                NULL
            )) {
            fprintf(stderr, "cluster insert %zu failed\n", i);
            goto done;
        }
    }
    if (index.capacity != 256 || index.size != FB_CLUSTER) {
        fputs("cluster did not fill the table as constructed; retune with core/map.c hash\n", stderr);
        goto done;
    }
    if (!markdown_core_key_index_insert(
            &index,
            (const unsigned char *)keys[FB_CLUSTER],
            (bufsize_t)strlen(keys[FB_CLUSTER]),
            (void *)(uintptr_t)(FB_CLUSTER + 1),
            0,
            NULL
        )) {
        fputs("probe-exhausted insert failed instead of growing\n", stderr);
        goto done;
    }
    if (index.capacity != 512 || index.size != FB_CLUSTER + 1) {
        fprintf(
            stderr,
            "expected one growth to capacity 512, found capacity %zu size %zu\n",
            index.capacity,
            index.size
        );
        goto done;
    }
    for (i = 0; i < FB_CLUSTER + 1; i++) {
        void *value =
            markdown_core_key_index_lookup(&index, (const unsigned char *)keys[i], (bufsize_t)strlen(keys[i]));
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
    "[^fn]: footnote *body*\n"
    "\n"
    "<!-- comment -->\n"
    "text after <span>html</span>\n";

static const char *FB_SWEEP_EXTENSIONS[] = {"table", "strikethrough", "autolink", "tasklist", "formula", "directive"};

static markdown_core_node *fb_sweep_parse(markdown_core_mem *mem) {
    int options = MARKDOWN_CORE_OPT_DIRECTIVE | MARKDOWN_CORE_OPT_FOOTNOTES | MARKDOWN_CORE_OPT_SMART |
                  MARKDOWN_CORE_OPT_STRIP_HTML_COMMENTS | MARKDOWN_CORE_OPT_DOLLAR_FORMULA_DELIMITERS |
                  MARKDOWN_CORE_OPT_LATEX_FORMULA_DELIMITERS;
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

/* One scripted run: a failed step is retried once (the injector fires at
 * most one failure per run). `stage_dumps[i]` receives the dump after stage
 * i's commit; failed commits are checked against the last committed dump. */
static int fb_session_run(markdown_core_mem *mem, bool pooled, uint8_t **stage_dumps, size_t *stage_lengths) {
    markdown_core_session *session = markdown_core_session_open_with_mem(NULL, mem, pooled, NULL);
    const char *stages[3] = {FB_SESSION_STAGE1, FB_SESSION_STAGE2, FB_SESSION_STAGE3};
    size_t inserts[3] = {0, 0, 0};
    uint8_t *committed_dump = NULL;
    size_t committed_length = 0;
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
            if (!markdown_core_session_commit(session, NULL, NULL)) {
                uint8_t *view = NULL;
                size_t view_length = 0;
                if (markdown_core_session_revision(session) != revision_before) {
                    fputs("failed commit advanced the revision\n", stderr);
                    goto done;
                }
                view = fb_session_dump(session, &view_length);
                if (!view || view_length != committed_length || memcmp(view, committed_dump, view_length) != 0) {
                    fputs("failed commit disturbed the committed view\n", stderr);
                    free(view);
                    goto done;
                }
                free(view);
                if (!markdown_core_session_commit(session, NULL, NULL)) {
                    fputs("commit retry failed\n", stderr);
                    goto done;
                }
            }
            if (markdown_core_session_revision(session) != revision_before + 1) {
                fputs("commit did not advance the revision by one\n", stderr);
                goto done;
            }
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

static int fb_session_sweep(bool pooled) {
    uint8_t *control_dumps[3] = {NULL, NULL, NULL};
    size_t control_lengths[3] = {0, 0, 0};
    uint8_t *counted_dumps[3] = {NULL, NULL, NULL};
    size_t counted_lengths[3] = {0, 0, 0};
    unsigned long total;
    unsigned long k;
    int result = -1;

    if (fb_session_run(markdown_core_mem_default(), pooled, control_dumps, control_lengths) != 0) {
        fputs("control session run failed\n", stderr);
        goto done;
    }

    fb_sweep_count = 0;
    fb_sweep_fail_at = 0;
    if (fb_session_run(&fb_sweep_mem, pooled, counted_dumps, counted_lengths) != 0 ||
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
        run = fb_session_run(&fb_sweep_mem, pooled, final_dumps, final_lengths);
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
    return result;
}

static int case_session_oom_sweep(void) { return fb_session_sweep(false); }

/* The pooled sweep drives the same script through a session arena over the
 * injected allocator, so every base refill — slab, passthrough block, the
 * arena itself — fails in turn; transactionality and retry convergence must
 * hold exactly as they do against direct allocation. */
static int case_session_oom_sweep_pooled(void) { return fb_session_sweep(true); }

/* One same-length in-place edit for a restart-locality scenario. */
typedef struct {
    size_t lo;
    size_t hi;
    const char *insert;
} rl_edit;

/* Opens a session over `initial`, applies each edit as its own commit, and
 * pins the whole inventory: exactly two full commits (the empty open and the
 * whole-text insert, which routes to the full path — a head restart with no
 * boundary beyond the damage), and every edit an incremental, reflowed
 * restart. */
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
    if (session->full_commits != 2 || session->restarted_commits != edit_count ||
        session->reflowed_commits != edit_count) {
        fprintf(
            stderr,
            "FAILED: restart_locality_counters: %s: expected 2 full / %zu restarted / %zu reflowed, "
            "got %zu / %zu / %zu\n",
            name,
            edit_count,
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

/* Head-of-document definition clusters must restart and reflow at sentinel
 * clean entries: retargeting the last definition of a leading cluster is an
 * incremental, reflowed commit, never a full reparse. The counters are the
 * session's restart-locality inventory. */
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
    /* Opening a session commits the empty document through the full path,
     * and the whole-text insert routes there too: a head restart with no
     * clean boundary at or beyond the damage reparses everything, and the
     * full path does that with wholesale rebuilds. */
    if (session->full_commits != 2 || session->restarted_commits != 0) {
        fprintf(stderr, "FAILED: restart_locality_counters: whole-text insert did not route to the full path\n");
        goto done;
    }
    /* Retarget the last head definition: [c]'s destination at bytes 24..27. */
    if (!markdown_core_session_edit(session, 24, 27, (const uint8_t *)retarget, sizeof(retarget) - 1, NULL) ||
        !markdown_core_session_commit(session, NULL, NULL)) {
        fprintf(stderr, "FAILED: restart_locality_counters: retarget commit failed\n");
        goto done;
    }
    if (session->full_commits != 2) {
        fprintf(stderr, "FAILED: restart_locality_counters: a head-cluster edit fell back to a full reparse\n");
        goto done;
    }
    if (session->restarted_commits != 1 || session->reflowed_commits != 1) {
        fprintf(
            stderr,
            "FAILED: restart_locality_counters: expected a reflowed restart (restarted %zu, reflowed %zu)\n",
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
    {"map_prepare_oom", case_map_prepare_oom},
    {"constructor_oom", case_constructor_oom},
    {"oom_sweep", case_oom_sweep},
    {"session_oom_sweep", case_session_oom_sweep},
    {"session_oom_sweep_pooled", case_session_oom_sweep_pooled},
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
