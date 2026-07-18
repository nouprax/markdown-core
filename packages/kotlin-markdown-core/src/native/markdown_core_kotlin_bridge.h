#ifndef MARKDOWN_CORE_KOTLIN_BRIDGE_H
#define MARKDOWN_CORE_KOTLIN_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Every payload-producing call returns false only when the payload itself
 * could not be allocated; structured failures (invalid edit ranges, commit
 * errors) travel inside the MKC3 payload as a status record. Payloads are
 * caller-owned and released with markdown_core_kotlin_free. */

bool markdown_core_kotlin_parse(const uint8_t *source, size_t length, uint32_t options_mask,
                                uint8_t **output, size_t *output_length);
void markdown_core_kotlin_free(uint8_t *output);

typedef struct markdown_core_kotlin_session markdown_core_kotlin_session;

markdown_core_kotlin_session *markdown_core_kotlin_session_open(uint32_t options_mask);
void markdown_core_kotlin_session_free(markdown_core_kotlin_session *session);
uint64_t markdown_core_kotlin_session_lineage(const markdown_core_kotlin_session *session);
uint64_t markdown_core_kotlin_session_revision(const markdown_core_kotlin_session *session);
uint64_t markdown_core_kotlin_session_length(const markdown_core_kotlin_session *session);
uint64_t markdown_core_kotlin_session_root(const markdown_core_kotlin_session *session);
bool markdown_core_kotlin_session_edit(markdown_core_kotlin_session *session, uint64_t byte_start,
                                       uint64_t byte_end, const uint8_t *bytes, size_t length,
                                       uint8_t **output, size_t *output_length);
bool markdown_core_kotlin_session_commit(markdown_core_kotlin_session *session, uint8_t **output,
                                         size_t *output_length);
bool markdown_core_kotlin_session_scopes(const markdown_core_kotlin_session *session,
                                         uint8_t **output, size_t *output_length);
bool markdown_core_kotlin_session_footnote_info(const markdown_core_kotlin_session *session,
                                                uint64_t id, uint8_t **output,
                                                size_t *output_length);
bool markdown_core_kotlin_session_footnotes(const markdown_core_kotlin_session *session,
                                            uint8_t **output, size_t *output_length);
bool markdown_core_kotlin_session_footnote_references(const markdown_core_kotlin_session *session,
                                                      uint64_t definition, uint8_t **output,
                                                      size_t *output_length);

#endif
