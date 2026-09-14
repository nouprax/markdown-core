#ifndef MARKDOWN_CORE_KOTLIN_PAYLOAD_H
#define MARKDOWN_CORE_KOTLIN_PAYLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The Kotlin payload encoder: one C call parses `source` and answers the
 * whole document as one flat byte payload that every Kotlin target decodes
 * with the one decoder in `src/payloadMain`. The JVM and Android JNI bridge
 * hands the payload over as a `byte[]`; Kotlin/Native calls this through
 * cinterop and copies the payload out of C once. This header is neither
 * installed nor part of the C facade: it depends only on `markdown_core.h`
 * and holds no JNI.
 *
 * A false result means the payload itself could not be allocated; parser
 * and internal failures are encoded as typed payloads for the Kotlin
 * consumer. */
bool markdown_core_kotlin_payload_encode(const uint8_t *source, size_t length, uint8_t **output, size_t *output_length);
void markdown_core_kotlin_payload_free(uint8_t *output);

#endif
