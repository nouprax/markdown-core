/* Acceptance gates for the stored bytes (7.1, 8.1).
 *
 * This file used to gate a PERSISTENT substrate: an edit that path-copied
 * O(log n) tree nodes, predecessors that stayed readable, a declared
 * amplification bound, and the asymmetric STRICT_UTF8 acceptance boundary.
 * All four are gone with the clauses they came from (14.3.4, 14.3.5, 14.3.6,
 * and `SourceProfile`; see docs/reviews/2026-08-07-requirement-audit.md), and
 * the store is one growable buffer spliced in place.
 *
 * Contract clauses under test, from incremental-canonical-ast.md:
 *   14.3.1  arbitrary legal stored-byte edits stay byte- and parse-equal to
 *           an independently maintained oracle          (random_edits)
 *   14.3.7  splitting one character across two commits: the intermediate
 *           document is legal and the final output equals a fresh parse
 *                                                       (split_character)
 *   14.8.2-3 randomized chunk partitions, including boundaries inside a
 *           multi-byte character, commit successfully with and without the
 *           completing chunk, and every final document parses identically
 *           to a fresh parse                            (chunk_partition)
 *
 * Three rules have no numbered clause of their own and are gated here
 * because prose without a failing check is not a rule: a span the store
 * cannot represent is refused before a byte moves and publishes nothing
 * (span_validation), the batch splice builds beside the source and so has
 * an empty-result state and a failed-allocation state of its own
 * (batch_splice), and allocation failure at every allocation site publishes
 * nothing and leaks nothing (oom_sweep).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <markdown_core.h>

#include "source.h"
#include "commit_compat.h"

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
    markdown_core_document *document = markdown_core_document_new(bytes, length, NULL, &error);
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

/* --- 14.3.7: one character split across two commits --------------------- */

