cmake_minimum_required(VERSION 3.20)
project(markdown-core VERSION 1.0.0)

enable_testing()
add_subdirectory(packages/markdown-core)
if(MARKDOWN_CORE_KOTLIN_JNI OR MARKDOWN_CORE_KOTLIN_NATIVE)
    add_subdirectory(packages/kotlin-markdown-core/src/native)
endif()
