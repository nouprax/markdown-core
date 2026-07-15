#ifndef MARKDOWN_CORE_EXPORT_H
#define MARKDOWN_CORE_EXPORT_H

#ifdef MARKDOWN_CORE_STATIC_DEFINE
#define MARKDOWN_CORE_EXPORT
#define MARKDOWN_CORE_NO_EXPORT
#else
#ifndef MARKDOWN_CORE_EXPORT
#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(MARKDOWN_CORE_EXPORTS)
#define MARKDOWN_CORE_EXPORT __declspec(dllexport)
#else
#define MARKDOWN_CORE_EXPORT __declspec(dllimport)
#endif
#else
#define MARKDOWN_CORE_EXPORT __attribute__((visibility("default")))
#endif
#endif

#ifndef MARKDOWN_CORE_NO_EXPORT
#define MARKDOWN_CORE_NO_EXPORT __attribute__((visibility("hidden")))
#endif
#endif

#ifndef MARKDOWN_CORE_DEPRECATED
#define MARKDOWN_CORE_DEPRECATED __attribute__((__deprecated__))
#endif

#ifndef MARKDOWN_CORE_DEPRECATED_EXPORT
#define MARKDOWN_CORE_DEPRECATED_EXPORT MARKDOWN_CORE_EXPORT MARKDOWN_CORE_DEPRECATED
#endif

#ifndef MARKDOWN_CORE_DEPRECATED_NO_EXPORT
#define MARKDOWN_CORE_DEPRECATED_NO_EXPORT MARKDOWN_CORE_NO_EXPORT MARKDOWN_CORE_DEPRECATED
#endif

#endif
