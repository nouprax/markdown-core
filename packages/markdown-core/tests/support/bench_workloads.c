#include "bench_workloads.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SHA-256 (FIPS 180-4), enough to fingerprint an input. --------------------- */

typedef struct sha256_state {
    uint32_t h[8];
    uint64_t length;
    unsigned char block[64];
    size_t used;
} sha256_state;

static const uint32_t SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static uint32_t rotr(uint32_t value, unsigned bits) { return (value >> bits) | (value << (32 - bits)); }

static void sha256_compress(sha256_state *state, const unsigned char *block) {
    uint32_t w[64], a, b, c, d, e, f, g, h;
    size_t i;
    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) | ((uint32_t)block[i * 4 + 2] << 8) |
               (uint32_t)block[i * 4 + 3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = state->h[0];
    b = state->h[1];
    c = state->h[2];
    d = state->h[3];
    e = state->h[4];
    f = state->h[5];
    g = state->h[6];
    h = state->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + SHA256_K[i] + w[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state->h[0] += a;
    state->h[1] += b;
    state->h[2] += c;
    state->h[3] += d;
    state->h[4] += e;
    state->h[5] += f;
    state->h[6] += g;
    state->h[7] += h;
}

static void sha256_update(sha256_state *state, const unsigned char *data, size_t length) {
    state->length += length;
    while (length > 0) {
        size_t take = 64 - state->used;
        if (take > length) {
            take = length;
        }
        memcpy(state->block + state->used, data, take);
        state->used += take;
        data += take;
        length -= take;
        if (state->used == 64) {
            sha256_compress(state, state->block);
            state->used = 0;
        }
    }
}

void bench_sha256_hex(const void *data, size_t length, char hex[65]) {
    static const char digits[] = "0123456789abcdef";
    sha256_state state = {
        {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}, 0, {0}, 0};
    unsigned char tail[72] = {0x80};
    uint64_t bits;
    size_t pad, i;
    sha256_update(&state, (const unsigned char *)data, length);
    bits = state.length * 8;
    pad = state.used < 56 ? 56 - state.used : 120 - state.used;
    for (i = 0; i < 8; i++) {
        tail[pad + i] = (unsigned char)(bits >> (56 - 8 * i));
    }
    /* The padding is fed through the block buffer, so no length is added twice. */
    {
        uint64_t kept = state.length;
        sha256_update(&state, tail, pad + 8);
        state.length = kept;
    }
    for (i = 0; i < 8; i++) {
        uint32_t word = state.h[i];
        hex[i * 8] = digits[(word >> 28) & 15];
        hex[i * 8 + 1] = digits[(word >> 24) & 15];
        hex[i * 8 + 2] = digits[(word >> 20) & 15];
        hex[i * 8 + 3] = digits[(word >> 16) & 15];
        hex[i * 8 + 4] = digits[(word >> 12) & 15];
        hex[i * 8 + 5] = digits[(word >> 8) & 15];
        hex[i * 8 + 6] = digits[(word >> 4) & 15];
        hex[i * 8 + 7] = digits[word & 15];
    }
    hex[64] = 0;
}

/* Growable text -------------------------------------------------------------- */

typedef struct text {
    char *data;
    size_t length, capacity;
    int failed;
} text;

static void text_reserve(text *t, size_t extra) {
    if (t->failed || t->length + extra + 1 <= t->capacity) {
        return;
    }
    size_t capacity = t->capacity ? t->capacity : 4096;
    while (capacity < t->length + extra + 1) {
        capacity *= 2;
    }
    char *grown = (char *)realloc(t->data, capacity);
    if (!grown) {
        t->failed = 1;
        return;
    }
    t->data = grown;
    t->capacity = capacity;
}

static void text_put(text *t, const char *bytes, size_t length) {
    text_reserve(t, length);
    if (t->failed) {
        return;
    }
    memcpy(t->data + t->length, bytes, length);
    t->length += length;
    t->data[t->length] = 0;
}

static void text_puts(text *t, const char *string) { text_put(t, string, strlen(string)); }

static void text_repeat(text *t, const char *unit, size_t count) {
    size_t unit_length = strlen(unit), i;
    text_reserve(t, unit_length * count);
    for (i = 0; i < count && !t->failed; i++) {
        text_put(t, unit, unit_length);
    }
}

static void text_printf(text *t, const char *format, size_t value) {
    char buffer[64];
    int written = snprintf(buffer, sizeof(buffer), format, value);
    if (written > 0) {
        text_put(t, buffer, (size_t)written);
    }
}

static char *read_sample(const char *samples_dir, const char *name, size_t *length) {
    char path[1024];
    FILE *file;
    long size;
    char *data;
    snprintf(path, sizeof(path), "%s/%s", samples_dir, name);
    file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = (char *)malloc((size_t)size + 1);
    if (!data) {
        fclose(file);
        return NULL;
    }
    if (fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    data[size] = 0;
    *length = (size_t)size;
    return data;
}

/* The tracked samples, in the order the representative workload measures. */
static const char *const SAMPLES[] = {
    "block-bq-flat.md",  "block-bq-nested.md",   "block-code.md",          "block-fences.md",    "block-heading.md",
    "block-hr.md",       "block-html.md",        "block-lheading.md",      "block-list-flat.md", "block-list-nested.md",
    "block-ref-flat.md", "block-ref-nested.md",  "directive.md",           "inline-autolink.md", "inline-backticks.md",
    "inline-em-flat.md", "inline-em-nested.md",  "inline-em-worst.md",     "inline-entity.md",   "inline-escape.md",
    "inline-html.md",    "inline-links-flat.md", "inline-links-nested.md", "inline-newlines.md", "lorem1.md",
    "rawtabs.md",
};
#define SAMPLE_COUNT (sizeof(SAMPLES) / sizeof(SAMPLES[0]))

/* Generators ------------------------------------------------------------------
 *
 * Each takes the samples directory and a scale and appends one input. The
 * scale of a standalone case is fixed by its workload. */

typedef void (*generator_func)(text *t, const char *samples_dir, size_t scale);

/* Every tracked sample once, each followed by a line ending. */
static void sample_block(text *t, const char *samples_dir) {
    size_t i;
    for (i = 0; i < SAMPLE_COUNT && !t->failed; i++) {
        size_t length = 0;
        char *sample = read_sample(samples_dir, SAMPLES[i], &length);
        if (!sample) {
            t->failed = 1;
            return;
        }
        text_put(t, sample, length);
        text_puts(t, "\n");
        free(sample);
    }
}

static void generate_binding_baseline(text *t, const char *samples_dir, size_t scale) {
    (void)samples_dir;
    text_repeat(t, "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n", scale);
}

/* One tracked sample repeated; a blank line between copies keeps each copy's
 * blocks its own, so the repeated shape is the sample's shape. */
static void generate_sample(text *t, const char *samples_dir, const char *name, size_t copies) {
    size_t length = 0, i;
    char *sample = read_sample(samples_dir, name, &length);
    if (!sample) {
        t->failed = 1;
        return;
    }
    for (i = 0; i < copies && !t->failed; i++) {
        text_put(t, sample, length);
        text_puts(t, "\n\n");
    }
    free(sample);
}

static void generate_large_document(text *t, const char *samples_dir, size_t scale) {
    text block = {0};
    size_t i;
    sample_block(&block, samples_dir);
    if (block.failed) {
        t->failed = 1;
    }
    for (i = 0; i < scale && !t->failed; i++) {
        text_put(t, block.data, block.length);
    }
    free(block.data);
}

static void generate_deep_nesting(text *t, const char *samples_dir, size_t scale) {
    (void)samples_dir;
    text_repeat(t, "> ", scale);
    text_puts(t, "a");
}

static void generate_elements(text *t, const char *samples_dir, size_t scale) {
    generate_sample(t, samples_dir, "directive.md", scale);
}

static void generate_adversarial_links(text *t, const char *samples_dir, size_t scale) {
    (void)samples_dir;
    text_repeat(t, "[a](b", scale);
}

static void generate_adversarial_emphasis(text *t, const char *samples_dir, size_t scale) {
    (void)samples_dir;
    text_repeat(t, "*a_ ", scale);
}

/* One fenced code block of `scale` lines: the copy-bound path, no inlines. */
static void generate_long_fence(text *t, const char *samples_dir, size_t scale) {
    (void)samples_dir;
    text_puts(t, "```c\n");
    text_repeat(t, "    static int value = compute(argument_one, argument_two) + 42; /* fill */\n", scale);
    text_puts(t, "```\n");
}

/* One paragraph of `scale` prose lines: a single inline root. */
static void generate_long_paragraph(text *t, const char *samples_dir, size_t scale) {
    (void)samples_dir;
    text_repeat(t, "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor.\n", scale);
}

/* `scale` paragraphs that use references, then `scale` definitions at the
 * end: every reference resolves after the block pass. */
static void generate_trailing_definitions(text *t, const char *samples_dir, size_t scale) {
    size_t i;
    (void)samples_dir;
    for (i = 0; i < scale && !t->failed; i++) {
        text_puts(t, "See [r");
        text_printf(t, "%zu", i);
        text_puts(t, "] and its note.\n\n");
    }
    for (i = 0; i < scale && !t->failed; i++) {
        text_puts(t, "[r");
        text_printf(t, "%zu", i);
        text_puts(t, "]: https://example.com/r");
        text_printf(t, "%zu", i);
        text_puts(t, "\n");
    }
}

/* `scale` blank lines before one paragraph. */
static void generate_leading_blank_lines(text *t, const char *samples_dir, size_t scale) {
    (void)samples_dir;
    text_repeat(t, "\n", scale);
    text_puts(t, "paragraph\n");
}

/* `scale` nested bracketed spans around 64 independent autolinks. */
static void generate_deep_wide(text *t, const char *samples_dir, size_t scale) {
    (void)samples_dir;
    text_repeat(t, "[", scale);
    text_repeat(t, "a@b.co ", 64);
    text_repeat(t, "]{}", scale);
    text_puts(t, "\n");
}

/* No bytes at all: the fixed cost of a parse, which the registry's prepared
 * projection keeps to the parser, its arena and buffers, and its maps. */
static void generate_empty_document(text *t, const char *samples_dir, size_t scale) {
    (void)t;
    (void)samples_dir;
    (void)scale;
}

/* Workloads ------------------------------------------------------------------- */

typedef struct workload_case {
    const char *name; /* NULL: the representative sample list */
    const char *generator;
    generator_func generate;
    const char *sample; /* representative only */
    size_t scale;
    size_t step; /* index in a doubling series, else 0 */
} workload_case;

typedef struct workload {
    const char *name;
    int version;
    const workload_case *cases;
    size_t case_count;
} workload;

static void generate_tables(text *t, const char *samples_dir, size_t scale);
static const char *const TABLE_UNITS[] = {": caption\n| h |\n| - |\n| b |\n\n", "h    i\n---- ----\na    b\n\n",
                                          "---------\nh    i\n---- ----\na    b\n\nc    d\n---------\n\n",
                                          "+---+---+\n| a | b |\n+---+---+\n\n",
                                          "header with Unicode: 表\n---x ---\n\n"};
static void generate_tables(text *t, const char *samples_dir, size_t scale) {
    (void)samples_dir;
    text_repeat(t, TABLE_UNITS[scale], 2000);
}

#define DOUBLING(generator_name, fn, a, b, c)                                                                          \
    {generator_name "@" #a, generator_name, fn, NULL, a, 1}, {generator_name "@" #b, generator_name, fn, NULL, b, 2},  \
        {generator_name "@" #c, generator_name, fn, NULL, c, 3}

static const workload_case BINDING_BASELINE_CASES[] = {
    {"binding_baseline", "binding_baseline", generate_binding_baseline, NULL, 2000, 0}};
static const workload_case EMPTY_DOCUMENT_CASES[] = {
    {"empty_document", "empty_document", generate_empty_document, NULL, 0, 0}};
static const workload_case LARGE_DOCUMENT_CASES[] = {
    DOUBLING("large_document", generate_large_document, 128, 256, 512)};
static const workload_case DEEP_NESTING_CASES[] = {DOUBLING("deep_nesting", generate_deep_nesting, 8192, 16384, 32768)};
static const workload_case ELEMENTS_CASES[] = {DOUBLING("elements", generate_elements, 100, 200, 400)};
static const workload_case TABLES_CASES[] = {{"tables_pipe_caption", "tables", generate_tables, NULL, 0, 0},
                                             {"tables_simple", "tables", generate_tables, NULL, 1, 0},
                                             {"tables_multiline", "tables", generate_tables, NULL, 2, 0},
                                             {"tables_grid", "tables", generate_tables, NULL, 3, 0},
                                             {"tables_rejected", "tables", generate_tables, NULL, 4, 0}};
static const workload_case ADVERSARIAL_CASES[] = {
    DOUBLING("adversarial_links", generate_adversarial_links, 16384, 32768, 65536),
    DOUBLING("adversarial_emphasis", generate_adversarial_emphasis, 16384, 32768, 65536)};
static const workload_case LONG_FENCE_CASES[] = {DOUBLING("long_fence", generate_long_fence, 2048, 4096, 8192)};
static const workload_case LONG_PARAGRAPH_CASES[] = {
    DOUBLING("long_paragraph", generate_long_paragraph, 1024, 2048, 4096)};
static const workload_case TRAILING_DEFINITIONS_CASES[] = {
    DOUBLING("trailing_definitions", generate_trailing_definitions, 512, 1024, 2048)};
static const workload_case LEADING_BLANK_LINES_CASES[] = {
    DOUBLING("leading_blank_lines", generate_leading_blank_lines, 2048, 4096, 8192)};
static const workload_case DEEP_WIDE_CASES[] = {DOUBLING("deep_wide", generate_deep_wide, 64, 128, 256)};

#define WORKLOAD(name, version, cases) {name, version, cases, sizeof(cases) / sizeof(cases[0])}
static const workload WORKLOADS[] = {
    WORKLOAD("binding_baseline", 1, BINDING_BASELINE_CASES),
    WORKLOAD("empty_document", 1, EMPTY_DOCUMENT_CASES),
    /* Version 2: a blank line separates the copies of a sample. */
    {"representative", 2, NULL, SAMPLE_COUNT},
    WORKLOAD("large_document", 1, LARGE_DOCUMENT_CASES),
    WORKLOAD("deep_nesting", 1, DEEP_NESTING_CASES),
    WORKLOAD("elements", 1, ELEMENTS_CASES),
    WORKLOAD("tables", 1, TABLES_CASES),
    WORKLOAD("adversarial", 1, ADVERSARIAL_CASES),
    WORKLOAD("long_fence", 1, LONG_FENCE_CASES),
    WORKLOAD("long_paragraph", 1, LONG_PARAGRAPH_CASES),
    WORKLOAD("trailing_definitions", 1, TRAILING_DEFINITIONS_CASES),
    WORKLOAD("leading_blank_lines", 1, LEADING_BLANK_LINES_CASES),
    WORKLOAD("deep_wide", 1, DEEP_WIDE_CASES),
};
#define WORKLOAD_COUNT (sizeof(WORKLOADS) / sizeof(WORKLOADS[0]))

size_t bench_workload_count(void) { return WORKLOAD_COUNT; }

const char *bench_workload_name(size_t index) { return index < WORKLOAD_COUNT ? WORKLOADS[index].name : NULL; }

static const workload *find_workload(const char *name) {
    size_t i;
    for (i = 0; i < WORKLOAD_COUNT; i++) {
        if (strcmp(WORKLOADS[i].name, name) == 0) {
            return &WORKLOADS[i];
        }
    }
    return NULL;
}

int bench_workload_version(const char *name) {
    const workload *w = find_workload(name);
    return w ? w->version : 0;
}

static int visit_input(const workload *w, const char *name, const char *generator, const char *parameters, size_t scale,
                       size_t step, text *t, bench_case_visitor visit, void *context) {
    bench_case input;
    int result;
    if (t->failed) {
        free(t->data);
        return -1;
    }
    if (!t->data) {
        text_puts(t, "");
        if (t->failed) {
            return -1;
        }
    }
    memset(&input, 0, sizeof(input));
    input.workload = w->name;
    input.version = w->version;
    snprintf(input.name, sizeof(input.name), "%s", name);
    input.generator = generator;
    snprintf(input.parameters, sizeof(input.parameters), "%s", parameters);
    input.scale = scale;
    input.step = step;
    input.data = t->data;
    input.length = t->length;
    bench_sha256_hex(t->data, t->length, input.sha256);
    result = visit(&input, context);
    free(t->data);
    return result;
}

int bench_workload_visit(const char *name, const char *samples_dir, bench_case_visitor visit, void *context) {
    return bench_workload_visit_copies(name, samples_dir, 0, visit, context);
}

int bench_workload_visit_copies(const char *name, const char *samples_dir, size_t copies, bench_case_visitor visit,
                                void *context) {
    const workload *w = find_workload(name);
    size_t i;
    if (!w) {
        return -2;
    }
    if (!w->cases) {
        /* representative: every tracked sample, 200 copies each unless the
         * caller asked for another count. */
        size_t each = copies ? copies : 200;
        for (i = 0; i < SAMPLE_COUNT; i++) {
            text t = {0};
            char parameters[128];
            int result;
            generate_sample(&t, samples_dir, SAMPLES[i], each);
            snprintf(parameters, sizeof(parameters), "sample=%s copies=%zu", SAMPLES[i], each);
            result = visit_input(w, SAMPLES[i], "sample", parameters, 0, 0, &t, visit, context);
            if (result) {
                return result;
            }
        }
        return 0;
    }
    for (i = 0; i < w->case_count; i++) {
        const workload_case *c = &w->cases[i];
        text t = {0};
        char parameters[128];
        int result;
        c->generate(&t, samples_dir, c->scale);
        if (c->generate == generate_tables) {
            snprintf(parameters, sizeof(parameters), "unit=%zu copies=2000", c->scale);
        } else {
            snprintf(parameters, sizeof(parameters), "scale=%zu", c->scale);
        }
        result = visit_input(w, c->name, c->generator, parameters, c->step ? c->scale : 0, c->step, &t, visit, context);
        if (result) {
            return result;
        }
    }
    return 0;
}
