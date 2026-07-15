#include "once.h"

#if defined(_WIN32)

/* INIT_ONCE passes the callback through a PVOID parameter.  C99 forbids
 * casting between function and object pointers, so smuggle the function
 * pointer through a union instead. */
typedef union once_callback {
    void (*init)(void);
    PVOID opaque;
} once_callback;

static BOOL CALLBACK once_trampoline(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    once_callback callback;
    (void)once;
    (void)context;
    callback.opaque = parameter;
    callback.init();
    return TRUE;
}

void markdown_core_once_run(markdown_core_once *once, void (*init)(void)) {
    once_callback callback;
    callback.init = init;
    InitOnceExecuteOnce(once, once_trampoline, callback.opaque, NULL);
}

#else

void markdown_core_once_run(markdown_core_once *once, void (*init)(void)) { pthread_once(once, init); }

#endif
