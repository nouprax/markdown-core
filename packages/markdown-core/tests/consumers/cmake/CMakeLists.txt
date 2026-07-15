cmake_minimum_required(VERSION 3.20)
project(markdown-core-installed-consumer LANGUAGES C)

find_package(markdown-core CONFIG REQUIRED)

add_executable(markdown-core-installed-consumer ../c/main.c)
target_link_libraries(markdown-core-installed-consumer PRIVATE markdown-core::markdown-core)
