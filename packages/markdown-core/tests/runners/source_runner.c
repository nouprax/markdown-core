/* Acceptance gates for the persistent stored-byte substrate (M2).
 *
 * Contract clauses under test, from incremental-canonical-ast.md:
 *   14.3.1  arbitrary legal stored-byte edits stay byte- and parse-equal to
 *           an independently maintained oracle          (random_edits)
 *   14.3.4  an edit path-copies O(log n) tree nodes, and predecessors stay
 *           readable unchanged                          (storage_sharing)
 *   14.3.5  long append traces never copy the retained prefix; tiny
 *           retained slices respect the declared amplification bound
 *                                (append_no_prefix_copy, amplification)
 *   14.3.6  the asymmetric STRICT_UTF8 acceptance boundary, all four rows,
 *           including the U+FFFD decode of a truncated final code point
 *                                                       (strict_boundary)
 *   14.3.7  splitting one character across two commits: the intermediate
 *           document is legal, final output equals a fresh parse, and the
 *           intermediate stays readable after its successor is gone
 *                                                       (split_character)
 *   14.8.2-3 randomized chunk partitions, including boundaries inside a
 *           multi-byte character, commit successfully under both profiles,
 *           with and without the completing chunk, and every final document
 *           parses identically to a fresh parse         (chunk_partition)
 *
 * Two locality rules have no numbered clause of their own and are gated
 * here because prose without a failing check is not a rule (delivery plan,
 * "the rule every milestone is held to"): STRICT_UTF8 validation examines
 * only edit-proportional byte counts (validation_locality), and allocation
 * failure at every allocation site publishes nothing and leaks nothing
 * (oom_sweep).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <markdown_core.h>

#include "source.h"

/* Standalone like fallback_runner: linked against the static archives so
 * the internal source symbols resolve, without markdown-core-test-support
 * (which binds to the shared facade under MARKDOWN_CORE_SHARED). The few
 * helpers it would have provided are inlined below. */

/* xorshift64*, the same generator test_support uses, for reproducibility. */
typedef struct sr_prng {
    uint64_t state;
} sr_prng;

static void sr_prng_seed(sr_prng *prng, uint64_t seed) { prng->state = seed ? seed : 0x9E3779B97F4A7C15ULL; }

static uint64_t sr_prng_next(sr_prng *prng) {
    uint64_t x = prng->state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    prng->state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* Appends `unit` to a malloc'd buffer `count` times, NUL-terminated. */
static char *sr_repeat(const char *unit, size_t count, size_t *length) {
    size_t unit_length = strlen(unit);
    char *buffer = (char *)malloc(unit_length * count + 1);
    size_t i;
    if (!buffer) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        memcpy(buffer + i * unit_length, unit, unit_length);
    }
    buffer[unit_length * count] = 0;
    *length = unit_length * count;
    return buffer;
}

/* Portable memmem for the three-byte U+FFFD probe. */
static int sr_contains(const uint8_t *haystack, size_t haystack_length, const char *needle, size_t needle_length) {
    size_t i;
    for (i = 0; i + needle_length <= haystack_length; i++) {
        if (memcmp(haystack + i, needle, needle_length) == 0) {
            return 1;
        }
    }
    return 0;
}

static markdown_core_document *sr_parse(const uint8_t *bytes, size_t length) {
    markdown_core_error *error = NULL;
    markdown_core_document *document = markdown_core_document_parse(bytes, length, NULL, &error);
    if (!document) {
        markdown_core_string_view message = markdown_core_error_get_message(error);
        fprintf(stderr, "parse failed: %.*s\n", (int)message.length, message.data);
        markdown_core_error_free(error);
    }
    return document;
}

/* --- counting/failing allocator ---------------------------------------- */

typedef struct counting_mem {
    markdown_core_mem mem; /* must stay first: allocator calls cast back */
    size_t live;           /* outstanding blocks */
    size_t attempts;       /* allocation attempts observed */
    size_t fail_at;        /* 1-based attempt to fail; 0 = never */
} counting_mem;

static void *counting_calloc(markdown_core_mem *mem, size_t nmem, size_t size) {
    counting_mem *counting = (counting_mem *)mem;
    void *block;
    counting->attempts++;
    if (counting->fail_at != 0 && counting->attempts == counting->fail_at) {
        return NULL;
    }
    block = calloc(nmem, size);
    if (block) {
        counting->live++;
    }
    return block;
}

static void *counting_realloc(markdown_core_mem *mem, void *ptr, size_t size) {
    counting_mem *counting = (counting_mem *)mem;
    void *block;
    counting->attempts++;
    if (counting->fail_at != 0 && counting->attempts == counting->fail_at) {
        return NULL;
    }
    block = realloc(ptr, size);
    if (block && !ptr) {
        counting->live++;
    }
    return block;
}

static void counting_free(markdown_core_mem *mem, void *ptr) {
    counting_mem *counting = (counting_mem *)mem;
    if (ptr) {
        counting->live--;
    }
    free(ptr);
}

static void counting_init(counting_mem *counting) {
    counting->mem.calloc = counting_calloc;
    counting->mem.realloc = counting_realloc;
    counting->mem.free = counting_free;
    counting->live = 0;
    counting->attempts = 0;
    counting->fail_at = 0;
}

/* --- helpers ------------------------------------------------------------ */

static size_t ceil_log2(size_t value) {
    size_t bits = 0;
    size_t probe = 1;
    while (probe < value) {
        probe *= 2;
        bits++;
    }
    return bits;
}

static uint8_t *materialize(const markdown_core_source *source, size_t *length) {
    size_t size = markdown_core_source_length(source);
    uint8_t *bytes = (uint8_t *)malloc(size + 1);
    if (!bytes) {
        return NULL;
    }
    markdown_core_source_copy_bytes(source, 0, size, bytes);
    bytes[size] = 0;
    *length = size;
    return bytes;
}

/* Asserts the source's bytes equal `expected` exactly. */
static int expect_bytes(const char *label, const markdown_core_source *source, const uint8_t *expected, size_t length) {
    size_t actual_length = 0;
    uint8_t *actual = materialize(source, &actual_length);
    if (!actual) {
        fprintf(stderr, "%s: materialize failed\n", label);
        return -1;
    }
    if (actual_length != length || (length > 0 && memcmp(actual, expected, length) != 0)) {
        fprintf(stderr, "%s: stored bytes diverge (%zu vs %zu bytes)\n", label, actual_length, length);
        free(actual);
        return -1;
    }
    free(actual);
    return 0;
}

/* Parses both buffers and requires byte-identical canonical dumps. */
static int expect_same_parse(
    const char *label,
    const uint8_t *left,
    size_t left_length,
    const uint8_t *right,
    size_t right_length
) {
    markdown_core_document *left_document = sr_parse(left, left_length);
    markdown_core_document *right_document;
    uint8_t *left_dump = NULL;
    uint8_t *right_dump = NULL;
    size_t left_dump_length = 0;
    size_t right_dump_length = 0;
    int result = -1;
    if (!left_document) {
        fprintf(stderr, "%s: left parse failed\n", label);
        return -1;
    }
    right_document = sr_parse(right, right_length);
    if (!right_document) {
        fprintf(stderr, "%s: right parse failed\n", label);
        markdown_core_document_free(left_document);
        return -1;
    }
    if (markdown_core_document_dump(left_document, &left_dump, &left_dump_length, NULL) &&
        markdown_core_document_dump(right_document, &right_dump, &right_dump_length, NULL)) {
        if (left_dump_length == right_dump_length && memcmp(left_dump, right_dump, left_dump_length) == 0) {
            result = 0;
        } else {
            size_t at = 0;
            while (at < left_dump_length && at < right_dump_length && left_dump[at] == right_dump[at]) {
                at++;
            }
            fprintf(
                stderr,
                "%s: canonical dumps diverge at byte %zu (%zu vs %zu bytes)\n",
                label,
                at,
                left_dump_length,
                right_dump_length
            );
        }
    } else {
        fprintf(stderr, "%s: dump failed\n", label);
    }
    markdown_core_dump_free(left_dump);
    markdown_core_dump_free(right_dump);
    markdown_core_document_free(left_document);
    markdown_core_document_free(right_document);
    return result;
}

/* Applies one edit batch to a malloc'd shadow buffer (the oracle). */
static int oracle_apply(uint8_t **bytes, size_t *length, const markdown_core_source_edit *edits, size_t count) {
    size_t removed = 0;
    size_t inserted = 0;
    size_t i;
    size_t next_length;
    uint8_t *next;
    size_t read = 0;
    size_t write = 0;
    for (i = 0; i < count; i++) {
        removed += edits[i].span.end - edits[i].span.start;
        inserted += edits[i].replacement_length;
    }
    next_length = *length - removed + inserted;
    next = (uint8_t *)malloc(next_length + 1);
    if (!next) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        memcpy(next + write, *bytes + read, edits[i].span.start - read);
        write += edits[i].span.start - read;
        memcpy(next + write, edits[i].replacement, edits[i].replacement_length);
        write += edits[i].replacement_length;
        read = edits[i].span.end;
    }
    memcpy(next + write, *bytes + read, *length - read);
    write += *length - read;
    next[write] = 0;
    free(*bytes);
    *bytes = next;
    *length = write;
    return 0;
}

/* --- 14.3.4: storage sharing and path-copy bound ------------------------ */

