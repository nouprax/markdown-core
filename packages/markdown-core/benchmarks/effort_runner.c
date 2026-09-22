#include "effort_runner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* These two edges expose, rather than hide, the price of the local boundary.
 * Failed preparation is a failed run, never a zero-cost operation. */
EFFORT_NOINLINE effort_state *bench_effort_prepare(const unsigned char *input, int32_t length, int count, int start,
                                                   int ticks, effort_op operation) {
    effort_state *states = calloc((size_t)count, sizeof(*states));
    if (!states) {
        return NULL;
    }
    for (int i = 0; i < count; i++) {
        effort_state *s = states + i;
        s->input = input;
        s->length = length;
        s->capacity = (operation == EFFORT_OWNERS ? length * 5 : length) + 1;
        s->start = start;
        s->ticks = ticks;
        s->data = malloc((size_t)s->capacity);
        if (!s->data) {
            for (int j = 0; j < i; j++) {
                free(states[j].data);
            }
            free(states);
            return NULL;
        }
        s->size = operation == EFFORT_COPY ? 0 : length;
        if (s->size) {
            memcpy(s->data, input, (size_t)s->size);
        }
        s->data[s->size] = 0;
    }
    return states;
}

EFFORT_NOINLINE void bench_effort_release(effort_state *states, int count) {
    for (int i = 0; i < count; i++) {
        free(states[i].data);
        free(states[i].native_nodes);
    }
    free(states);
}

int main(int argc, char **argv) {
    const char *names[] = {"copy", "trim", "unescape", "whitespace", "code", "closer", "owners"};
    long count, start, ticks;
    char *end;
    int operation;
    if (argc != 6) {
        return 2;
    }
    for (operation = 0; operation < 7 && strcmp(names[operation], argv[1]); operation++) {
    }
    if (operation == 7) {
        return 2;
    }
    count = strtol(argv[3], &end, 10);
    if (end == argv[3] || *end || count < 1 || count > 128) {
        return 2;
    }
    start = strtol(argv[4], &end, 10);
    if (end == argv[4] || *end || start < 0 || start > INT32_MAX / 2) {
        return 2;
    }
    ticks = strtol(argv[5], &end, 10);
    if (end == argv[5] || *end || ticks < 1 || ticks > 80) {
        return 2;
    }
    FILE *file = fopen(argv[2], "rb");
    if (!file) {
        return 1;
    }
    long length;
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 || length >= INT32_MAX / 2 || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return 1;
    }
    unsigned char *input = malloc((size_t)length + 1);
    if (!input) {
        fclose(file);
        return 1;
    }
    size_t got = fread(input, 1, (size_t)length, file);
    fclose(file);
    input[length] = 0;
    if (got != (size_t)length) {
        free(input);
        return 1;
    }
    if (operation == EFFORT_OWNERS) {
        if (length < 4 || length % 4 || length >= INT32_MAX / 10 || effort_read_index(input) != UINT32_MAX) {
            free(input);
            return 2;
        }
        for (long i = 1; i < length / 4; i++) {
            if (effort_read_index(input + i * 4) >= (uint32_t)i) {
                free(input);
                return 2;
            }
        }
    }
    if (start > length ||
        ((operation == EFFORT_TRIM || operation == EFFORT_WHITESPACE) &&
         (memchr(input, 11, (size_t)length) || memchr(input, 12, (size_t)length))) ||
        ((operation == EFFORT_CODE || operation == EFFORT_CLOSER) &&
         (memchr(input, 0, (size_t)length) || memchr(input, '\r', (size_t)length))) ||
        (operation == EFFORT_CLOSER && start > 0 && input[start - 1] == '`' && start < length && input[start] == '`')) {
        free(input);
        return 2;
    }
    effort_state *states =
        bench_effort_prepare(input, (int32_t)length, (int)count, (int)start, (int)ticks, (effort_op)operation);
    if (!states) {
        free(input);
        return 1;
    }
    for (int i = 0; i < count; i++) {
        bench_effort_operation(states + i, (effort_op)operation);
    }
    /* Inspect EVERY invocation, including its terminator. Outside measurement. */
    for (int i = 0; i < count; i++) {
        effort_state *s = states + i;
        if (operation == EFFORT_OWNERS && !s->failed) {
            bench_effort_observe_owners(s);
        }
        if (s->failed || s->size < 0 || s->size >= s->capacity || s->data[s->size]) {
            bench_effort_release(states, (int)count);
            free(input);
            return 1;
        }
        printf("{\"source\":\"");
        for (int j = 0; j < s->length; j++) {
            printf("%02x", s->input[j]);
        }
        printf("\",\"hex\":\"");
        for (int j = 0; j < s->size; j++) {
            printf("%02x", s->data[j]);
        }
        printf("\",\"position\":%d,\"result\":%d,\"scanned\":%d,\"cache\":[", s->position, s->result, s->scanned);
        for (int j = 0; j <= 80; j++) {
            printf("%s%d", j ? "," : "", s->cache[j]);
        }
        puts("]}");
    }
    bench_effort_release(states, (int)count);
    free(input);
    return 0;
}
