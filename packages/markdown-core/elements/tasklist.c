#include "tasklist.h"
#include "block_internal.h"
#include "parser.h"

static markdown_core_task_found *S_task_of(markdown_core_tokenizer *tokenizer) {
    return MARKDOWN_CORE_BLOCK_FOUND(markdown_core_block_turn_of(tokenizer)->start, markdown_core_task_found);
}

static inline bufsize_t S_offset(const markdown_core_tokenizer *tokenizer) {
    return (bufsize_t)(tokenizer->position - tokenizer->base);
}

static markdown_core_state S_task_pad(markdown_core_tokenizer *tokenizer, int code) {
    if (markdown_core_code_space(code)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_task_pad);
    }
    S_task_of(tokenizer)->end = S_offset(tokenizer);
    return markdown_core_ok(tokenizer);
}

static markdown_core_state S_task_space(markdown_core_tokenizer *tokenizer, int code) {
    if (!markdown_core_code_space(code)) {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_task_pad);
}

static markdown_core_state S_task_close(markdown_core_tokenizer *tokenizer, int code) {
    if (code != ']') {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_task_space);
}

/* The marker is one character, whatever it is: a code, read whole, a
 * tab's columns included. */
static markdown_core_state S_task_rest(markdown_core_tokenizer *tokenizer, int code) {
    if (tokenizer->split) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_task_rest);
    }
    markdown_core_task_found *task = S_task_of(tokenizer);
    task->width = S_offset(tokenizer) - task->at - 1;
    return S_task_close(tokenizer, code);
}

static markdown_core_state S_task_mark(markdown_core_tokenizer *tokenizer, int code) {
    if (markdown_core_code_ending(code)) {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_task_rest);
}

static markdown_core_state S_task(markdown_core_tokenizer *tokenizer, int code) {
    if (markdown_core_code_space(code)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_task);
    }
    if (code != '[') {
        return markdown_core_nok(tokenizer);
    }
    S_task_of(tokenizer)->at = S_offset(tokenizer);
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_task_mark);
}

const markdown_core_construct markdown_core_task_marker = {.start = S_task};

void markdown_core_task_take(markdown_core_parser *parser, markdown_core_node *item, markdown_core_reader *reader,
                             const unsigned char *line, const markdown_core_task_found *task) {
    if (!task->end) {
        return;
    }
    /* Own the marker; allocation failure aborts the parse rather than
     * borrowing the line. */
    markdown_core_chunk marker = {(unsigned char *)line + task->at + 1, task->width, 0};
    if (!markdown_core_chunk_to_cstr(&marker)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return;
    }
    item->as.list->task_marker = markdown_core_optional_chunk_present(marker);
    const markdown_core_field_to fields = {parser, item, NULL, reader, 0, NULL};
    markdown_core_field_add(&fields, MARKDOWN_CORE_SLOT_NONE, task->at + 1, task->at + 1 + task->width);
}
