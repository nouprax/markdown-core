#ifndef MARKDOWN_CORE_KOTLIN_BRIDGE_H
#define MARKDOWN_CORE_KOTLIN_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool markdown_core_kotlin_parse(const uint8_t *source, size_t length, uint32_t options_mask,
                                uint8_t **output, size_t *output_length);
void markdown_core_kotlin_free(uint8_t *output);

#endif
