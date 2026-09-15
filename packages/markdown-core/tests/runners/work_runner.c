/* The work-invariant lane of the benchmark workloads (CTest label: benchmark).
 *
 * The same deterministic inputs as bench_runner, parsed through the
 * diagnostics build of the engine with an injected allocator. What is
 * reported is not time but counts: the parser's deterministic work counters,
 * the nodes built, the allocations made and the bytes they asked for, the
 * peak of live bytes during the parse and the bytes a document retains. The
 * counts are an exact contract per (workload, version, case): `--expect`
 * compares them with the tracked expectations and fails on any difference,
 * and `--write` regenerates the expectations when a change is intended, so a
 * change of work is a reviewed line in a diff rather than a timing that a
 * loaded runner may or may not reproduce. The finishing phases are timed
 * through the parser's phase clock and reported as information only.
 *
 *   work_runner --list
 *   work_runner (--workload NAME | --all) --samples DIR [--json FILE]
 *               [--expect FILE | --write FILE] [--max-ratio R]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#include "bench_workloads.h"
#include "element.h"
#include "map.h"
#include "markdown-core.h"
#include "node.h"
#include "parser.h"
#include "utf8.h"

/* Counting allocator ---------------------------------------------------------
 *
 * A size header in front of every allocation keeps the live-byte count exact
 * without touching the engine. */

typedef struct allocation_header {
    size_t size;
    size_t padding;
} allocation_header;

static size_t live_bytes, peak_live_bytes, requested_bytes, allocation_calls, reallocation_calls;

static void account(size_t old_size, size_t new_size) {
    requested_bytes += new_size;
    live_bytes = live_bytes - old_size + new_size;
    if (live_bytes > peak_live_bytes) {
        peak_live_bytes = live_bytes;
    }
}

static void *counting_calloc(size_t count, size_t size) {
    allocation_header *header;
    size_t bytes;
    if (count && size > (SIZE_MAX - sizeof(*header)) / count) {
        return NULL;
    }
    bytes = count * size;
    header = (allocation_header *)calloc(1, sizeof(*header) + bytes);
    if (!header) {
        return NULL;
    }
    header->size = bytes;
    allocation_calls++;
    account(0, bytes);
    return header + 1;
}

static void counting_free(void *pointer) {
    if (pointer) {
        allocation_header *header = (allocation_header *)pointer - 1;
        account(header->size, 0);
        free(header);
    }
}

static void *counting_realloc(void *pointer, size_t size) {
    allocation_header *header = pointer ? (allocation_header *)pointer - 1 : NULL;
    size_t old_size = header ? header->size : 0;
    if (!size) {
        counting_free(pointer);
        return NULL;
    }
    if (size > SIZE_MAX - sizeof(*header)) {
        return NULL;
    }
    header = (allocation_header *)realloc(header, sizeof(*header) + size);
    if (!header) {
        return NULL;
    }
    header->size = size;
    if (pointer) {
        reallocation_calls++;
    } else {
        allocation_calls++;
    }
    account(old_size, size);
    return header + 1;
}

static markdown_core_mem counting_mem = {counting_calloc, counting_realloc, counting_free};

/* Clock ----------------------------------------------------------------------- */

static uint64_t monotonic_ns(void *context) {
    (void)context;
#if defined(_WIN32)
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }
    QueryPerformanceCounter(&counter);
    return (uint64_t)((counter.QuadPart * 1000000000.0) / frequency.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
#endif
}

/* Work record ----------------------------------------------------------------- */

#define WORK_MAX_ENTRIES 64

typedef struct work_entry {
    const char *name;
    size_t value;
} work_entry;

typedef struct work_record {
    work_entry entries[WORK_MAX_ENTRIES];
    size_t count;
    markdown_core_phase_clock clock;
} work_record;

static void record_entry(work_record *record, const char *name, size_t value) {
    if (record->count < WORK_MAX_ENTRIES) {
        record->entries[record->count].name = name;
        record->entries[record->count].value = value;
        record->count++;
    }
}

#define RECORD(field) record_entry(record, #field, parser->field)

/* Reads every deterministic counter once the tree is complete: the whole-tree
 * postprocess hook runs after the finishing phases, before the tree is
 * handed out. */