static int case_storage_sharing(void) {
    counting_mem counting;
    markdown_core_source_stats stats = {0, 0, 0, 0};
    markdown_core_source_status status = MARKDOWN_CORE_SOURCE_OK;
    size_t size = 512 * 1024;
    uint8_t *text = (uint8_t *)malloc(size);
    uint8_t *before;
    size_t before_length = 0;
    markdown_core_source *base;
    markdown_core_source *edited;
    markdown_core_source_edit edit;
    size_t node_bound;
    size_t i;
    int failed = 0;
    if (!text) {
        return -1;
    }
    counting_init(&counting);
    for (i = 0; i < size; i++) {
        text[i] = (uint8_t)('a' + (i % 26));
    }
    base = markdown_core_source_new(&counting.mem, MARKDOWN_CORE_SOURCE_STRICT_UTF8, text, size, &stats, &status);
    if (!base) {
        fprintf(stderr, "storage_sharing: base creation failed\n");
        free(text);
        return -1;
    }
    before = materialize(base, &before_length);
    if (!before) {
        markdown_core_source_release(base);
        free(text);
        return -1;
    }

    /* One small replacement in the middle: the path-copy bound. */
    memset(&stats, 0, sizeof(stats));
    edit.span.start = size / 2;
    edit.span.end = size / 2 + 3;
    edit.replacement = (const uint8_t *)"XYZ";
    edit.replacement_length = 3;
    edited = markdown_core_source_apply(base, &edit, 1, &stats, &status);
    if (!edited) {
        fprintf(stderr, "storage_sharing: apply failed (%d)\n", (int)status);
        failed = 1;
    } else {
        node_bound = 8 * ceil_log2(size) + 16;
        if (stats.nodes_created > node_bound) {
            fprintf(
                stderr,
                "storage_sharing: %zu nodes created, bound %zu (14.3.4)\n",
                stats.nodes_created,
                node_bound
            );
            failed = 1;
        }
        /* Path copying is copying paths, not content: a mid-document edit
         * may copy the replacement plus boundary-leaf slack, never the
         * document (14.3.4). */
        if (stats.bytes_copied > edit.replacement_length + 2 * MARKDOWN_CORE_SOURCE_LEAF_MERGE_LIMIT) {
            fprintf(stderr, "storage_sharing: %zu bytes copied for a 3-byte edit (14.3.4)\n", stats.bytes_copied);
            failed = 1;
        }
        /* The predecessor is untouched: same bytes as before the edit. */
        if (expect_bytes("storage_sharing (old)", base, before, before_length) != 0) {
            failed = 1;
        }
        /* The successor carries the edit. */
        memcpy(text + size / 2, "XYZ", 3);
        if (expect_bytes("storage_sharing (new)", edited, text, size) != 0) {
            failed = 1;
        }
        /* Old readable after the successor is released, and vice versa. */
        markdown_core_source_release(edited);
        if (expect_bytes("storage_sharing (old after successor freed)", base, before, before_length) != 0) {
            failed = 1;
        }
    }
    markdown_core_source_release(base);
    free(before);
    free(text);
    if (counting.live != 0) {
        fprintf(stderr, "storage_sharing: %zu blocks leaked\n", counting.live);
        failed = 1;
    }
    return failed ? -1 : 0;
}

/* --- 14.3.5: append traces and the amplification bound ------------------ */

static int case_append_no_prefix_copy(void) {
    counting_mem counting;
    markdown_core_source_stats stats = {0, 0, 0, 0};
    markdown_core_source_status status = MARKDOWN_CORE_SOURCE_OK;
    size_t base_size = 512 * 1024;
    uint8_t *text = (uint8_t *)malloc(base_size);
    markdown_core_source *current;
    size_t i;
    int failed = 0;
    if (!text) {
        return -1;
    }
    counting_init(&counting);
    for (i = 0; i < base_size; i++) {
        text[i] = (uint8_t)('a' + (i % 26));
    }
    current =
        markdown_core_source_new(&counting.mem, MARKDOWN_CORE_SOURCE_STRICT_UTF8, text, base_size, &stats, &status);
    free(text);
    if (!current) {
        return -1;
    }

    /* Chunked appends: per commit, copying is bounded by the chunk plus the
     * boundary-leaf merge limit — the half-megabyte prefix never moves. */
    for (i = 0; i < 200 && !failed; i++) {
        uint8_t chunk[1024];
        markdown_core_source_edit edit;
        markdown_core_source *next;
        size_t length = markdown_core_source_length(current);
        memset(chunk, 'x', sizeof(chunk));
        edit.span.start = length;
        edit.span.end = length;
        edit.replacement = chunk;
        edit.replacement_length = sizeof(chunk);
        memset(&stats, 0, sizeof(stats));
        next = markdown_core_source_apply(current, &edit, 1, &stats, &status);
        if (!next) {
            fprintf(stderr, "append_no_prefix_copy: chunk append %zu failed\n", i);
            failed = 1;
            break;
        }
        if (stats.bytes_copied > sizeof(chunk) + MARKDOWN_CORE_SOURCE_LEAF_MERGE_LIMIT) {
            fprintf(stderr, "append_no_prefix_copy: append %zu copied %zu bytes (14.3.5)\n", i, stats.bytes_copied);
            failed = 1;
        }
        if (stats.nodes_created > 8 * ceil_log2(length + sizeof(chunk)) + 16) {
            fprintf(stderr, "append_no_prefix_copy: append %zu created %zu nodes\n", i, stats.nodes_created);
            failed = 1;
        }
        markdown_core_source_release(current);
        current = next;
    }

    /* Byte-wise appends: the merge limit bounds the copy, not the prefix. */
    for (i = 0; i < 300 && !failed; i++) {
        markdown_core_source_edit edit;
        markdown_core_source *next;
        size_t length = markdown_core_source_length(current);
        edit.span.start = length;
        edit.span.end = length;
        edit.replacement = (const uint8_t *)".";
        edit.replacement_length = 1;
        memset(&stats, 0, sizeof(stats));
        next = markdown_core_source_apply(current, &edit, 1, &stats, &status);
        if (!next) {
            failed = 1;
            break;
        }
        if (stats.bytes_copied > 1 + MARKDOWN_CORE_SOURCE_LEAF_MERGE_LIMIT) {
            fprintf(
                stderr,
                "append_no_prefix_copy: byte append %zu copied %zu bytes (14.3.5)\n",
                i,
                stats.bytes_copied
            );
            failed = 1;
        }
        markdown_core_source_release(current);
        current = next;
    }
    markdown_core_source_release(current);
    if (counting.live != 0) {
        fprintf(stderr, "append_no_prefix_copy: %zu blocks leaked\n", counting.live);
        failed = 1;
    }
    return failed ? -1 : 0;
}

static int case_amplification_bound(void) {
    counting_mem counting;
    markdown_core_source_stats stats = {0, 0, 0, 0};
    markdown_core_source_status status = MARKDOWN_CORE_SOURCE_OK;
    size_t size = 1024 * 1024;
    uint8_t *text = (uint8_t *)malloc(size);
    size_t offsets[] = {0, 1, 4096, 512 * 1024, 1024 * 1024 - 40};
    size_t i;
    int failed = 0;
    if (!text) {
        return -1;
    }
    counting_init(&counting);
    for (i = 0; i < size; i++) {
        text[i] = (uint8_t)('a' + (i % 26));
    }
    for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]) && !failed; i++) {
        /* Delete everything except a tiny slice at offsets[i]. */
        markdown_core_source *base =
            markdown_core_source_new(&counting.mem, MARKDOWN_CORE_SOURCE_STRICT_UTF8, text, size, &stats, &status);
        markdown_core_source *tiny;
        markdown_core_source_edit edits[2];
        size_t keep = 32;
        size_t retained = 0;
        if (!base) {
            failed = 1;
            break;
        }
        edits[0].span.start = 0;
        edits[0].span.end = offsets[i];
        edits[0].replacement = NULL;
        edits[0].replacement_length = 0;
        edits[1].span.start = offsets[i] + keep;
        edits[1].span.end = size;
        edits[1].replacement = NULL;
        edits[1].replacement_length = 0;
        tiny = markdown_core_source_apply(base, edits, 2, &stats, &status);
        markdown_core_source_release(base);
        if (!tiny) {
            fprintf(stderr, "amplification: slice at %zu failed\n", offsets[i]);
            failed = 1;
            break;
        }
        if (!markdown_core_source_retained_bytes(tiny, &counting.mem, &retained)) {
            failed = 1;
        } else if (retained > MARKDOWN_CORE_SOURCE_MAX_AMPLIFICATION * markdown_core_source_length(tiny)) {
            fprintf(
                stderr,
                "amplification: %zu retained for %zu logical bytes at %zu (14.3.5)\n",
                retained,
                markdown_core_source_length(tiny),
                offsets[i]
            );
            failed = 1;
        }
        if (expect_bytes("amplification", tiny, text + offsets[i], keep) != 0) {
            failed = 1;
        }
        markdown_core_source_release(tiny);
    }

    /* Deleting a narrow middle span leaves two windows of one buffer: both
     * share it, the accounting counts its capacity once, and the bound
     * still holds. */
    if (!failed) {
        markdown_core_source *base =
            markdown_core_source_new(&counting.mem, MARKDOWN_CORE_SOURCE_STRICT_UTF8, text, size, &stats, &status);
        markdown_core_source *gapped;
        markdown_core_source_edit edit;
        size_t retained = 0;
        edit.span.start = size / 2;
        edit.span.end = size / 2 + 64;
        edit.replacement = NULL;
        edit.replacement_length = 0;
        memset(&stats, 0, sizeof(stats));
        gapped = markdown_core_source_apply(base, &edit, 1, &stats, &status);
        markdown_core_source_release(base);
        if (!gapped) {
            failed = 1;
        } else {
            if (stats.bytes_copied > MARKDOWN_CORE_SOURCE_LEAF_MERGE_LIMIT) {
                fprintf(
                    stderr,
                    "amplification: middle delete copied %zu bytes instead of sharing\n",
                    stats.bytes_copied
                );
                failed = 1;
            }
            if (!markdown_core_source_retained_bytes(gapped, &counting.mem, &retained)) {
                failed = 1;
            } else if (retained > MARKDOWN_CORE_SOURCE_MAX_AMPLIFICATION * markdown_core_source_length(gapped)) {
                fprintf(stderr, "amplification: middle delete retains %zu bytes (14.3.5)\n", retained);
                failed = 1;
            } else if (retained != size) {
                fprintf(stderr, "amplification: shared buffer counted twice (%zu, expected %zu)\n", retained, size);
                failed = 1;
            }
            markdown_core_source_release(gapped);
        }
    }

    /* A half split must share, not copy: both sides keep the one buffer. */
    if (!failed) {
        markdown_core_source *base =
            markdown_core_source_new(&counting.mem, MARKDOWN_CORE_SOURCE_STRICT_UTF8, text, size, &stats, &status);
        markdown_core_source *half;
        markdown_core_source_edit edit;
        size_t retained = 0;
        edit.span.start = size / 2;
        edit.span.end = size;
        edit.replacement = NULL;
        edit.replacement_length = 0;
        memset(&stats, 0, sizeof(stats));
        half = markdown_core_source_apply(base, &edit, 1, &stats, &status);
        markdown_core_source_release(base);
        if (!half) {
            failed = 1;
        } else {
            if (stats.bytes_copied > MARKDOWN_CORE_SOURCE_LEAF_MERGE_LIMIT) {
                fprintf(stderr, "amplification: half split copied %zu bytes instead of sharing\n", stats.bytes_copied);
                failed = 1;
            }
            if (!markdown_core_source_retained_bytes(half, &counting.mem, &retained) ||
                retained > MARKDOWN_CORE_SOURCE_MAX_AMPLIFICATION * markdown_core_source_length(half)) {
                fprintf(stderr, "amplification: half split retains %zu bytes\n", retained);
                failed = 1;
            }
            markdown_core_source_release(half);
        }
    }
    free(text);
    if (counting.live != 0) {
        fprintf(stderr, "amplification: %zu blocks leaked\n", counting.live);
        failed = 1;
    }
    return failed ? -1 : 0;
}

