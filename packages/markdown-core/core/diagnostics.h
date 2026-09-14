#ifndef MARKDOWN_CORE_DIAGNOSTICS_H
#define MARKDOWN_CORE_DIAGNOSTICS_H

/* Private deterministic work accounting. Product builds omit both storage
 * and evaluation; only the internal diagnostic target enables it. Never put
 * semantic state changes or required side effects in these expressions. */
#ifndef MARKDOWN_CORE_DIAGNOSTICS
#define MARKDOWN_CORE_DIAGNOSTICS 0
#endif
#if MARKDOWN_CORE_DIAGNOSTICS
#define MARKDOWN_CORE_DIAGNOSTIC(...) __VA_ARGS__
#define MARKDOWN_CORE_DIAGNOSTIC_ADDRESS(field) (&(field))
#else
#define MARKDOWN_CORE_DIAGNOSTIC(...)
#define MARKDOWN_CORE_DIAGNOSTIC_ADDRESS(field) NULL
#endif

#endif
