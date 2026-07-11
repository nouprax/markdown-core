if(MARKDOWN_CORE_KOTLIN_JNI)
    find_package(JNI REQUIRED)

    add_library(markdown_core_kotlin_jni SHARED markdown_core_kotlin_bridge.c markdown_core_kotlin_jni.c)
    target_include_directories(markdown_core_kotlin_jni PRIVATE ${JNI_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}
                                                                ${CMAKE_SOURCE_DIR}/packages/markdown-core/include)
    target_compile_definitions(markdown_core_kotlin_jni PRIVATE MARKDOWN_CORE_STATIC_DEFINE
                                                                MARKDOWN_CORE_EXTENSIONS_STATIC_DEFINE)
    target_link_libraries(markdown_core_kotlin_jni PRIVATE libmarkdown-core-extensions_static libmarkdown-core_static)
    set_target_properties(
        markdown_core_kotlin_jni
        PROPERTIES OUTPUT_NAME markdown_core_kotlin
                   C_STANDARD 99
                   C_STANDARD_REQUIRED YES
                   C_EXTENSIONS NO
                   C_VISIBILITY_PRESET hidden
                   POSITION_INDEPENDENT_CODE ON)
    if(APPLE)
        target_link_options(markdown_core_kotlin_jni PRIVATE
                            "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/markdown_core_kotlin.exports")
    elseif(WIN32)
        target_sources(markdown_core_kotlin_jni PRIVATE markdown_core_kotlin.def)
    else()
        target_link_options(markdown_core_kotlin_jni PRIVATE
                            "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/markdown_core_kotlin.map")
    endif()
endif()

if(MARKDOWN_CORE_KOTLIN_NATIVE)
    add_library(markdown_core_kotlin_native STATIC markdown_core_kotlin_bridge.c)
    target_include_directories(markdown_core_kotlin_native PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                                                   ${CMAKE_SOURCE_DIR}/packages/markdown-core/include)
    target_compile_definitions(markdown_core_kotlin_native PRIVATE MARKDOWN_CORE_STATIC_DEFINE
                                                                   MARKDOWN_CORE_EXTENSIONS_STATIC_DEFINE)
    add_dependencies(markdown_core_kotlin_native libmarkdown-core-extensions_static libmarkdown-core_static)
    set_target_properties(
        markdown_core_kotlin_native
        PROPERTIES OUTPUT_NAME markdown_core_kotlin
                   C_STANDARD 99
                   C_STANDARD_REQUIRED YES
                   C_EXTENSIONS NO
                   POSITION_INDEPENDENT_CODE ON)
endif()