/* --- 14.3.6: the asymmetric STRICT_UTF8 boundary ------------------------ */

static int case_strict_boundary(void) {
    counting_mem counting;
    markdown_core_source_stats stats = {0, 0, 0, 0};
    markdown_core_source_status status = MARKDOWN_CORE_SOURCE_OK;
    static const uint8_t base_text[] = "paragraph one\n";
    static const uint8_t truncated_tail[] = {0xF0, 0x9F, 0x92}; /* U+1F496 minus its last byte */
    static const uint8_t invalid_complete[] = {0xC0, 0xAF};     /* overlong '/': always invalid */
    static const uint8_t truncated_then_more[] = {0xC3, 0x78};  /* lead, then ASCII */
    markdown_core_source *base;
    markdown_core_source *published;
    markdown_core_source_edit edit;
    int failed = 0;
    size_t row;
    counting_init(&counting);

    base = markdown_core_source_new(
        &counting.mem,
        MARKDOWN_CORE_SOURCE_STRICT_UTF8,
        base_text,
        sizeof(base_text) - 1,
        &stats,
        &status
    );
    if (!base) {
        return -1;
    }

    /* Row 1: a truncated final code point commits, decodes to U+FFFD, and
     * needs no further commit to stay readable. */
    edit.span.start = sizeof(base_text) - 1;
    edit.span.end = sizeof(base_text) - 1;
    edit.replacement = truncated_tail;
    edit.replacement_length = sizeof(truncated_tail);
    published = markdown_core_source_apply(base, &edit, 1, &stats, &status);
    if (!published || status != MARKDOWN_CORE_SOURCE_OK) {
        fprintf(stderr, "strict_boundary: truncated final code point rejected (14.3.6 row 1)\n");
        failed = 1;
    } else {
        size_t published_length = 0;
        uint8_t *bytes = materialize(published, &published_length);
        markdown_core_document *document = bytes ? sr_parse(bytes, published_length) : NULL;
        if (!document) {
            fprintf(stderr, "strict_boundary: truncated-tail document failed to parse (14.3.6 row 1)\n");
            failed = 1;
        } else {
            /* The canonical dump prints text content verbatim, so the
             * replacement character shows up in it byte-for-byte. */
            uint8_t *dump = NULL;
            size_t dump_length = 0;
            if (!markdown_core_document_dump(document, &dump, &dump_length, NULL) || dump_length == 0 ||
                !sr_contains(dump, dump_length, "\xEF\xBF\xBD", 3)) {
                fprintf(stderr, "strict_boundary: truncated tail did not decode to U+FFFD (14.3.6 row 1)\n");
                failed = 1;
            }
            markdown_core_dump_free(dump);
            markdown_core_document_free(document);
        }
        free(bytes);
        markdown_core_source_release(published);
    }

    /* Rows 2 and 3: a complete invalid sequence, and a truncated code point
     * with bytes after it, both fail publishing nothing — the base keeps
     * its exact bytes. Probed at the tail and mid-document. */
    for (row = 0; row < 4 && !failed; row++) {
        const uint8_t *payload = (row < 2) ? invalid_complete : truncated_then_more;
        size_t payload_length = (row < 2) ? sizeof(invalid_complete) : sizeof(truncated_then_more);
        size_t at = (row % 2 == 0) ? sizeof(base_text) - 1 : 4;
        edit.span.start = at;
        edit.span.end = at;
        edit.replacement = payload;
        edit.replacement_length = payload_length;
        published = markdown_core_source_apply(base, &edit, 1, &stats, &status);
        if (published || status != MARKDOWN_CORE_SOURCE_INVALID_UTF8) {
            fprintf(stderr, "strict_boundary: invalid sequence accepted (14.3.6 rows 2-3, probe %zu)\n", row);
            failed = 1;
            if (published) {
                markdown_core_source_release(published);
            }
        }
        if (expect_bytes("strict_boundary (nothing published)", base, base_text, sizeof(base_text) - 1) != 0) {
            failed = 1;
        }
    }

    /* The positional exception, spelled as two commits: a truncated tail
     * commit succeeds, and appending a non-continuation after it fails. */
    if (!failed) {
        static const uint8_t lead[] = {0xC3};
        markdown_core_source *dangling;
        edit.span.start = sizeof(base_text) - 1;
        edit.span.end = sizeof(base_text) - 1;
        edit.replacement = lead;
        edit.replacement_length = 1;
        dangling = markdown_core_source_apply(base, &edit, 1, &stats, &status);
        if (!dangling) {
            fprintf(stderr, "strict_boundary: dangling lead rejected at end-of-source\n");
            failed = 1;
        } else {
            markdown_core_source *bad;
            edit.span.start = markdown_core_source_length(dangling);
            edit.span.end = edit.span.start;
            edit.replacement = (const uint8_t *)"x";
            edit.replacement_length = 1;
            bad = markdown_core_source_apply(dangling, &edit, 1, &stats, &status);
            if (bad || status != MARKDOWN_CORE_SOURCE_INVALID_UTF8) {
                fprintf(stderr, "strict_boundary: byte after truncated code point accepted (14.3.6 row 3)\n");
                failed = 1;
                if (bad) {
                    markdown_core_source_release(bad);
                }
            }
            markdown_core_source_release(dangling);
        }
    }

    /* A replacement that destroys a character's lead strands its
     * continuation bytes after the edit: the retained suffix no longer
     * begins at a character boundary and the commit must reject, even
     * though the replacement itself ends cleanly. The mirror rows, where
     * the replacement supplies a compatible lead, must still commit. */
    if (!failed) {
        static const uint8_t two_byte_doc[] = {0x78, 0xC2, 0x80, 0x79};  /* x U+0080 y */
        static const uint8_t four_byte_doc[] = {0xF0, 0x9F, 0x92, 0x96}; /* U+1F496 */
        static const struct {
            const uint8_t *doc;
            size_t doc_length;
            size_t start;
            size_t end;
            const char *replacement;
            size_t replacement_length;
            int accepted;
        } stranded[] = {
            {two_byte_doc, 4, 1, 2, "a", 1, 0},     /* lead replaced by ASCII */
            {two_byte_doc, 4, 1, 2, "\xC3", 1, 1},  /* lead replaced by lead */
            {four_byte_doc, 4, 0, 2, "ab", 2, 0},   /* two of four replaced by ASCII */
            {four_byte_doc, 4, 0, 1, "\xF1", 1, 1}, /* four-byte lead swapped */
            {two_byte_doc, 4, 1, 2, "", 0, 0},      /* lead deleted outright */
        };
        for (row = 0; row < sizeof(stranded) / sizeof(stranded[0]) && !failed; row++) {
            markdown_core_source *doc = markdown_core_source_new(
                &counting.mem,
                MARKDOWN_CORE_SOURCE_STRICT_UTF8,
                stranded[row].doc,
                stranded[row].doc_length,
                &stats,
                &status
            );
            if (!doc) {
                failed = 1;
                break;
            }
            edit.span.start = stranded[row].start;
            edit.span.end = stranded[row].end;
            edit.replacement = (const uint8_t *)stranded[row].replacement;
            edit.replacement_length = stranded[row].replacement_length;
            published = markdown_core_source_apply(doc, &edit, 1, &stats, &status);
            if (stranded[row].accepted && !published) {
                fprintf(stderr, "strict_boundary: compatible lead swap %zu rejected (%d)\n", row, (int)status);
                failed = 1;
            }
            if (!stranded[row].accepted && (published || status != MARKDOWN_CORE_SOURCE_INVALID_UTF8)) {
                fprintf(stderr, "strict_boundary: stranded continuation published (row %zu, 14.3.6)\n", row);
                failed = 1;
            }
            if (published) {
                markdown_core_source_release(published);
            }
            if (expect_bytes(
                    "strict_boundary (stranded, nothing published)",
                    doc,
                    stranded[row].doc,
                    stranded[row].doc_length
                ) != 0) {
                failed = 1;
            }
            markdown_core_source_release(doc);
        }
        /* The same stranding through the far-apart multi-edit path, so the
         * split-range flush validates its suffix too. */
        if (!failed) {
            uint8_t wide_doc[40];
            markdown_core_source *doc;
            size_t i;
            for (i = 0; i < sizeof(wide_doc); i++) {
                wide_doc[i] = (uint8_t)('0' + (i % 10));
            }
            wide_doc[20] = 0xC2;
            wide_doc[21] = 0x80;
            doc = markdown_core_source_new(
                &counting.mem,
                MARKDOWN_CORE_SOURCE_STRICT_UTF8,
                wide_doc,
                sizeof(wide_doc),
                &stats,
                &status
            );
            if (!doc) {
                failed = 1;
            } else {
                markdown_core_source_edit pair[2];
                pair[0].span.start = 20;
                pair[0].span.end = 21; /* destroys the C2 lead */
                pair[0].replacement = (const uint8_t *)"a";
                pair[0].replacement_length = 1;
                pair[1].span.start = 35;
                pair[1].span.end = 35;
                pair[1].replacement = (const uint8_t *)"ok";
                pair[1].replacement_length = 2;
                published = markdown_core_source_apply(doc, pair, 2, &stats, &status);
                if (published || status != MARKDOWN_CORE_SOURCE_INVALID_UTF8) {
                    fprintf(stderr, "strict_boundary: stranded continuation published via flush path (14.3.6)\n");
                    failed = 1;
                    if (published) {
                        markdown_core_source_release(published);
                    }
                }
                markdown_core_source_release(doc);
            }
        }
    }

    /* Row 4: the same three payloads under PERMISSIVE_BYTES all succeed —
     * that acceptance is what keeps the profiles observably different. */
    if (!failed) {
        markdown_core_source *permissive = markdown_core_source_new(
            &counting.mem,
            MARKDOWN_CORE_SOURCE_PERMISSIVE_BYTES,
            base_text,
            sizeof(base_text) - 1,
            &stats,
            &status
        );
        const uint8_t *payloads[3] = {truncated_tail, invalid_complete, truncated_then_more};
        const size_t payload_lengths[3] =
            {sizeof(truncated_tail), sizeof(invalid_complete), sizeof(truncated_then_more)};
        if (!permissive) {
            failed = 1;
        }
        for (row = 0; row < 3 && !failed; row++) {
            edit.span.start = 4;
            edit.span.end = 4;
            edit.replacement = payloads[row];
            edit.replacement_length = payload_lengths[row];
            published = markdown_core_source_apply(permissive, &edit, 1, &stats, &status);
            if (!published || status != MARKDOWN_CORE_SOURCE_OK) {
                fprintf(stderr, "strict_boundary: PERMISSIVE_BYTES rejected payload %zu (14.3.6 row 4)\n", row);
                failed = 1;
            } else {
                markdown_core_source_release(published);
            }
        }
        if (permissive) {
            markdown_core_source_release(permissive);
        }
    }
    markdown_core_source_release(base);
    if (counting.live != 0) {
        fprintf(stderr, "strict_boundary: %zu blocks leaked\n", counting.live);
        failed = 1;
    }
    return failed ? -1 : 0;
}