static markdown_core_node *record_work(const markdown_core_element *element, markdown_core_parser *parser,
                                       markdown_core_node *root) {
    work_record *record = (work_record *)root->opaque;
    (void)element;
    if (!record) {
        return root;
    }
    root->opaque = NULL;
    RECORD(cross_link_scan_work);
    RECORD(autolink_domain_work);
    RECORD(opaque_scan_work);
    RECORD(footnote_body_work);
    RECORD(definition_registration_work);
    RECORD(delimiter_work);
    RECORD(whitespace_work);
    RECORD(content_map_work);
    RECORD(key_index_work);
    RECORD(key_index_operations);
    RECORD(bracket_work);
    RECORD(comment_scan_work);
    RECORD(html_scan_work);
    RECORD(block_lookahead_work);
    RECORD(block_dispatch_work);
    RECORD(block_indent_probe_work);
    RECORD(reference_probe_work);
    RECORD(completion_work);
    RECORD(finishing_work);
    RECORD(finisher_work);
    RECORD(inline_lifecycle_work);
    RECORD(text_run_extensions);
    RECORD(code_block_move_work);
    RECORD(table_scan_work);
    RECORD(table_frontier_peak);
    RECORD(table_workspace_growth);
    RECORD(table_geometry_lines);
    RECORD(table_separator_scans);
    RECORD(table_row_scans);
    RECORD(table_row_work);
    RECORD(table_geometry_allocations);
    RECORD(table_scratch_growth);
    RECORD(metadata_decoded_bytes);
    RECORD(block_identifier_work);
    RECORD(callout_scan_work);
    RECORD(attribute_work);
    RECORD(anchor_work);
    RECORD(dimension_work);
    RECORD(list_marker_work);
    RECORD(specimen_work);
    RECORD(citation_work);
    RECORD(citation_brace_bytes);
    RECORD(definition_list_work);
    record_entry(record, "reference_fold_work", parser->refmap ? parser->refmap->fold_work : 0);
    record_entry(record, "unicode_range_work", markdown_core_unicode_range_work);
    return root;
}

static const markdown_core_element WORK_RECORDER = {.name = "work-recorder", .postprocess_func = record_work};

static bool install_recorder(markdown_core_parser *parser, void *context) {
    work_record *record = (work_record *)context;
    parser->root->opaque = record;
    parser->phase_clock = &record->clock;
    return markdown_core_parser_attach_element(parser, &WORK_RECORDER);
}

static size_t count_nodes(markdown_core_node *root) {
    size_t nodes = 0;
    markdown_core_iter *iter = markdown_core_iter_new(root);
    markdown_core_event_type event;
    if (!iter) {
        return 0;
    }
    while ((event = markdown_core_iter_next(iter)) != MARKDOWN_CORE_EVENT_DONE) {
        nodes += event == MARKDOWN_CORE_EVENT_ENTER;
    }
    markdown_core_iter_free(iter);
    return nodes;
}

/* Expectations ---------------------------------------------------------------- */

typedef struct expectations {
    char *text;
    char **lines;
    unsigned char *matched;
    size_t count;
} expectations;

