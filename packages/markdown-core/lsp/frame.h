#ifndef MARKDOWN_CORE_LSP_FRAME_H
#define MARKDOWN_CORE_LSP_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * The protocol's base framing: a header of `Name: value` lines, each ended
 * by `\r\n`, an empty line, and a body of as many bytes as the header's
 * Content-Length names. Other header fields, such as Content-Type, are read
 * and ignored.
 */

typedef struct frame {
    uint8_t *body;
    size_t size;
    size_t capacity;
} frame;

typedef enum frame_status {
    /* A message was read into the frame. */
    FRAME_OK = 0,
    /* The input ended before a message began. */
    FRAME_END = 1,
    /* The input ended inside a message, a header line is malformed or too
     * long, the header names no length, or the body cannot be held. The
     * stream cannot be read past it. */
    FRAME_BROKEN = 2
} frame_status;

frame_status frame_read(FILE *in, frame *message);
/* Writes `size` bytes of `body` as one message; false when the write fails. */
bool frame_write(FILE *out, const uint8_t *body, size_t size);
void frame_release(frame *message);

#endif