/* --- 14.3.7: one character split across two commits --------------------- */

static int case_split_character(void) {
    counting_mem counting;
    markdown_core_source_stats stats = {0, 0, 0, 0};
    markdown_core_source_status status = MARKDOWN_CORE_SOURCE_OK;
    static const struct {
        const char *bytes;
        size_t length;
    } characters[] = {
        {"\xC3\xA9", 2},         /* é */
        {"\xE2\x82\xAC", 3},     /* € */
        {"\xF0\x9F\x92\x96", 4}, /* U+1F496 */
    };
    static const uint8_t prefix_text[] = "before ";
    size_t character;
    int failed = 0;
    counting_init(&counting);

    for (character = 0; character < 3 && !failed; character++) {
        size_t split;
        for (split = 1; split < characters[character].length && !failed; split++) {
            markdown_core_source *base = markdown_core_source_new(
                &counting.mem,
                MARKDOWN_CORE_SOURCE_STRICT_UTF8,
                prefix_text,
                sizeof(prefix_text) - 1,
                &stats,
                &status
            );
            markdown_core_source *intermediate;
            markdown_core_source *final_source;
            markdown_core_source_edit edit;
            uint8_t expected[16];
            size_t expected_length;
            if (!base) {
                failed = 1;
                break;
            }
            /* First chunk: the character's first `split` bytes. */
            edit.span.start = sizeof(prefix_text) - 1;
            edit.span.end = sizeof(prefix_text) - 1;
            edit.replacement = (const uint8_t *)characters[character].bytes;
            edit.replacement_length = split;
            intermediate = markdown_core_source_apply(base, &edit, 1, &stats, &status);
            markdown_core_source_release(base);
            if (!intermediate) {
                fprintf(
                    stderr,
                    "split_character: chunk boundary at %zu/%zu rejected (14.3.7)\n",
                    split,
                    characters[character].length
                );
                failed = 1;
                break;
            }
            /* The intermediate is a legal document: it parses. */
            {
                size_t bytes_length = 0;
                uint8_t *bytes = materialize(intermediate, &bytes_length);
                markdown_core_document *document = bytes ? sr_parse(bytes, bytes_length) : NULL;
                if (!document) {
                    fprintf(stderr, "split_character: intermediate document unreadable (14.3.7)\n");
                    failed = 1;
                } else {
                    markdown_core_document_free(document);
                }
                free(bytes);
            }
            /* Completing chunk: the rest of the character. */
            edit.span.start = markdown_core_source_length(intermediate);
            edit.span.end = edit.span.start;
            edit.replacement = (const uint8_t *)characters[character].bytes + split;
            edit.replacement_length = characters[character].length - split;
            final_source = markdown_core_source_apply(intermediate, &edit, 1, &stats, &status);
            if (!final_source) {
                fprintf(stderr, "split_character: completing chunk rejected (14.3.7)\n");
                failed = 1;
                markdown_core_source_release(intermediate);
                break;
            }
            memcpy(expected, prefix_text, sizeof(prefix_text) - 1);
            memcpy(expected + sizeof(prefix_text) - 1, characters[character].bytes, characters[character].length);
            expected_length = sizeof(prefix_text) - 1 + characters[character].length;
            if (expect_bytes("split_character (final)", final_source, expected, expected_length) != 0) {
                failed = 1;
            }
            /* The final document equals a fresh parse of the final bytes. */
            if (!failed) {
                size_t bytes_length = 0;
                uint8_t *bytes = materialize(final_source, &bytes_length);
                if (!bytes ||
                    expect_same_parse("split_character", bytes, bytes_length, expected, expected_length) != 0) {
                    failed = 1;
                }
                free(bytes);
            }
            /* The intermediate outlives its successor: release the final
             * document first, then read the intermediate again — a session
             * may close at any commit (14.3.7). */
            markdown_core_source_release(final_source);
            memcpy(expected + sizeof(prefix_text) - 1, characters[character].bytes, split);
            if (expect_bytes(
                    "split_character (intermediate after close)",
                    intermediate,
                    expected,
                    sizeof(prefix_text) - 1 + split
                ) != 0) {
                failed = 1;
            }
            markdown_core_source_release(intermediate);
        }
    }
    if (counting.live != 0) {
        fprintf(stderr, "split_character: %zu blocks leaked\n", counting.live);
        failed = 1;
    }
    return failed ? -1 : 0;
}

/* --- tree shapes: adversarial joins, splits, and rotations -------------- */

/* The deterministic shape script shared by tree_shapes and the second
 * oom_sweep trace: appends (left-taller joins), prepends (right-taller
 * joins), then mid-document splices at shifting offsets. Sixty steps of
 * 200-byte blocks reach every join, split, and rotation shape, single and
 * double, on both sides. */
#define SHAPE_STEPS 60

static void shape_edit(
    size_t step,
    size_t length,
    const uint8_t *block,
    size_t block_length,
    markdown_core_source_edit *edit
) {
    size_t at;
    if (step < 21) {
        at = length;
    } else if (step < 42) {
        at = 0;
    } else {
        at = (length / 7) * (step % 7) + step;
    }
    edit->span.start = at;
    edit->span.end = (step % 5 == 4 && length - at >= 64) ? at + 64 : at;
    edit->replacement = block;
    edit->replacement_length = block_length;
}

/* Byte equality against the oracle over edits chosen to force every tree
 * shape the substrate can take: multi-leaf builds, prepends (right-taller
 * joins), appends (left-taller joins), and mid-document splices that cross
 * leaf and subtree boundaries. The path-copy bound of 14.3.4 is asserted on
 * every edit, so a shape that degenerates the tree fails here even though
 * every byte still reads back correctly. */