static int load_expectations(const char *path, expectations *e) {
    FILE *file = fopen(path, "rb");
    long size;
    size_t i, line = 0;
    if (!file) {
        fprintf(stderr, "cannot read expectations %s\n", path);
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }
    e->text = (char *)malloc((size_t)size + 1);
    if (!e->text || fread(e->text, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        return -1;
    }
    fclose(file);
    e->text[size] = 0;
    for (i = 0; i < (size_t)size; i++) {
        e->count += e->text[i] == '\n';
    }
    e->lines = (char **)calloc(e->count + 1, sizeof(*e->lines));
    e->matched = (unsigned char *)calloc(e->count + 1, 1);
    if (!e->lines || !e->matched) {
        return -1;
    }
    e->lines[line++] = e->text;
    for (i = 0; i < (size_t)size; i++) {
        if (e->text[i] == '\n') {
            e->text[i] = 0;
            if (line <= e->count) {
                e->lines[line++] = e->text + i + 1;
            }
        }
    }
    return 0;
}

typedef struct run {
    expectations *expect;
    FILE *write;
    FILE *json;
    int json_cases;
    double max_ratio;
    int failures;
    const char *series;
    size_t series_previous_total;
} run;

/* The exact line a case's counts make; the expectations file is these
 * lines, one per case, in workload order. */
static void format_line(char *line, size_t capacity, const bench_case *input, size_t nodes, size_t allocations,
                        size_t reallocations, size_t requested, size_t peak, size_t retained,
                        const work_record *record) {
    size_t at = (size_t)snprintf(line, capacity,
                                 "%s v=%d case=%s sha256=%s bytes=%zu nodes=%zu allocations=%zu reallocations=%zu"
                                 " requested_bytes=%zu peak_live_bytes=%zu retained_bytes=%zu work:",
                                 input->workload, input->version, input->name, input->sha256, input->length, nodes,
                                 allocations, reallocations, requested, peak, retained);
    size_t i;
    for (i = 0; i < record->count && at < capacity; i++) {
        at += (size_t)snprintf(line + at, capacity - at, " %s=%zu", record->entries[i].name, record->entries[i].value);
    }
}

static int run_case(const bench_case *input, void *context) {
    run *r = (run *)context;
    work_record record;
    markdown_core_node *root;
    uint64_t started, returned, freed;
    size_t nodes, allocations, reallocations, requested, peak, retained, total, i;
    char line[8192];
    char prefix[512];

    memset(&record, 0, sizeof(record));
    record.clock.now = monotonic_ns;
    live_bytes = peak_live_bytes = requested_bytes = allocation_calls = reallocation_calls = 0;
    markdown_core_unicode_range_work = 0;

    started = monotonic_ns(NULL);
    root = markdown_core_parse_document_with_mem(input->data, input->length, &counting_mem, install_recorder, &record);
    returned = monotonic_ns(NULL);
    if (!root) {
        fprintf(stderr, "%s: parse failed\n", input->name);
        return 1;
    }
    /* The document's own footprint, before anything else allocates. */
    allocations = allocation_calls;
    reallocations = reallocation_calls;
    requested = requested_bytes;
    peak = peak_live_bytes;
    retained = live_bytes;
    nodes = count_nodes(root);
    freed = monotonic_ns(NULL);
    markdown_core_node_free(root);
    freed = monotonic_ns(NULL) - freed;
    if (live_bytes != 0) {
        fprintf(stderr, "%s: %zu bytes still live after the document was freed\n", input->name, live_bytes);
        r->failures++;
    }

    format_line(line, sizeof(line), input, nodes, allocations, reallocations, requested, peak, retained, &record);
    printf("work %s\n", line);
    printf("phases case=%s blocks_ns=%llu prepare_ns=%llu inlines_ns=%llu finish_ns=%llu postprocess_ns=%llu"
           " free_ns=%llu\n",
           input->name, (unsigned long long)(record.clock.blocks - started),
           (unsigned long long)(record.clock.prepared - record.clock.blocks),
           (unsigned long long)(record.clock.inlines - record.clock.prepared),
           (unsigned long long)(record.clock.finished - record.clock.inlines),
           (unsigned long long)(returned - record.clock.finished), (unsigned long long)freed);

    total = allocations + reallocations;
    for (i = 0; i < record.count; i++) {
        total += record.entries[i].value;
    }
    if (input->step > 1 && r->series && strcmp(r->series, input->generator) == 0) {
        double ratio = (double)total / (double)(r->series_previous_total ? r->series_previous_total : 1);
        printf("work scaling=%s scale=%zu total=%zu adjacent_ratio=%.3f\n", input->generator, input->scale, total,
               ratio);
        if (r->max_ratio > 0 && ratio > r->max_ratio) {
            fprintf(stderr, "%s: work grew %.3fx across a doubling, above %.3f\n", input->name, ratio, r->max_ratio);
            r->failures++;
        }
    }
    r->series = input->step ? input->generator : NULL;
    r->series_previous_total = total;

    if (r->write) {
        fprintf(r->write, "%s\n", line);
    }
    if (r->expect) {
        size_t found = 0;
        snprintf(prefix, sizeof(prefix), "%s v=%d case=%s ", input->workload, input->version, input->name);
        for (i = 0; i < r->expect->count; i++) {
            if (strncmp(r->expect->lines[i], prefix, strlen(prefix)) == 0) {
                found = 1;
                r->expect->matched[i] = 1;
                if (strcmp(r->expect->lines[i], line) != 0) {
                    fprintf(stderr, "%s: work changed\n  expected: %s\n  actual:   %s\n", input->name,
                            r->expect->lines[i], line);
                    r->failures++;
                }
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "%s: no expectation for this case; regenerate with --write\n", input->name);
            r->failures++;
        }
    }
    if (r->json) {
        fprintf(r->json,
                "%s    {\n      \"workload\": \"%s\",\n      \"workloadVersion\": %d,\n      \"name\": \"%s\",\n"
                "      \"generator\": \"%s\",\n      \"parameters\": \"%s\",\n      \"scale\": %zu,\n"
                "      \"bytes\": %zu,\n      \"inputSha256\": \"%s\",\n      \"nodes\": %zu,\n"
                "      \"allocations\": %zu,\n      \"reallocations\": %zu,\n      \"requestedBytes\": %zu,\n"
                "      \"peakLiveBytes\": %zu,\n      \"retainedBytes\": %zu,\n      \"workTotal\": %zu,\n"
                "      \"work\": {",
                r->json_cases ? ",\n" : "", input->workload, input->version, input->name, input->generator,
                input->parameters, input->scale, input->length, input->sha256, nodes, allocations, reallocations,
                requested, peak, retained, total);
        for (i = 0; i < record.count; i++) {
            fprintf(r->json, "%s\"%s\": %zu", i ? ", " : "", record.entries[i].name, record.entries[i].value);
        }
        fprintf(r->json,
                "},\n      \"phasesNs\": {\"blocks\": %llu, \"prepare\": %llu, \"inlines\": %llu, \"finish\": %llu,"
                " \"postprocess\": %llu, \"free\": %llu}\n    }",
                (unsigned long long)(record.clock.blocks - started),
                (unsigned long long)(record.clock.prepared - record.clock.blocks),
                (unsigned long long)(record.clock.inlines - record.clock.prepared),
                (unsigned long long)(record.clock.finished - record.clock.inlines),
                (unsigned long long)(returned - record.clock.finished), (unsigned long long)freed);
        r->json_cases++;
    }
    return 0;
}

static int usage(void) {
    fputs("usage: work_runner --list | (--workload NAME | --all) --samples DIR [--json FILE]"
          " [--expect FILE | --write FILE] [--max-ratio R]\n",
          stderr);
    return 2;
}

int main(int argc, char **argv) {
    const char *workload_name = NULL, *samples_dir = NULL, *json_path = NULL, *expect_path = NULL, *write_path = NULL;
    int list_only = 0, all = 0;
    double max_ratio = 0.0;
    expectations expect;
    run r;
    size_t i;

    memset(&expect, 0, sizeof(expect));
    memset(&r, 0, sizeof(r));
    for (i = 1; i < (size_t)argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            list_only = 1;
        } else if (strcmp(argv[i], "--all") == 0) {
            all = 1;
        } else if (strcmp(argv[i], "--workload") == 0 && i + 1 < (size_t)argc) {
            workload_name = argv[++i];
        } else if (strcmp(argv[i], "--samples") == 0 && i + 1 < (size_t)argc) {
            samples_dir = argv[++i];
        } else if (strcmp(argv[i], "--json") == 0 && i + 1 < (size_t)argc) {
            json_path = argv[++i];
        } else if (strcmp(argv[i], "--expect") == 0 && i + 1 < (size_t)argc) {
            expect_path = argv[++i];
        } else if (strcmp(argv[i], "--write") == 0 && i + 1 < (size_t)argc) {
            write_path = argv[++i];
        } else if (strcmp(argv[i], "--max-ratio") == 0 && i + 1 < (size_t)argc) {
            max_ratio = atof(argv[++i]);
        } else {
            return usage();
        }
    }
    if (list_only) {
        for (i = 0; i < bench_workload_count(); i++) {
            puts(bench_workload_name(i));
        }
        return 0;
    }
    if ((!workload_name && !all) || (workload_name && all) || !samples_dir || (expect_path && write_path)) {
        return usage();
    }
    if (expect_path && load_expectations(expect_path, &expect) != 0) {
        return 1;
    }
    r.expect = expect_path ? &expect : NULL;
    r.max_ratio = max_ratio;
    if (write_path) {
        r.write = fopen(write_path, "wb");
        if (!r.write) {
            fprintf(stderr, "cannot write %s\n", write_path);
            return 1;
        }
    }
    if (json_path) {
        r.json = fopen(json_path, "wb");
        if (!r.json) {
            fprintf(stderr, "cannot write %s\n", json_path);
            return 1;
        }
        fprintf(r.json, "{\n  \"schema\": 2,\n  \"runtime\": \"c\",\n  \"lane\": \"work\",\n  \"cases\": [\n");
    }
    for (i = 0; i < bench_workload_count(); i++) {
        const char *name = bench_workload_name(i);
        int result;
        if (!all && strcmp(name, workload_name) != 0) {
            continue;
        }
        result = bench_workload_visit(name, samples_dir, run_case, &r);
        if (result == -1) {
            fprintf(stderr, "%s: cannot build input\n", name);
            r.failures++;
        } else if (result != 0) {
            r.failures++;
        }
    }
    if (!all && bench_workload_version(workload_name) == 0) {
        fprintf(stderr, "unknown workload: %s\n", workload_name);
        return 2;
    }
    if (r.expect && all) {
        for (i = 0; i < expect.count; i++) {
            if (expect.lines[i][0] && !expect.matched[i]) {
                fprintf(stderr, "expectation without a case; regenerate with --write:\n  %s\n", expect.lines[i]);
                r.failures++;
            }
        }
    }
    if (r.json) {
        fprintf(r.json, "\n  ]\n}\n");
        fclose(r.json);
    }
    if (r.write) {
        fclose(r.write);
    }
    if (r.failures) {
        fprintf(stderr, "%d work-invariant failure(s)\n", r.failures);
        return 1;
    }
    return 0;
}
