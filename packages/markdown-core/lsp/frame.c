#include "frame.h"

#include <stdlib.h>
#include <string.h>

/* The longest header line read, and the largest body: a 1 GiB document
 * with every byte escaped, and room for the message around it. */
enum { FRAME_LINE = 1024 };
static const uint64_t FRAME_BODY = (uint64_t)6 << 30;

/* Reads one header line without its `\r\n` into `line`; its size, or -1 at
 * the end of the input before any byte, or -2 when it is malformed. */
static long read_line(FILE *in, char *line) {
    long size = 0;
    for (;;) {
        int byte = fgetc(in);
        if (byte == EOF) {
            return size == 0 ? -1 : -2;
        }
        if (byte == '\n') {
            return size > 0 && line[size - 1] == '\r' ? size - 1 : -2;
        }
        if (size == FRAME_LINE) {
            return -2;
        }
        line[size++] = (char)byte;
    }
}

static bool named(const char *line, long size, const char *name) {
    long length = (long)strlen(name);
    if (size < length + 1 || line[length] != ':') {
        return false;
    }
    for (long i = 0; i < length; i++) {
        char letter = line[i];
        if (letter >= 'A' && letter <= 'Z') {
            letter = (char)(letter - 'A' + 'a');
        }
        if (letter != name[i]) {
            return false;
        }
    }
    return true;
}

frame_status frame_read(FILE *in, frame *message) {
    char line[FRAME_LINE];
    bool sized = false, first = true;
    uint64_t size = 0;
    for (;;) {
        long read = read_line(in, line);
        if (read == -1 && first) {
            return FRAME_END;
        }
        if (read < 0) {
            return FRAME_BROKEN;
        }
        first = false;
        if (read == 0) {
            break;
        }
        if (named(line, read, "content-length")) {
            long at = (long)strlen("content-length") + 1;
            while (at < read && line[at] == ' ') {
                at++;
            }
            if (at == read) {
                return FRAME_BROKEN;
            }
            size = 0;
            for (; at < read; at++) {
                if (line[at] < '0' || line[at] > '9' || size > FRAME_BODY) {
                    return FRAME_BROKEN;
                }
                size = size * 10 + (uint64_t)(line[at] - '0');
            }
            sized = true;
        }
    }
    if (!sized || size > FRAME_BODY || size > SIZE_MAX) {
        return FRAME_BROKEN;
    }
    if (size > message->capacity) {
        uint8_t *body = (uint8_t *)realloc(message->body, (size_t)size);
        if (!body) {
            return FRAME_BROKEN;
        }
        message->body = body;
        message->capacity = (size_t)size;
    }
    if (size && fread(message->body, 1, (size_t)size, in) != (size_t)size) {
        return FRAME_BROKEN;
    }
    message->size = (size_t)size;
    return FRAME_OK;
}

bool frame_write(FILE *out, const uint8_t *body, size_t size) {
    return fprintf(out, "Content-Length: %llu\r\n\r\n", (unsigned long long)size) > 0 &&
           (size == 0 || fwrite(body, 1, size, out) == size) && fflush(out) == 0;
}

void frame_release(frame *message) {
    free(message->body);
    memset(message, 0, sizeof(*message));
}