static int case_tree_shapes(void) {
    counting_mem counting;
    markdown_core_source_stats stats = {0, 0, 0, 0};
    markdown_core_source_status status = MARKDOWN_CORE_SOURCE_OK;
    uint8_t block[200];
    uint8_t *oracle = NULL;
    size_t oracle_length = 0;
    markdown_core_source *current;
    size_t i;
    int failed = 0;
    counting_init(&counting);
    for (i = 0; i < sizeof(block); i++) {
        block[i] = (uint8_t)('a' + (i % 26));
    }
    current = markdown_core_source_new(
        &counting.mem,
        MARKDOWN_CORE_SOURCE_STRICT_UTF8,
        block,
        sizeof(block),
        &stats,
        &status
    );
    if (!current) {
        return -1;
    }
    oracle = (uint8_t *)malloc(sizeof(block) + 1);
    if (!oracle) {
        markdown_core_source_release(current);
        return -1;
    }
    memcpy(oracle, block, sizeof(block));
    oracle_length = sizeof(block);

    /* Blocks larger than the merge limit never coalesce, so every step
     * inserts a distinct leaf. */
    for (i = 0; i < SHAPE_STEPS && !failed; i++) {
        markdown_core_source_edit edit;
        markdown_core_source *next;
        shape_edit(i, oracle_length, block, sizeof(block), &edit);
        memset(&stats, 0, sizeof(stats));
        next = markdown_core_source_apply(current, &edit, 1, &stats, &status);
        if (!next) {
            fprintf(stderr, "tree_shapes: edit %zu failed (%d)\n", i, (int)status);
            failed = 1;
            break;
        }
        if (stats.nodes_created > 8 * ceil_log2(oracle_length + sizeof(block)) + 16) {
            fprintf(stderr, "tree_shapes: edit %zu created %zu nodes (14.3.4)\n", i, stats.nodes_created);
            failed = 1;
        }
        if (oracle_apply(&oracle, &oracle_length, &edit, 1) != 0) {
            markdown_core_source_release(next);
            failed = 1;
            break;
        }
        markdown_core_source_release(current);
        current = next;
        if (expect_bytes("tree_shapes", current, oracle, oracle_length) != 0) {
            fprintf(stderr, "tree_shapes: divergence after edit %zu\n", i);
            failed = 1;
        }
    }
    markdown_core_source_release(current);
    free(oracle);
    if (counting.live != 0) {
        fprintf(stderr, "tree_shapes: %zu blocks leaked\n", counting.live);
        failed = 1;
    }
    return failed ? -1 : 0;
}

/* --- the frozen UTF-8 acceptance matrix --------------------------------- */

/* One row per boundary of RFC 3629: every lead class, every constrained
 * continuation range (overlongs, surrogates, above U+10FFFF), truncation at
 * end-of-source, and the same bytes accepted under PERMISSIVE_BYTES. This
 * pins the decode profile at construction time the way strict_boundary pins
 * it at edit time. */
static int case_utf8_matrix(void) {
    static const struct {
        const char *bytes;
        size_t length;
        int accepted; /* under STRICT_UTF8 */
    } rows[] = {
        {"\x7F", 1, 1},             /* highest ASCII */
        {"\xC2\x80", 2, 1},         /* lowest two-byte */
        {"\xDF\xBF", 2, 1},         /* highest two-byte */
        {"\xC0\xAF", 2, 0},         /* overlong two-byte lead */
        {"\xC1\x80", 2, 0},         /* overlong two-byte lead */
        {"\xC2\x41", 2, 0},         /* continuation out of range (low) */
        {"\xC2\xC0", 2, 0},         /* continuation out of range (high) */
        {"\xE0\xA0\x80", 3, 1},     /* lowest legal E0 */
        {"\xE0\x9F\xBF", 3, 0},     /* E0 overlong */
        {"\xED\x9F\xBF", 3, 1},     /* highest before surrogates */
        {"\xED\xA0\x80", 3, 0},     /* surrogate */
        {"\xE7\x88\x88", 3, 1},     /* ordinary three-byte */
        {"\xF0\x90\x80\x80", 4, 1}, /* lowest legal F0 */
        {"\xF0\x8F\xBF\xBF", 4, 0}, /* F0 overlong */
        {"\xF4\x8F\xBF\xBF", 4, 1}, /* U+10FFFF */
        {"\xF4\x90\x80\x80", 4, 0}, /* above U+10FFFF */
        {"\xF5\x80\x80\x80", 4, 0}, /* lead out of range */
        {"\xFF", 1, 0},             /* invalid everywhere */
        {"\x80", 1, 0},             /* bare continuation */
        {"\xE2\x82", 2, 1},         /* truncated final, two of three */
        {"\xF0\x9F\x92", 3, 1},     /* truncated final, three of four */
        {"a\xC3", 2, 1},            /* truncated after ASCII */
        {"a\xC3 b", 4, 0},          /* truncated then more bytes */
        {"long ascii run with \xC3\xA9 and \xE2\x82\xAC and \xF0\x9F\x92\x96 inside", 46, 1},
    };
    counting_mem counting;
    markdown_core_source_stats stats = {0, 0, 0, 0};
    markdown_core_source_status status = MARKDOWN_CORE_SOURCE_OK;
    size_t row;
    int failed = 0;
    counting_init(&counting);

    for (row = 0; row < sizeof(rows) / sizeof(rows[0]) && !failed; row++) {
        markdown_core_source *strict = markdown_core_source_new(
            &counting.mem,
            MARKDOWN_CORE_SOURCE_STRICT_UTF8,
            (const uint8_t *)rows[row].bytes,
            rows[row].length,
            &stats,
            &status
        );
        markdown_core_source *permissive;
        if (rows[row].accepted && !strict) {
            fprintf(stderr, "utf8_matrix: row %zu rejected (%d)\n", row, (int)status);
            failed = 1;
        }
        if (!rows[row].accepted && (strict || status != MARKDOWN_CORE_SOURCE_INVALID_UTF8)) {
            fprintf(stderr, "utf8_matrix: row %zu accepted\n", row);
            failed = 1;
        }
        if (strict) {
            if (expect_bytes("utf8_matrix", strict, (const uint8_t *)rows[row].bytes, rows[row].length) != 0) {
                failed = 1;
            }
            markdown_core_source_release(strict);
        }
        /* Every row stores under PERMISSIVE_BYTES: acceptance there is what
         * keeps the two profiles observably different (7.1). */
        permissive = markdown_core_source_new(
            &counting.mem,
            MARKDOWN_CORE_SOURCE_PERMISSIVE_BYTES,
            (const uint8_t *)rows[row].bytes,
            rows[row].length,
            &stats,
            &status
        );
        if (!permissive) {
            fprintf(stderr, "utf8_matrix: row %zu rejected under PERMISSIVE_BYTES\n", row);
            failed = 1;
        } else {
            markdown_core_source_release(permissive);
        }
    }

    /* A batch whose edits sit far apart validates as separate ranges; an
     * invalid early range must reject even though the last range is clean. */
    if (!failed) {
        static const uint8_t clean[] = "0123456789012345678901234567890123456789";
        markdown_core_source *base = markdown_core_source_new(
            &counting.mem,
            MARKDOWN_CORE_SOURCE_STRICT_UTF8,
            clean,
            sizeof(clean) - 1,
            &stats,
            &status
        );
        markdown_core_source *result;
        markdown_core_source_edit edits[2];
        if (!base) {
            failed = 1;
        } else {
            edits[0].span.start = 2;
            edits[0].span.end = 2;
            edits[0].replacement = (const uint8_t *)"\xFF";
            edits[0].replacement_length = 1;
            edits[1].span.start = 30;
            edits[1].span.end = 30;
            edits[1].replacement = (const uint8_t *)"ok";
            edits[1].replacement_length = 2;
            result = markdown_core_source_apply(base, edits, 2, &stats, &status);
            if (result || status != MARKDOWN_CORE_SOURCE_INVALID_UTF8) {
                fprintf(stderr, "utf8_matrix: invalid early range accepted\n");
                failed = 1;
                if (result) {
                    markdown_core_source_release(result);
                }
            }
            /* And the mirror image: both far-apart ranges clean commits. */
            edits[0].replacement = (const uint8_t *)"\xC3\xA9";
            edits[0].replacement_length = 2;
            result = markdown_core_source_apply(base, edits, 2, &stats, &status);
            if (!result) {
                fprintf(stderr, "utf8_matrix: clean far-apart batch rejected (%d)\n", (int)status);
                failed = 1;
            } else {
                markdown_core_source_release(result);
            }
            markdown_core_source_release(base);
        }
    }
    if (counting.live != 0) {
        fprintf(stderr, "utf8_matrix: %zu blocks leaked\n", counting.live);
        failed = 1;
    }
    return failed ? -1 : 0;
}

/* --- span validation ---------------------------------------------------- */

/* Wider than any plausible buffer header, so that a replacement length can sit
 * strictly between "the header can be added to it" and "the retained bytes can
 * be added to it" — the window in which only the resulting-length check
 * refuses the edit. */
#define SPAN_WIDE_SOURCE 4096

