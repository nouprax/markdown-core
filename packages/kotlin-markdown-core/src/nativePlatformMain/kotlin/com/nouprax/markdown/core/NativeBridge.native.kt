@file:OptIn(kotlinx.cinterop.ExperimentalForeignApi::class)

package com.nouprax.markdown.core

import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_free
import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_parse
import kotlinx.cinterop.CPointerVar
import kotlinx.cinterop.UByteVar
import kotlinx.cinterop.addressOf
import kotlinx.cinterop.alloc
import kotlinx.cinterop.memScoped
import kotlinx.cinterop.ptr
import kotlinx.cinterop.readBytes
import kotlinx.cinterop.reinterpret
import kotlinx.cinterop.usePinned
import kotlinx.cinterop.value
import platform.posix.size_tVar

internal actual fun nativeParse(
    source: ByteArray,
    options: ParseOptions,
): ByteArray =
    memScoped {
        val output = alloc<CPointerVar<UByteVar>>()
        val outputLength = alloc<size_tVar>()
        val succeeded =
            if (source.isEmpty()) {
                markdown_core_kotlin_parse(
                    null,
                    0u,
                    options.toNativeMask().toUInt(),
                    output.ptr,
                    outputLength.ptr,
                )
            } else {
                source.usePinned { pinned ->
                    markdown_core_kotlin_parse(
                        pinned.addressOf(0).reinterpret(),
                        source.size.toULong(),
                        options.toNativeMask().toUInt(),
                        output.ptr,
                        outputLength.ptr,
                    )
                }
            }
        if (!succeeded) throw OutOfMemoryError("native AST copy failed")
        val pointer = requireNotNull(output.value)
        try {
            pointer.readBytes(outputLength.value.toInt())
        } finally {
            markdown_core_kotlin_free(pointer)
        }
    }
