@file:OptIn(kotlinx.cinterop.ExperimentalForeignApi::class, kotlin.experimental.ExperimentalNativeApi::class)

package com.nouprax.markdown.core

import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_append
import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_edit
import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_free
import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_open
import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_release
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
import kotlin.native.ref.createCleaner

private inline fun payload(
    message: String,
    block: (
        kotlinx.cinterop.CValuesRef<CPointerVar<UByteVar>>,
        kotlinx.cinterop.CValuesRef<size_tVar>,
    ) -> Boolean,
): ByteArray =
    memScoped {
        val output = alloc<CPointerVar<UByteVar>>()
        val outputLength = alloc<size_tVar>()
        if (!block(output.ptr, outputLength.ptr)) throw OutOfMemoryError(message)
        val pointer = requireNotNull(output.value)
        try {
            val length = outputLength.value
            if (length > Int.MAX_VALUE.toULong()) {
                throw OutOfMemoryError("native payload exceeds the Kotlin ByteArray limit")
            }
            pointer.readBytes(length.toInt())
        } finally {
            markdown_core_kotlin_free(pointer)
        }
    }

internal actual fun cOpen(
    source: ByteArray,
    options: ParseOptions,
): ByteArray =
    payload("native AST copy failed") { output, outputLength ->
        if (source.isEmpty()) {
            markdown_core_kotlin_open(null, 0u, options.toNativeMask().toUInt(), output, outputLength)
        } else {
            source.usePinned { pinned ->
                markdown_core_kotlin_open(
                    pinned.addressOf(0).reinterpret(),
                    source.size.toULong(),
                    options.toNativeMask().toUInt(),
                    output,
                    outputLength,
                )
            }
        }
    }

internal actual fun CDocumentHandle.edit(source: ByteArray): ByteArray {
    val handle = toULong()
    return payload("native document edit failed") { output, outputLength ->
        if (source.isEmpty()) {
            markdown_core_kotlin_edit(handle, null, 0u, output, outputLength)
        } else {
            source.usePinned { pinned ->
                markdown_core_kotlin_edit(
                    handle,
                    pinned.addressOf(0).reinterpret(),
                    source.size.toULong(),
                    output,
                    outputLength,
                )
            }
        }
    }
}

internal actual fun CDocumentHandle.append(
    chunk: ByteArray,
    baseline: ULong,
): ByteArray {
    val handle = toULong()
    return payload("native document append failed") { output, outputLength ->
        if (chunk.isEmpty()) {
            markdown_core_kotlin_append(handle, null, 0u, baseline, output, outputLength)
        } else {
            chunk.usePinned { pinned ->
                markdown_core_kotlin_append(
                    handle,
                    pinned.addressOf(0).reinterpret(),
                    chunk.size.toULong(),
                    baseline,
                    output,
                    outputLength,
                )
            }
        }
    }
}

internal actual fun CDocumentHandle.release(): Unit = markdown_core_kotlin_release(toULong())

internal actual fun attachNativeCleanup(
    owner: Any,
    release: () -> Unit,
): Any? {
    // Kotlin/Native runs the cleanup when the CLEANER becomes unreachable, so
    // the owner holds it and it holds only `release`. `owner` is deliberately
    // untouched: a cleaner that reached its document would keep that document
    // reachable, and the release would never happen.
    return createCleaner(release) { it() }
}