static int case_span_validation(void) {
    counting_mem counting;
    markdown_core_source_stats stats = {0, 0, 0, 0};
    markdown_core_source_status status = MARKDOWN_CORE_SOURCE_OK;
    static const uint8_t text[] = "0123456789";
    markdown_core_source *base;
    markdown_core_source *result;
    markdown_core_source_edit edits[2];
    int failed = 0;
    counting_init(&counting);
    base = markdown_core_source_new(
        &counting.mem,
        MARKDOWN_CORE_SOURCE_STRICT_UTF8,
        text,
        sizeof(text) - 1,
        &stats,
        &status
    );
    if (!base) {
        return -1;
    }

    /* end < start */
    edits[0].span.start = 5;
    edits[0].span.end = 3;
    edits[0].replacement = NULL;
    edits[0].replacement_length = 0;
    result = markdown_core_source_apply(base, edits, 1, &stats, &status);
    if (result || status != MARKDOWN_CORE_SOURCE_INVALID_SPAN) {
        fprintf(stderr, "span_validation: inverted span accepted\n");
        failed = 1;
    }

    /* end past the source */
    edits[0].span.start = 5;
    edits[0].span.end = sizeof(text);
    result = markdown_core_source_apply(base, edits, 1, &stats, &status);
    if (result || status != MARKDOWN_CORE_SOURCE_INVALID_SPAN) {
        fprintf(stderr, "span_validation: overflowing span accepted\n");
        failed = 1;
    }

    /* A replacement length that no allocation could have produced. The span is
     * valid, so this is refused for the resulting length rather than the
     * range, and refused before a byte of the (impossible) buffer is read. The
     * session hands this number straight through from public API, which is why
     * the guard is reachable at all. */
    edits[0].span.start = 1;
    edits[0].span.end = 1;
    edits[0].replacement = text;
    edits[0].replacement_length = SIZE_MAX;
    result = markdown_core_source_apply(base, edits, 1, &stats, &status);
    if (result || status != MARKDOWN_CORE_SOURCE_NO_MEMORY) {
        fprintf(stderr, "span_validation: unrepresentable resulting length accepted\n");
        failed = 1;
    }

    /* The same absurd length where the *result* is the replacement alone: the
     * edit deletes the whole document, so nothing is retained and a check
     * phrased only against the resulting length has nothing to overflow. The
     * replacement still becomes one buffer sized as a header plus a payload,
     * and that sum is what wraps — leaving a byte-sized allocation to receive
     * a SIZE_MAX memcpy. Whole-document replacement and the empty-source case
     * below are the two ways to reach it. */
    edits[0].span.start = 0;
    edits[0].span.end = sizeof(text) - 1;
    edits[0].replacement = text;
    edits[0].replacement_length = SIZE_MAX;
    result = markdown_core_source_apply(base, edits, 1, &stats, &status);
    if (result || status != MARKDOWN_CORE_SOURCE_NO_MEMORY) {
        fprintf(stderr, "span_validation: whole-document replacement of unrepresentable length accepted\n");
        failed = 1;
    }

    /* Between the two: a length the buffer header can be added to, but not to
     * the bytes this edit keeps. Only the resulting-length arm refuses it, so
     * the two checks are each other's complement rather than one subsuming
     * the other. It needs a source wider than the buffer header for the gap
     * between the two thresholds to exist at all. */
    {
        uint8_t wide[SPAN_WIDE_SOURCE];
        markdown_core_source *wide_base;
        size_t i;
        for (i = 0; i < sizeof(wide); i++) {
            wide[i] = (uint8_t)('a' + (i % 26));
        }
        wide_base = markdown_core_source_new(
            &counting.mem,
            MARKDOWN_CORE_SOURCE_STRICT_UTF8,
            wide,
            sizeof(wide),
            &stats,
            &status
        );
        if (!wide_base) {
            markdown_core_source_release(base);
            return -1;
        }
        edits[0].span.start = 1;
        edits[0].span.end = 1;
        edits[0].replacement = wide;
        edits[0].replacement_length = SIZE_MAX - (SPAN_WIDE_SOURCE - 1);
        result = markdown_core_source_apply(wide_base, edits, 1, &stats, &status);
        if (result || status != MARKDOWN_CORE_SOURCE_NO_MEMORY) {
            fprintf(stderr, "span_validation: unrepresentable resulting length accepted (header-representable)\n");
            failed = 1;
        }
        markdown_core_source_release(wide_base);
    }

    edits[0].replacement = NULL;
    edits[0].replacement_length = 0;

    /* overlapping pair */
    edits[0].span.start = 2;
    edits[0].span.end = 6;
    edits[1].span.start = 5;
    edits[1].span.end = 8;
    edits[1].replacement = NULL;
    edits[1].replacement_length = 0;
    result = markdown_core_source_apply(base, edits, 2, &stats, &status);
    if (result || status != MARKDOWN_CORE_SOURCE_INVALID_SPAN) {
        fprintf(stderr, "span_validation: overlapping edits accepted\n");
        failed = 1;
    }

    /* out of order */
    edits[0].span.start = 6;
    edits[0].span.end = 7;
    edits[1].span.start = 2;
    edits[1].span.end = 3;
    result = markdown_core_source_apply(base, edits, 2, &stats, &status);
    if (result || status != MARKDOWN_CORE_SOURCE_INVALID_SPAN) {
        fprintf(stderr, "span_validation: unsorted edits accepted\n");
        failed = 1;
    }

    /* Nothing was published by any failure. */
    if (expect_bytes("span_validation", base, text, sizeof(text) - 1) != 0) {
        failed = 1;
    }

    /* The empty source is the same hole reached without deleting anything, and
     * it is the state every session opens in — so this is one public
     * markdown_core_session_edit away, with no edit history in front of it. */
    {
        markdown_core_source *empty =
            markdown_core_source_new(&counting.mem, MARKDOWN_CORE_SOURCE_PERMISSIVE_BYTES, NULL, 0, &stats, &status);
        if (!empty) {
            markdown_core_source_release(base);
            return -1;
        }
        edits[0].span.start = 0;
        edits[0].span.end = 0;
        edits[0].replacement = text;
        edits[0].replacement_length = SIZE_MAX;
        result = markdown_core_source_apply(empty, edits, 1, &stats, &status);
        if (result || status != MARKDOWN_CORE_SOURCE_NO_MEMORY) {
            fprintf(stderr, "span_validation: unrepresentable insertion into an empty source accepted\n");
            failed = 1;
        }
        markdown_core_source_release(empty);
        edits[0].replacement = NULL;
        edits[0].replacement_length = 0;
    }

    /* Adjacent spans are legal and deterministic. */
    edits[0].span.start = 2;
    edits[0].span.end = 4;
    edits[0].replacement = (const uint8_t *)"AB";
    edits[0].replacement_length = 2;
    edits[1].span.start = 4;
    edits[1].span.end = 4;
    edits[1].replacement = (const uint8_t *)"CD";
    edits[1].replacement_length = 2;
    result = markdown_core_source_apply(base, edits, 2, &stats, &status);
    if (!result) {
        fprintf(stderr, "span_validation: adjacent edits rejected\n");
        failed = 1;
    } else {
        if (expect_bytes("span_validation (adjacent)", result, (const uint8_t *)"01ABCD456789", 12) != 0) {
            failed = 1;
        }
        markdown_core_source_release(result);
    }

    /* Zero edits publish an identical successor. */
    result = markdown_core_source_apply(base, NULL, 0, &stats, &status);
    if (!result || expect_bytes("span_validation (no-op)", result, text, sizeof(text) - 1) != 0) {
        failed = 1;
    }
    if (result) {
        markdown_core_source_release(result);
    }

    /* Empty source accepts an insertion at offset zero. */
    {
        markdown_core_source *empty =
            markdown_core_source_new(&counting.mem, MARKDOWN_CORE_SOURCE_STRICT_UTF8, NULL, 0, &stats, &status);
        if (!empty || markdown_core_source_length(empty) != 0 ||
            markdown_core_source_profile_of(empty) != MARKDOWN_CORE_SOURCE_STRICT_UTF8) {
            failed = 1;
        } else {
            markdown_core_source *seeded;
            edits[0].span.start = 0;
            edits[0].span.end = 0;
            edits[0].replacement = (const uint8_t *)"hi";
            edits[0].replacement_length = 2;
            seeded = markdown_core_source_apply(empty, edits, 1, &stats, &status);
            if (!seeded || expect_bytes("span_validation (empty)", seeded, (const uint8_t *)"hi", 2) != 0) {
                failed = 1;
            }
            if (seeded) {
                markdown_core_source_release(seeded);
            }
            /* Materializing an empty source is a no-op, not a crash, and
             * it retains no buffer bytes at all. */
            markdown_core_source_copy_bytes(empty, 0, 0, NULL);
            {
                size_t retained = 1;
                if (!markdown_core_source_retained_bytes(empty, &counting.mem, &retained) || retained != 0) {
                    fprintf(stderr, "span_validation: empty source retains %zu bytes\n", retained);
                    failed = 1;
                }
            }
        }
        if (empty) {
            markdown_core_source_release(empty);
        }
    }

    /* Deleting everything is a legal STRICT edit publishing an empty
     * document. */
    {
        markdown_core_source_edit wipe;
        markdown_core_source *emptied;
        wipe.span.start = 0;
        wipe.span.end = sizeof(text) - 1;
        wipe.replacement = NULL;
        wipe.replacement_length = 0;
        emptied = markdown_core_source_apply(base, &wipe, 1, &stats, &status);
        if (!emptied || markdown_core_source_length(emptied) != 0) {
            fprintf(stderr, "span_validation: delete-all failed\n");
            failed = 1;
        }
        if (emptied) {
            markdown_core_source_release(emptied);
        }
    }

    /* Retain/release: a second reference keeps the source alive. */
    markdown_core_source_retain(base);
    markdown_core_source_release(base);
    if (expect_bytes("span_validation (retained)", base, text, sizeof(text) - 1) != 0) {
        failed = 1;
    }
    markdown_core_source_release(base);
    if (counting.live != 0) {
        fprintf(stderr, "span_validation: %zu blocks leaked\n", counting.live);
        failed = 1;
    }
    return failed ? -1 : 0;
}

/* --- validation locality ------------------------------------------------ */

