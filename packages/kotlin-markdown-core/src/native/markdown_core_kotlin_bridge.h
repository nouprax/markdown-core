#ifndef MARKDOWN_CORE_KOTLIN_BRIDGE_H
#define MARKDOWN_CORE_KOTLIN_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* THE FACADE'S OWN SESSION TYPE, forward-declared by tag so this header stays
 * self-contained -- cinterop reads it alone, and an incomplete type is all a
 * handle needs. */
typedef struct markdown_core_session markdown_core_kotlin_session;

bool markdown_core_kotlin_parse(
    const uint8_t *source,
    size_t length,
    uint32_t options_mask,
    uint8_t **output,
    size_t *output_length
);

/* THE STREAM (docs/STREAMING.md §4 D5), one bridge call per facade call.
 * `session_new` answers NULL only for the allocation failure the facade can
 * report there, so no payload crosses; `session_feed` and `session_finish`
 * speak exactly `markdown_core_kotlin_parse`'s contract -- false when the
 * payload buffer itself could not be built, otherwise an MKC5 payload carrying
 * the document or the error, the finished-session refusal included. */
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
void markdown_core_kotlin_session_free(markdown_core_kotlin_session *session);

void markdown_core_kotlin_free(uint8_t *output);

#endif
