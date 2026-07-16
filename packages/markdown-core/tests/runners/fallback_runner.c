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

/* Injected allocator.  Only the targeted shapes fail: key-index slot tables
 * are calloc(capacity >= 16, sizeof(slot)) and the sorted-fallback pointer
 * arrays are calloc(count >= 2, sizeof(pointer)); every other allocation in
 * the engine uses calloc(1, size) or byte-sized elements. */
static size_t fb_blocked_allocations;
static int fb_block_slot_tables;
static int fb_block_pointer_arrays;
static int fb_block_all_callocs;

static void *fb_calloc(size_t nmemb, size_t size) {
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

static void *fb_realloc(void *pointer, size_t size) { return realloc(pointer, size); }

static void fb_free(void *pointer) { free(pointer); }

static markdown_core_mem fb_failing_mem = {fb_calloc, fb_realloc, fb_free};

/* Sweep allocator: counts allocations, or fails exactly the k-th one
 * (calloc and realloc share the counter). */
static unsigned long fb_sweep_count;
static unsigned long fb_sweep_fail_at; /* 0 = count only */
static int fb_sweep_fired;

static void *fb_sweep_calloc(size_t nmemb, size_t size) {
    if (++fb_sweep_count == fb_sweep_fail_at) {
        fb_sweep_fired = 1;
        return NULL;
    }
    return calloc(nmemb, size);
}

static void *fb_sweep_realloc(void *pointer, size_t size) {
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
        fprintf(stderr, "%s: label '%s' resolved to '%.*s', expected '%s'\n", context, label, (int)ref->url.len,
                ref->url.data, url);
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
    markdown_core_map *hash_map = markdown_core_reference_map_new(markdown_core_get_default_mem_allocator());
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
    markdown_core_extension *extension = markdown_core_find_extension("directive");
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

    control = fb_parse_directive_attributes(input, markdown_core_get_default_mem_allocator());
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
    if (!markdown_core_key_index_init(&index, markdown_core_get_default_mem_allocator(), 128)) {
        fputs("index initialization failed\n", stderr);
        return -1;
    }
    if (index.capacity != 256) {
        fprintf(stderr, "unexpected initial capacity %zu\n", index.capacity);
        goto done;
    }
    for (i = 0; i < FB_CLUSTER; i++) {
        if (!markdown_core_key_index_insert(&index, (const unsigned char *)keys[i], (bufsize_t)strlen(keys[i]),
                                            (void *)(uintptr_t)(i + 1), 0, NULL)) {
            fprintf(stderr, "cluster insert %zu failed\n", i);
            goto done;
        }
    }
    if (index.capacity != 256 || index.size != FB_CLUSTER) {
        fputs("cluster did not fill the table as constructed; retune with core/map.c hash\n", stderr);
        goto done;
    }
    if (!markdown_core_key_index_insert(&index, (const unsigned char *)keys[FB_CLUSTER],
                                        (bufsize_t)strlen(keys[FB_CLUSTER]), (void *)(uintptr_t)(FB_CLUSTER + 1), 0,
                                        NULL)) {
        fputs("probe-exhausted insert failed instead of growing\n", stderr);
        goto done;
    }
    if (index.capacity != 512 || index.size != FB_CLUSTER + 1) {
        fprintf(stderr, "expected one growth to capacity 512, found capacity %zu size %zu\n", index.capacity,
                index.size);
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
        markdown_core_extension *extension = markdown_core_find_extension(FB_SWEEP_EXTENSIONS[i]);
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
        fprintf(stderr, "  %s: type=%d literal='%.*s' (len %d)\n", side, (int)markdown_core_node_get_type(node),
                (int)(node->as.literal.len < 40 ? node->as.literal.len : 40), (const char *)node->as.literal.data,
                (int)node->as.literal.len);
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

    control = fb_sweep_parse(markdown_core_get_default_mem_allocator());
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

typedef struct fb_case_entry {
    const char *name;
    int (*run)(void);
} fb_case_entry;

static const fb_case_entry FB_CASES[] = {
    {"reference_sorted_fallback", case_reference_sorted_fallback},
    {"directive_sorted_fallback", case_directive_sorted_fallback},
    {"key_index_probe_growth", case_key_index_probe_growth},
    {"map_prepare_oom", case_map_prepare_oom},
    {"constructor_oom", case_constructor_oom},
    {"oom_sweep", case_oom_sweep},
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