static int case_validation_locality(void) {
    counting_mem counting;
    markdown_core_source_stats stats = {0, 0, 0, 0};
    markdown_core_source_status status = MARKDOWN_CORE_SOURCE_OK;
    size_t size = 8 * 1024 * 1024;
    uint8_t *text = (uint8_t *)malloc(size);
    markdown_core_source *base;
    markdown_core_source *edited;
    markdown_core_source_edit edit;
    size_t i;
    int failed = 0;
    if (!text) {
        return -1;
    }
    counting_init(&counting);
    for (i = 0; i < size; i++) {
        text[i] = (uint8_t)('a' + (i % 26));
    }
    base = markdown_core_source_new(&counting.mem, MARKDOWN_CORE_SOURCE_STRICT_UTF8, text, size, &stats, &status);
    free(text);
    if (!base) {
        return -1;
    }
    /* One replaced byte in an 8 MiB document: STRICT validation may examine
     * the edit and a bounded neighbourhood, never the document. */
    memset(&stats, 0, sizeof(stats));
    edit.span.start = size / 2;
    edit.span.end = size / 2 + 1;
    edit.replacement = (const uint8_t *)"Q";
    edit.replacement_length = 1;
    edited = markdown_core_source_apply(base, &edit, 1, &stats, &status);
    if (!edited) {
        failed = 1;
    } else {
        if (stats.validated_bytes > 16) {
            fprintf(stderr, "validation_locality: %zu bytes validated for a 1-byte edit\n", stats.validated_bytes);
            failed = 1;
        }
        markdown_core_source_release(edited);
    }
    markdown_core_source_release(base);
    if (counting.live != 0) {
        fprintf(stderr, "validation_locality: %zu blocks leaked\n", counting.live);
        failed = 1;
    }
    return failed ? -1 : 0;
}

/* --- allocation-failure sweep ------------------------------------------- */

static int case_oom_sweep(void) {
    static const uint8_t text[] = "alpha βήτα 💖 *gamma* [delta](#) \n\n> quote\n";
    counting_mem counting;
    size_t fail_at;
    int completed = 0;
    counting_init(&counting);

    for (fail_at = 1; !completed; fail_at++) {
        markdown_core_source_stats stats = {0, 0, 0, 0};
        markdown_core_source_status status = MARKDOWN_CORE_SOURCE_OK;
        markdown_core_source *base;
        markdown_core_source *next;
        markdown_core_source_edit edits[2];
        size_t retained = 0;
        counting.attempts = 0;
        counting.fail_at = fail_at;

        /* The trace: create, edit twice in one batch, append, account. */
        base = markdown_core_source_new(
            &counting.mem,
            MARKDOWN_CORE_SOURCE_STRICT_UTF8,
            text,
            sizeof(text) - 1,
            &stats,
            &status
        );
        if (!base) {
            if (status != MARKDOWN_CORE_SOURCE_NO_MEMORY) {
                fprintf(stderr, "oom_sweep: constructor failure %zu misreported (%d)\n", fail_at, (int)status);
                return -1;
            }
            goto swept;
        }
        edits[0].span.start = 0;
        edits[0].span.end = 5;
        edits[0].replacement = (const uint8_t *)"OMEGA";
        edits[0].replacement_length = 5;
        edits[1].span.start = 10;
        edits[1].span.end = 12;
        edits[1].replacement = (const uint8_t *)"é";
        edits[1].replacement_length = 2;
        next = markdown_core_source_apply(base, edits, 2, &stats, &status);
        if (!next) {
            if (status != MARKDOWN_CORE_SOURCE_NO_MEMORY) {
                fprintf(stderr, "oom_sweep: apply failure %zu misreported (%d)\n", fail_at, (int)status);
                markdown_core_source_release(base);
                return -1;
            }
            /* Failure published nothing: the base still reads back whole. */
            if (expect_bytes("oom_sweep", base, text, sizeof(text) - 1) != 0) {
                markdown_core_source_release(base);
                return -1;
            }
            markdown_core_source_release(base);
            goto swept;
        }
        markdown_core_source_release(base);
        if (!markdown_core_source_retained_bytes(next, &counting.mem, &retained)) {
            markdown_core_source_release(next);
            goto swept;
        }
        markdown_core_source_release(next);
        completed = 1;

    swept:
        if (counting.live != 0) {
            fprintf(stderr, "oom_sweep: failure at allocation %zu leaked %zu blocks\n", fail_at, counting.live);
            return -1;
        }
    }

    /* An empty source whose one allocation fails: nothing to release but
     * the failure still reports NO_MEMORY. */
    {
        markdown_core_source_stats stats = {0, 0, 0, 0};
        markdown_core_source_status status = MARKDOWN_CORE_SOURCE_OK;
        markdown_core_source *empty;
        counting.attempts = 0;
        counting.fail_at = 1;
        empty = markdown_core_source_new(&counting.mem, MARKDOWN_CORE_SOURCE_STRICT_UTF8, NULL, 0, &stats, &status);
        if (empty || status != MARKDOWN_CORE_SOURCE_NO_MEMORY || counting.live != 0) {
            fprintf(stderr, "oom_sweep: empty-source constructor failure mishandled\n");
            if (empty) {
                markdown_core_source_release(empty);
            }
            return -1;
        }
    }

    /* Second sweep, over a trace that exercises the tree machinery: a
     * multi-leaf build (blocks above the merge limit), a prepend, a
     * mid-document splice, and the retained-bytes walk. Failure landing
     * inside a descend, a rotation, or a slice crossing must publish
     * nothing and leak nothing, exactly like failure anywhere else. */
    completed = 0;
    for (fail_at = 1; !completed; fail_at++) {
        markdown_core_source_stats stats = {0, 0, 0, 0};
        markdown_core_source_status status = MARKDOWN_CORE_SOURCE_OK;
        uint8_t block[200]; /* the exact tree_shapes trajectory */
        markdown_core_source *current;
        size_t step;
        size_t retained = 0;
        int swept_early = 0;
        size_t i;
        for (i = 0; i < sizeof(block); i++) {
            block[i] = (uint8_t)('a' + (i % 26));
        }
        counting.attempts = 0;
        counting.fail_at = fail_at;
        current = markdown_core_source_new(
            &counting.mem,
            MARKDOWN_CORE_SOURCE_STRICT_UTF8,
            block,
            sizeof(block),
            &stats,
            &status
        );
        if (!current) {
            if (status != MARKDOWN_CORE_SOURCE_NO_MEMORY) {
                fprintf(stderr, "oom_sweep(tree): constructor failure %zu misreported (%d)\n", fail_at, (int)status);
                return -1;
            }
            goto tree_swept;
        }
        for (step = 0; step < SHAPE_STEPS; step++) {
            markdown_core_source_edit edit;
            markdown_core_source *next;
            size_t length = markdown_core_source_length(current);
            size_t expected_length;
            /* The same shape script as tree_shapes, so an allocation
             * failure gets its turn inside every rotation flavour too. */
            shape_edit(step, length, block, sizeof(block), &edit);
            expected_length = length + sizeof(block) - (edit.span.end - edit.span.start);
            next = markdown_core_source_apply(current, &edit, 1, &stats, &status);
            if (!next) {
                if (status != MARKDOWN_CORE_SOURCE_NO_MEMORY) {
                    fprintf(stderr, "oom_sweep(tree): apply failure %zu misreported (%d)\n", fail_at, (int)status);
                    markdown_core_source_release(current);
                    return -1;
                }
                /* Nothing published: the predecessor still reads whole. */
                if (markdown_core_source_length(current) != length) {
                    fprintf(stderr, "oom_sweep(tree): predecessor changed length\n");
                    markdown_core_source_release(current);
                    return -1;
                }
                markdown_core_source_release(current);
                swept_early = 1;
                break;
            }
            if (markdown_core_source_length(next) != expected_length) {
                fprintf(stderr, "oom_sweep(tree): wrong length after step %zu\n", step);
                markdown_core_source_release(current);
                markdown_core_source_release(next);
                return -1;
            }
            markdown_core_source_release(current);
            current = next;
        }
        if (swept_early) {
            goto tree_swept;
        }
        if (!markdown_core_source_retained_bytes(current, &counting.mem, &retained)) {
            markdown_core_source_release(current);
            goto tree_swept;
        }
        markdown_core_source_release(current);
        completed = 1;

    tree_swept:
        if (counting.live != 0) {
            fprintf(stderr, "oom_sweep(tree): failure at allocation %zu leaked %zu blocks\n", fail_at, counting.live);
            return -1;
        }
    }
    return 0;
}

/* --- 14.3.1: randomized legal edits against an oracle ------------------- */

/* Steps back to the nearest UTF-8 boundary in the oracle. */
static size_t align_to_boundary(const uint8_t *bytes, size_t offset) {
    while (offset > 0 && bytes[offset] >= 0x80 && bytes[offset] <= 0xBF) {
        offset--;
    }
    return offset;
}

