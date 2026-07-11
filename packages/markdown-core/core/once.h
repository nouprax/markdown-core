#ifndef MARKDOWN_CORE_ONCE_H
#define MARKDOWN_CORE_ONCE_H

#include "markdown-core-export.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Process-level one-time initialization for the C99 baseline across the
 * supported platform matrix: pthread_once on POSIX (macOS, Linux, Android,
 * Emscripten) and InitOnceExecuteOnce on Windows (MSVC and MinGW-w64).
 *
 * Both primitives guarantee that the callback runs exactly once per process
 * and that every caller of markdown_core_once_run observes the callback's
 * writes after returning (the necessary happens-before edge).
 */
#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef INIT_ONCE markdown_core_once;
#define MARKDOWN_CORE_ONCE_INIT INIT_ONCE_STATIC_INIT

#else

#include <pthread.h>

typedef pthread_once_t markdown_core_once;
#define MARKDOWN_CORE_ONCE_INIT PTHREAD_ONCE_INIT

#endif

MARKDOWN_CORE_EXPORT
void markdown_core_once_run(markdown_core_once *once, void (*init)(void));

#ifdef __cplusplus
}
#endif

#endif