static int case_split_character(void) {
    counting_mem counting;
    markdown_core_source_stats stats = {0, 0};
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
            markdown_core_source *base =
                markdown_core_source_new(&counting.mem, prefix_text, sizeof(prefix_text) - 1, &stats, &status);
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
            if (!markdown_core_source_apply(base, &edit, 1, &stats, &status)) {
                fprintf(
                    stderr,
                    "split_character: chunk boundary at %zu/%zu rejected (14.3.7)\n",
                    split,
                    characters[character].length
                );
                markdown_core_source_release(base);
                failed = 1;
                break;
            }
            /* The intermediate is a legal document: it parses. */
            {
                size_t bytes_length = 0;
                uint8_t *bytes = materialize(base, &bytes_length);
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
            edit.span.start = markdown_core_source_length(base);
            edit.span.end = edit.span.start;
            edit.replacement = (const uint8_t *)characters[character].bytes + split;
            edit.replacement_length = characters[character].length - split;
            if (!markdown_core_source_apply(base, &edit, 1, &stats, &status)) {
                fprintf(stderr, "split_character: completing chunk rejected (14.3.7)\n");
                markdown_core_source_release(base);
                failed = 1;
                break;
            }
            memcpy(expected, prefix_text, sizeof(prefix_text) - 1);
            memcpy(expected + sizeof(prefix_text) - 1, characters[character].bytes, characters[character].length);
            expected_length = sizeof(prefix_text) - 1 + characters[character].length;
            if (expect_bytes("split_character (final)", base, expected, expected_length) != 0) {
                failed = 1;
            }
            /* The final document equals a fresh parse of the final bytes. */
            if (!failed) {
                size_t bytes_length = 0;
                uint8_t *bytes = materialize(base, &bytes_length);
                if (!bytes ||
                    expect_same_parse("split_character", bytes, bytes_length, expected, expected_length) != 0) {
                    failed = 1;
                }
                free(bytes);
            }
            /* What the intermediate looked like is no longer asserted here.
             * It used to read the predecessor after releasing its successor,
             * on the ground that a session may close at any commit — but a
             * source is spliced in place and there is no predecessor to read.
             * What that row was about, that a boundary inside a character
             * commits and stays readable, is the intermediate parse above. */
            markdown_core_source_release(base);
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

/* --- the frozen UTF-8 acceptance matrix --------------------------------- */

/* --- span validation ---------------------------------------------------- */

/* The batch splice is a separate mechanism from the single-edit one — it
 * builds beside the source instead of in place — and it has two states no
 * other case reaches: a batch whose result is EMPTY, and a batch whose one
 * allocation fails. Both are ordinary inputs, not corner-hunting: a select-all
 * delete arrives as spans covering the document, and every allocation in this
 * engine is allowed to fail. */
static int case_batch_splice(void) {
    static const uint8_t text[] = "alpha beta gamma";
    counting_mem counting;
    markdown_core_source_stats stats;
    markdown_core_source_status status = MARKDOWN_CORE_SOURCE_OK;
    markdown_core_source_edit edits[2];
    markdown_core_source *base;
    int failed = 0;

    counting_init(&counting);
    memset(&stats, 0, sizeof(stats));

    /* Two adjacent deletions covering everything: the result is zero bytes,
     * so the batch has nothing to allocate. */
    base = markdown_core_source_new(&counting.mem, text, sizeof(text) - 1, &stats, &status);
    if (!base) {
        return -1;
    }
    edits[0].span.start = 0;
    edits[0].span.end = 6;
    edits[0].replacement = NULL;
    edits[0].replacement_length = 0;
    edits[1].span.start = 6;
    edits[1].span.end = sizeof(text) - 1;
    edits[1].replacement = NULL;
    edits[1].replacement_length = 0;
    if (!markdown_core_source_apply(base, edits, 2, &stats, &status)) {
        fprintf(stderr, "batch_splice: emptying batch rejected (%d)\n", (int)status);
        failed = 1;
    } else if (markdown_core_source_length(base) != 0) {
        fprintf(
            stderr,
            "batch_splice: emptying batch left %zu bytes\n",
            markdown_core_source_length(base)
        );
        failed = 1;
    }
    markdown_core_source_release(base);

    /* And the same batch with a replacement, under an allocator that refuses
     * the build. Nothing is published: the source still reads as it did. */
    base = markdown_core_source_new(&counting.mem, text, sizeof(text) - 1, &stats, &status);
    if (!base) {
        return -1;
    }
    edits[0].replacement = (const uint8_t *)"A";
    edits[0].replacement_length = 1;
    edits[1].replacement = (const uint8_t *)"B";
    edits[1].replacement_length = 1;
    counting.attempts = 0;
    counting.fail_at = 1;
    if (markdown_core_source_apply(base, edits, 2, &stats, &status) ||
        status != MARKDOWN_CORE_SOURCE_NO_MEMORY) {
        fprintf(stderr, "batch_splice: batch accepted with its allocation refused\n");
        failed = 1;
    }
    counting.fail_at = 0;
    if (expect_bytes("batch_splice", base, text, sizeof(text) - 1) != 0) {
        failed = 1;
    }
    markdown_core_source_release(base);

    if (counting.live != 0) {
        fprintf(stderr, "batch_splice: %zu blocks leaked\n", counting.live);
        failed = 1;
    }
    return failed ? -1 : 0;
}

static int case_span_validation(void) {
    counting_mem counting;
    markdown_core_source_stats stats = {0, 0};
    markdown_core_source_status status = MARKDOWN_CORE_SOURCE_OK;
    static const uint8_t text[] = "0123456789";
    markdown_core_source *base;
    markdown_core_source *result;
    markdown_core_source_edit edits[2];
    int failed = 0;
    counting_init(&counting);
    base = markdown_core_source_new(&counting.mem, text, sizeof(text) - 1, &stats, &status);
    if (!base) {
        return -1;
    }

    /* end < start */
    edits[0].span.start = 5;
    edits[0].span.end = 3;
    edits[0].replacement = NULL;
    edits[0].replacement_length = 0;
    if (markdown_core_source_apply(base, edits, 1, &stats, &status) || status != MARKDOWN_CORE_SOURCE_INVALID_SPAN) {
        fprintf(stderr, "span_validation: inverted span accepted\n");
        failed = 1;
    }

    /* end past the source */
    edits[0].span.start = 5;
    edits[0].span.end = sizeof(text);
    if (markdown_core_source_apply(base, edits, 1, &stats, &status) || status != MARKDOWN_CORE_SOURCE_INVALID_SPAN) {
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
    if (markdown_core_source_apply(base, edits, 1, &stats, &status) || status != MARKDOWN_CORE_SOURCE_NO_MEMORY) {
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
    if (markdown_core_source_apply(base, edits, 1, &stats, &status) || status != MARKDOWN_CORE_SOURCE_NO_MEMORY) {
        fprintf(stderr, "span_validation: whole-document replacement of unrepresentable length accepted\n");
        failed = 1;
    }

    /* A length no allocator can satisfy used to be unpinnable here, because
     * reaching it meant actually asking for something near SIZE_MAX: libc
     * answers NULL and the apply reports NO_MEMORY, but AddressSanitizer
     * aborts the process instead, since allocator_may_return_null defaults off
     * and a request that size is a program defect by its lights. It is pinned
     * now, at the bottom of this case, because the store refuses at PTRDIFF_MAX
     * before an allocator is asked. Ordinary allocation failure is still the
     * OOM sweeps' job, through an allocator that refuses on demand rather than
     * by magnitude. */

    edits[0].replacement = NULL;
    edits[0].replacement_length = 0;

    /* overlapping pair */
    edits[0].span.start = 2;
    edits[0].span.end = 6;
    edits[1].span.start = 5;
    edits[1].span.end = 8;
    edits[1].replacement = NULL;
    edits[1].replacement_length = 0;
    if (markdown_core_source_apply(base, edits, 2, &stats, &status) || status != MARKDOWN_CORE_SOURCE_INVALID_SPAN) {
        fprintf(stderr, "span_validation: overlapping edits accepted\n");
        failed = 1;
    }

    /* out of order */
    edits[0].span.start = 6;
    edits[0].span.end = 7;
    edits[1].span.start = 2;
    edits[1].span.end = 3;
    if (markdown_core_source_apply(base, edits, 2, &stats, &status) || status != MARKDOWN_CORE_SOURCE_INVALID_SPAN) {
        fprintf(stderr, "span_validation: unsorted edits accepted\n");
        failed = 1;
    }

    /* Nothing was published by any failure. */
    if (expect_bytes("span_validation", base, text, sizeof(text) - 1) != 0) {
        failed = 1;
    }

    /* The empty source is the same hole reached without deleting anything, and
     * it is the state every session opens in — so this is one public
     * markdown_core_document_edit away, with no edit history in front of it. */
    {
        markdown_core_source *empty = markdown_core_source_new(&counting.mem, NULL, 0, &stats, &status);
        if (!empty) {
            markdown_core_source_release(base);
            return -1;
        }
        edits[0].span.start = 0;
        edits[0].span.end = 0;
        edits[0].replacement = text;
        edits[0].replacement_length = SIZE_MAX;
        if (markdown_core_source_apply(empty, edits, 1, &stats, &status) || status != MARKDOWN_CORE_SOURCE_NO_MEMORY) {
            fprintf(stderr, "span_validation: unrepresentable insertion into an empty source accepted\n");
            failed = 1;
        }
        markdown_core_source_release(empty);
        edits[0].replacement = NULL;
        edits[0].replacement_length = 0;
    }

    /* A construction whose length is not a C object. The two guards are
     * different: `apply` rejects the SUM before it moves a byte, and this one
     * sits in the store's own reserve, which is what a construction reaches.
     * Both refuse at PTRDIFF_MAX rather than SIZE_MAX, because a size between
     * the two is arithmetically fine and still cannot be allocated. */
    {
        markdown_core_source *absurd = markdown_core_source_new(&counting.mem, text, SIZE_MAX, &stats, &status);
        if (absurd || status != MARKDOWN_CORE_SOURCE_NO_MEMORY) {
            fprintf(stderr, "span_validation: construction of unrepresentable length accepted\n");
            markdown_core_source_release(absurd);
            failed = 1;
        }
    }

    /* Releasing nothing is not an error: every unwind path in the session
     * reaches this with a store it may or may not have built. */
    markdown_core_source_release(NULL);

    /* Adjacent spans are legal and deterministic. A splice is in place, so
     * each row below starts from a source of its own rather than reading a
     * predecessor the row before it left behind. */
    edits[0].span.start = 2;
    edits[0].span.end = 4;
    edits[0].replacement = (const uint8_t *)"AB";
    edits[0].replacement_length = 2;
    edits[1].span.start = 4;
    edits[1].span.end = 4;
    edits[1].replacement = (const uint8_t *)"CD";
    edits[1].replacement_length = 2;
    if (!markdown_core_source_apply(base, edits, 2, &stats, &status)) {
        fprintf(stderr, "span_validation: adjacent edits rejected\n");
        failed = 1;
    } else if (expect_bytes("span_validation (adjacent)", base, (const uint8_t *)"01ABCD456789", 12) != 0) {
        failed = 1;
    }
    markdown_core_source_release(base);

    /* Zero edits change nothing. */
    base = markdown_core_source_new(&counting.mem, text, sizeof(text) - 1, &stats, &status);
    if (!base) {
        return -1;
    }
    if (!markdown_core_source_apply(base, NULL, 0, &stats, &status) ||
        expect_bytes("span_validation (no-op)", base, text, sizeof(text) - 1) != 0) {
        failed = 1;
    }

    /* Deleting everything leaves an empty source. */
    {
        markdown_core_source_edit wipe;
        wipe.span.start = 0;
        wipe.span.end = sizeof(text) - 1;
        wipe.replacement = NULL;
        wipe.replacement_length = 0;
        if (!markdown_core_source_apply(base, &wipe, 1, &stats, &status) ||
            markdown_core_source_length(base) != 0) {
            fprintf(stderr, "span_validation: delete-all failed\n");
            failed = 1;
        }
    }
    markdown_core_source_release(base);

    /* An empty source accepts an insertion at offset zero, and materializing
     * one is a no-op rather than a crash. */
    {
        markdown_core_source *empty = markdown_core_source_new(&counting.mem, NULL, 0, &stats, &status);
        if (!empty || markdown_core_source_length(empty) != 0) {
            failed = 1;
        } else {
            markdown_core_source_copy_bytes(empty, 0, 0, NULL);
            edits[0].span.start = 0;
            edits[0].span.end = 0;
            edits[0].replacement = (const uint8_t *)"hi";
            edits[0].replacement_length = 2;
            if (!markdown_core_source_apply(empty, edits, 1, &stats, &status) ||
                expect_bytes("span_validation (empty)", empty, (const uint8_t *)"hi", 2) != 0) {
                failed = 1;
            }
        }
        markdown_core_source_release(empty);
    }

    if (counting.live != 0) {
        fprintf(stderr, "span_validation: %zu blocks leaked\n", counting.live);
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
        markdown_core_source_stats stats = {0, 0};
        markdown_core_source_status status = MARKDOWN_CORE_SOURCE_OK;
        markdown_core_source *base;
        markdown_core_source_edit edits[2];
        size_t retained = 0;
        counting.attempts = 0;
        counting.fail_at = fail_at;

        /* The trace: create, edit twice in one batch, append, account. */
        base = markdown_core_source_new(&counting.mem, text, sizeof(text) - 1, &stats, &status);
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
        if (!markdown_core_source_apply(base, edits, 2, &stats, &status)) {
            if (status != MARKDOWN_CORE_SOURCE_NO_MEMORY) {
                fprintf(stderr, "oom_sweep: apply failure %zu misreported (%d)\n", fail_at, (int)status);
                markdown_core_source_release(base);
                return -1;
            }
            /* A FAILED APPLY MOVED NO BYTE. This is what "nothing is
             * published on failure" costs once the splice is in place: the
             * spans are validated and the buffer reserved while the source is
             * still untouched, so the only failure left is one that happens
             * before anything is written, and the source still reads back
             * whole. */
            if (expect_bytes("oom_sweep", base, text, sizeof(text) - 1) != 0) {
                markdown_core_source_release(base);
                return -1;
            }
            markdown_core_source_release(base);
            goto swept;
        }
        markdown_core_source_release(base);
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
        markdown_core_source_stats stats = {0, 0};
        markdown_core_source_status status = MARKDOWN_CORE_SOURCE_OK;
        markdown_core_source *empty;
        counting.attempts = 0;
        counting.fail_at = 1;
        empty = markdown_core_source_new(&counting.mem, NULL, 0, &stats, &status);
        if (empty || status != MARKDOWN_CORE_SOURCE_NO_MEMORY || counting.live != 0) {
            fprintf(stderr, "oom_sweep: empty-source constructor failure mishandled\n");
            if (empty) {
                markdown_core_source_release(empty);
            }
            return -1;
        }
    }

    /* Second sweep, over the longer trace: appends, a prepend, and
     * mid-document splices at shifting offsets, so a failed growth gets its
     * turn at every shape. Failure must move no byte and leak nothing,
     * exactly like failure anywhere else. */
    completed = 0;
    for (fail_at = 1; !completed; fail_at++) {
        markdown_core_source_stats stats = {0, 0};
        markdown_core_source_status status = MARKDOWN_CORE_SOURCE_OK;
        uint8_t block[200]; /* the exact tree_shapes trajectory */
        markdown_core_source *current;
        size_t step;
        int swept_early = 0;
        size_t i;
        for (i = 0; i < sizeof(block); i++) {
            block[i] = (uint8_t)('a' + (i % 26));
        }
        counting.attempts = 0;
        counting.fail_at = fail_at;
        current = markdown_core_source_new(&counting.mem, block, sizeof(block), &stats, &status);
        if (!current) {
            if (status != MARKDOWN_CORE_SOURCE_NO_MEMORY) {
                fprintf(stderr, "oom_sweep(tree): constructor failure %zu misreported (%d)\n", fail_at, (int)status);
                return -1;
            }
            goto tree_swept;
        }
        for (step = 0; step < SHAPE_STEPS; step++) {
            markdown_core_source_edit edit;
            size_t length = markdown_core_source_length(current);
            size_t expected_length;
            /* The same shape script as tree_shapes, so an allocation
             * failure gets its turn inside every rotation flavour too. */
            shape_edit(step, length, block, sizeof(block), &edit);
            expected_length = length + sizeof(block) - (edit.span.end - edit.span.start);
            if (!markdown_core_source_apply(current, &edit, 1, &stats, &status)) {
                if (status != MARKDOWN_CORE_SOURCE_NO_MEMORY) {
                    fprintf(stderr, "oom_sweep(tree): apply failure %zu misreported (%d)\n", fail_at, (int)status);
                    markdown_core_source_release(current);
                    return -1;
                }
                /* Nothing moved: the source still reads at the length it had. */
                if (markdown_core_source_length(current) != length) {
                    fprintf(stderr, "oom_sweep(tree): failed apply changed the length\n");
                    markdown_core_source_release(current);
                    return -1;
                }
                markdown_core_source_release(current);
                swept_early = 1;
                break;
            }
            if (markdown_core_source_length(current) != expected_length) {
                fprintf(stderr, "oom_sweep(tree): wrong length after step %zu\n", step);
                markdown_core_source_release(current);
                return -1;
            }
        }
        if (swept_early) {
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
    markdown_core_source_stats stats = {0, 0};
    markdown_core_source_status status = MARKDOWN_CORE_SOURCE_OK;
    int profile_index;
    int failed = 0;
    counting_init(&counting);

    /* One run, not two. It looped over the two source profiles; there is one
     * behaviour now, and 7.1 says so. */
    for (profile_index = 0; profile_index < 1 && !failed; profile_index++) {
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
        current = markdown_core_source_new(&counting.mem, NULL, 0, &stats, &status);
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
            for (i = 0; i < count; i++) {
                size_t start = cursor + (size_t)(sr_prng_next(&prng) % (oracle_length - cursor + 1));
                size_t end = start + (size_t)(sr_prng_next(&prng) % (oracle_length - start + 1));
                {
                    /* Legal edits: spans on character boundaries and valid
                     * replacement text (14.3.1). */
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
                }
                edits[i].span.start = start;
                edits[i].span.end = end;
                cursor = end;
            }
            if (!markdown_core_source_apply(current, edits, count, &stats, &status)) {
                fprintf(stderr, "random_edits: legal batch rejected in round %zu (%d, 14.3.1)\n", round, (int)status);
                failed = 1;
                break;
            }
            if (oracle_apply(&oracle, &oracle_length, edits, count) != 0) {
                failed = 1;
                break;
            }
            if (expect_bytes("random_edits", current, oracle, oracle_length) != 0) {
                fprintf(stderr, "random_edits: divergence in round %zu, profile %d (14.3.1)\n", round, profile_index);
                failed = 1;
            }
        }
        /* The committed bytes parse exactly as an independent buffer of the
         * same bytes does: the decode profile owes nothing to history. */
        if (!failed) {
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
    markdown_core_source_stats stats = {0, 0};
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
        for (profile_index = 0; profile_index < 1 && !failed; profile_index++) {
            sr_prng prng;
            markdown_core_source *current = markdown_core_source_new(&counting.mem, NULL, 0, &stats, &status);
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
                if (!markdown_core_source_apply(current, &edit, 1, &stats, &status)) {
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
                markdown_core_source *doc =
                    markdown_core_source_new(&counting.mem, (const uint8_t *)unit, position + split, &stats, &status);
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
                if (!markdown_core_source_apply(doc, &edit, 1, &stats, &status)) {
                    fprintf(
                        stderr,
                        "chunk_partition: completing chunk at %zu+%zu rejected (14.8.2)\n",
                        position,
                        split
                    );
                    failed = 1;
                } else {
                    if (expect_bytes("chunk_partition (completed)", doc, (const uint8_t *)unit, position + width) !=
                        0) {
                        failed = 1;
                    }
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
    {"batch_splice", case_batch_splice},
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
