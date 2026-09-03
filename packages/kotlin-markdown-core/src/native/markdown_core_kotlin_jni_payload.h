#ifndef MARKDOWN_CORE_KOTLIN_JNI_PAYLOAD_H
#define MARKDOWN_CORE_KOTLIN_JNI_PAYLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* JVM/Android-only JNI payload encoder. This header is neither installed nor
 * used by Kotlin/Native or the portable C facade. A false result means the
 * payload itself could not be allocated; parser and internal failures are
 * encoded as typed payloads for the Kotlin consumer. */
bool markdown_core_kotlin_jni_encode(const uint8_t *source, size_t length, uint32_t options_mask, uint8_t **output,
                                     size_t *output_length);
void markdown_core_kotlin_jni_payload_free(uint8_t *output);

#endif
