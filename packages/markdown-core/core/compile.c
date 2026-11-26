#include "alloc.h"
#include "compile.h"

void markdown_core_compiler_init(markdown_core_compiler *compiler) { *compiler = (markdown_core_compiler){0}; }

void markdown_core_compiler_free(markdown_core_compiler *compiler) {
    markdown_core_free(compiler->stack);
    *compiler = (markdown_core_compiler){0};
}

bool markdown_core_compile_push(markdown_core_compiler *compiler, void *item) {
    void **stack =
        markdown_core_reserve(compiler->stack, &compiler->stack_room, compiler->stack_count + 1, sizeof(*stack));
    if (!stack) {
        compiler->failed = true;
        return false;
    }
    compiler->stack = stack;
    stack[compiler->stack_count++] = item;
    return true;
}

void *markdown_core_compile_pop(markdown_core_compiler *compiler) {
    return compiler->stack_count ? compiler->stack[--compiler->stack_count] : NULL;
}

bool markdown_core_compile(markdown_core_compiler *compiler, const markdown_core_events *events,
                           const unsigned char *data, const markdown_core_compile_ops *const *ops, size_t kind_count,
                           void *reader) {
    compiler->events = events;
    compiler->data = data;
    compiler->ops = ops;
    compiler->kind_count = kind_count;
    compiler->reader = reader;
    compiler->stack_count = 0;
    compiler->failed = events->failed;
    for (compiler->at = 0; !compiler->failed && compiler->at < events->count; compiler->at++) {
        const markdown_core_event event = events->items[compiler->at];
        const markdown_core_token *token = &events->tokens[event.token];
        const markdown_core_compile_ops *handlers = token->kind < kind_count ? ops[token->kind] : NULL;
        if (!handlers) {
            continue;
        }
        compiler->current = handlers;
        if (event.phase == MARKDOWN_CORE_ENTER) {
            if (handlers->enter) {
                handlers->enter(compiler, token, event.token);
            }
            if (handlers->opaque) {
                while (compiler->at + 1 < events->count && events->items[compiler->at + 1].token != event.token) {
                    compiler->at++;
                }
            }
        } else if (handlers->exit) {
            handlers->exit(compiler, token, event.token);
        }
    }
    compiler->current = NULL;
    return !compiler->failed;
}