static int case_random_edits(void) {
    static const char *snippets[] = {"", "a", "text ", "é", "€", "💖", "*em*\n", "> q\n", "χ αβ"};
    counting_mem counting;
    markdown_core_source_stats stats = {0, 0, 0, 0};
    markdown_core_source_status status = MARKDOWN_CORE_SOURCE_OK;
    int profile_index;
    int failed = 0;
    counting_init(&counting);

    for (profile_index = 0; profile_index < 2 && !failed; profile_index++) {
        markdown_core_source_profile profile =
            profile_index == 0 ? MARKDOWN_CORE_SOURCE_STRICT_UTF8 : MARKDOWN_CORE_SOURCE_PERMISSIVE_BYTES;
        sr_prng prng;
        uint8_t *oracle = (uint8_t *)malloc(1);
        size_t oracle_length = 0;
        markdown_core_source *current;
        size_t round;
        if (!oracle) {
            return -1;
        }
        oracle[0] = 0;
        sr_prng_seed(&prng, 0x5EEDF00D + (uint64_t)profile_index);
        current = markdown_core_source_new(&counting.mem, profile, NULL, 0, &stats, &status);
        if (!current) {
            free(oracle);
            return -1;
        }
        for (round = 0; round < 240 && !failed; round++) {
            markdown_core_source_edit edits[3];
            uint8_t random_bytes[3][12];
            size_t count = 1 + (size_t)(sr_prng_next(&prng) % 3);
            size_t cursor = 0;
            size_t i;
            markdown_core_source *next;
            for (i = 0; i < count; i++) {
                size_t start = cursor + (size_t)(sr_prng_next(&prng) % (oracle_length - cursor + 1));
                size_t end = start + (size_t)(sr_prng_next(&prng) % (oracle_length - start + 1));
                if (profile == MARKDOWN_CORE_SOURCE_STRICT_UTF8) {
                    /* Legal edits only: spans on character boundaries and
                     * valid replacement text (14.3.1 covers legal edits;
                     * the boundary rows live in strict_boundary). */
                    const char *snippet;
                    start = align_to_boundary(oracle, start);
                    if (start < cursor) {
                        start = cursor;
                    }
                    end = align_to_boundary(oracle, end);
                    if (end < start) {
                        end = start;
                    }
                    snippet = snippets[sr_prng_next(&prng) % (sizeof(snippets) / sizeof(snippets[0]))];
                    edits[i].replacement = (const uint8_t *)snippet;
                    edits[i].replacement_length = strlen(snippet);
                } else {
                    size_t byte_count = (size_t)(sr_prng_next(&prng) % sizeof(random_bytes[i]));
                    size_t b;
                    for (b = 0; b < byte_count; b++) {
                        random_bytes[i][b] = (uint8_t)(sr_prng_next(&prng) & 0xFF);
                    }
                    edits[i].replacement = random_bytes[i];
                    edits[i].replacement_length = byte_count;
                }
                edits[i].span.start = start;
                edits[i].span.end = end;
                cursor = end;
            }
            next = markdown_core_source_apply(current, edits, count, &stats, &status);
            if (!next) {
                fprintf(stderr, "random_edits: legal batch rejected in round %zu (%d, 14.3.1)\n", round, (int)status);
                failed = 1;
                break;
            }
            if (oracle_apply(&oracle, &oracle_length, edits, count) != 0) {
                markdown_core_source_release(next);
                failed = 1;
                break;
            }
            markdown_core_source_release(current);
            current = next;
            if (expect_bytes("random_edits", current, oracle, oracle_length) != 0) {
                fprintf(stderr, "random_edits: divergence in round %zu, profile %d (14.3.1)\n", round, profile_index);
                failed = 1;
            }
        }
        /* The committed bytes parse exactly as an independent buffer of the
         * same bytes does: the decode profile owes nothing to history. */
        if (!failed && profile == MARKDOWN_CORE_SOURCE_STRICT_UTF8) {
            size_t bytes_length = 0;
            uint8_t *bytes = materialize(current, &bytes_length);
            if (!bytes || expect_same_parse("random_edits", bytes, bytes_length, oracle, oracle_length) != 0) {
                failed = 1;
            }
            free(bytes);
        }
        markdown_core_source_release(current);
        free(oracle);
    }
    if (counting.live != 0) {
        fprintf(stderr, "random_edits: %zu blocks leaked\n", counting.live);
        failed = 1;
    }
    return failed ? -1 : 0;
}

/* --- 14.8.2-3: randomized chunk partitions ------------------------------ */

static int case_chunk_partition(void) {
    static const char unit[] = "Héllo 💖 wörld €—βγ *em* [l](#u)\n\n> quß\n";
    counting_mem counting;
    markdown_core_source_stats stats = {0, 0, 0, 0};
    markdown_core_source_status status = MARKDOWN_CORE_SOURCE_OK;
    size_t text_length = 0;
    char *text = sr_repeat(unit, 40, &text_length);
    uint64_t seed;
    int failed = 0;
    if (!text) {
        return -1;
    }
    counting_init(&counting);

    for (seed = 1; seed <= 8 && !failed; seed++) {
        int profile_index;
        for (profile_index = 0; profile_index < 2 && !failed; profile_index++) {
            markdown_core_source_profile profile =
                profile_index == 0 ? MARKDOWN_CORE_SOURCE_STRICT_UTF8 : MARKDOWN_CORE_SOURCE_PERMISSIVE_BYTES;
            sr_prng prng;
            markdown_core_source *current = markdown_core_source_new(&counting.mem, profile, NULL, 0, &stats, &status);
            size_t offset = 0;
            int stopped_short = 0;
            if (!current) {
                failed = 1;
                break;
            }
            sr_prng_seed(&prng, seed * 0x9E3779B97F4A7C15ULL + (uint64_t)profile_index);
            while (offset < text_length && !failed) {
                size_t chunk = 1 + (size_t)(sr_prng_next(&prng) % 7);
                markdown_core_source_edit edit;
                markdown_core_source *next;
                if (chunk > text_length - offset) {
                    chunk = text_length - offset;
                }
                /* A stream may simply stop mid-character (14.8.2): abandon
                 * the trace at a deterministic point on some seeds and keep
                 * the truncated document as the final one. */
                if (seed % 3 == 0 && offset > text_length / 2 && !stopped_short) {
                    stopped_short = 1;
                    break;
                }
                edit.span.start = offset;
                edit.span.end = offset;
                edit.replacement = (const uint8_t *)text + offset;
                edit.replacement_length = chunk;
                next = markdown_core_source_apply(current, &edit, 1, &stats, &status);
                if (!next) {
                    fprintf(
                        stderr,
                        "chunk_partition: chunk at %zu(+%zu) failed, seed %llu profile %d (14.8.2)\n",
                        offset,
                        chunk,
                        (unsigned long long)seed,
                        profile_index
                    );
                    failed = 1;
                    break;
                }
                markdown_core_source_release(current);
                current = next;
                offset += chunk;
                /* Every intermediate document is exactly the prefix bytes. */
                if (expect_bytes("chunk_partition", current, (const uint8_t *)text, offset) != 0) {
                    failed = 1;
                }
            }
            /* The final document — completed or stopped short — parses
             * identically to a fresh parse of exactly those bytes (14.8.3). */
            if (!failed) {
                size_t bytes_length = 0;
                uint8_t *bytes = materialize(current, &bytes_length);
                if (!bytes || bytes_length != offset ||
                    expect_same_parse("chunk_partition", bytes, bytes_length, (const uint8_t *)text, offset) != 0) {
                    failed = 1;
                }
                free(bytes);
            }
            markdown_core_source_release(current);
        }
    }

    /* Deterministic mid-character boundaries: split every multi-byte
     * character of one unit at every interior byte under STRICT_UTF8. */
    if (!failed) {
        size_t position = 0;
        while (position < strlen(unit) && !failed) {
            uint8_t lead = (uint8_t)unit[position];
            size_t width = lead < 0x80 ? 1 : (lead < 0xE0 ? 2 : (lead < 0xF0 ? 3 : 4));
            size_t split;
            for (split = 1; split < width && !failed; split++) {
                markdown_core_source *doc = markdown_core_source_new(
                    &counting.mem,
                    MARKDOWN_CORE_SOURCE_STRICT_UTF8,
                    (const uint8_t *)unit,
                    position + split,
                    &stats,
                    &status
                );
                markdown_core_source *whole;
                markdown_core_source_edit edit;
                if (!doc) {
                    fprintf(
                        stderr,
                        "chunk_partition: mid-character stop at %zu+%zu rejected (14.8.2)\n",
                        position,
                        split
                    );
                    failed = 1;
                    break;
                }
                edit.span.start = position + split;
                edit.span.end = position + split;
                edit.replacement = (const uint8_t *)unit + position + split;
                edit.replacement_length = width - split;
                whole = markdown_core_source_apply(doc, &edit, 1, &stats, &status);
                if (!whole) {
                    fprintf(
                        stderr,
                        "chunk_partition: completing chunk at %zu+%zu rejected (14.8.2)\n",
                        position,
                        split
                    );
                    failed = 1;
                } else {
                    if (expect_bytes("chunk_partition (completed)", whole, (const uint8_t *)unit, position + width) !=
                        0) {
                        failed = 1;
                    }
                    markdown_core_source_release(whole);
                }
                markdown_core_source_release(doc);
            }
            position += width;
        }
    }
    free(text);
    if (counting.live != 0) {
        fprintf(stderr, "chunk_partition: %zu blocks leaked\n", counting.live);
        failed = 1;
    }
    return failed ? -1 : 0;
}

/* --- case table --------------------------------------------------------- */

typedef struct source_case {
    const char *name;
    int (*run)(void);
} source_case;

static const source_case CASES[] = {
    {"span_validation", case_span_validation},
    {"tree_shapes", case_tree_shapes},
    {"utf8_matrix", case_utf8_matrix},
    {"storage_sharing", case_storage_sharing},
    {"append_no_prefix_copy", case_append_no_prefix_copy},
    {"amplification_bound", case_amplification_bound},
    {"strict_boundary", case_strict_boundary},
    {"validation_locality", case_validation_locality},
    {"oom_sweep", case_oom_sweep},
    {"random_edits", case_random_edits},
    {"split_character", case_split_character},
    {"chunk_partition", case_chunk_partition},
};

int main(int argc, char **argv) {
    size_t i;
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        for (i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
            puts(CASES[i].name);
        }
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--case") == 0) {
        for (i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
            if (strcmp(CASES[i].name, argv[2]) == 0) {
                int failed = CASES[i].run(); /* prints its own diagnostics */
                printf("%s %s\n", CASES[i].name, failed ? "[FAILED]" : "[PASSED]");
                return failed ? 1 : 0;
            }
        }
        fprintf(stderr, "unknown case: %s\n", argv[2]);
        return 2;
    }
    fputs("usage: source_runner --list | --case NAME\n", stderr);
    return 2;
}
