#ifndef MARKDOWN_CORE_KOTLIN_BRIDGE_H
#define MARKDOWN_CORE_KOTLIN_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* THE FACADE'S OWN SESSION TYPE, forward-declared by tag so this header stays
 * self-contained -- cinterop reads it alone, and an incomplete type is all a
 * handle needs. */
typedef struct markdown_core_session markdown_core_kotlin_session;

/* THE STREAM (docs/STREAMING.md §4 D5), one bridge call per facade call.
 * `session_new` answers NULL only for the allocation failure the facade can
 * report there, so no payload crosses; `session_feed` and `session_finish`
 * answer false when the payload buffer itself could not be built, otherwise
 * an MKC7 envelope -- the versioned magic, a status byte, then either the
 * facade's own `markdown_core_document_wire` bytes or the error's code and
 * message, the finished-session refusal included. */
markdown_core_kotlin_session *markdown_core_kotlin_session_new(uint32_t options_mask);
bool markdown_core_kotlin_session_feed(
    markdown_core_kotlin_session *session,
    const uint8_t *chunk,
    size_t length,
    uint8_t **output,
    size_t *output_length
);
bool markdown_core_kotlin_session_finish(
    markdown_core_kotlin_session *session,
    uint8_t **output,
    size_t *output_length
);
/* Feed whose read is DISCARDED BY CONTRACT (the constructor's initial feed):
 * no projection, no serialization -- the answer is the bare MKC7 envelope,
 * status 0, or the error's code and message behind status 1. */
bool markdown_core_kotlin_session_advance(
    markdown_core_kotlin_session *session,
    const uint8_t *chunk,
    size_t length,
    uint8_t **output,
    size_t *output_length
);
void markdown_core_kotlin_session_free(markdown_core_kotlin_session *session);

void markdown_core_kotlin_free(uint8_t *output);

#endif
