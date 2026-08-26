@file:OptIn(kotlinx.cinterop.ExperimentalForeignApi::class)

package com.nouprax.markdown.core

import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_free
import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_parse
import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_session
import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_session_feed
import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_session_finish
import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_session_free
import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_session_new
import kotlinx.cinterop.CPointer
import kotlinx.cinterop.CPointerVar
import kotlinx.cinterop.UByteVar
import kotlinx.cinterop.addressOf
import kotlinx.cinterop.alloc
import kotlinx.cinterop.memScoped
import kotlinx.cinterop.ptr
import kotlinx.cinterop.readBytes
import kotlinx.cinterop.reinterpret
import kotlinx.cinterop.toCPointer
import kotlinx.cinterop.toLong
import kotlinx.cinterop.usePinned
import kotlinx.cinterop.value
import platform.posix.size_tVar

/**
 * One payload becomes one [ByteArray], whoever produced it: the one-shot
 * parse and a session's feed and finish all read and free their MKC5 bytes
 * through here.
 */
private inline fun payload(invoke: (CPointer<CPointerVar<UByteVar>>, CPointer<size_tVar>) -> Boolean): ByteArray =
    memScoped {
        val output = alloc<CPointerVar<UByteVar>>()
        val outputLength = alloc<size_tVar>()
        if (!invoke(output.ptr, outputLength.ptr)) throw OutOfMemoryError("native AST copy failed")
        val pointer = requireNotNull(output.value)
        try {
            pointer.readBytes(outputLength.value.toInt())
        } finally {
            markdown_core_kotlin_free(pointer)
        }
    }

private fun Long.toSession(): CPointer<markdown_core_kotlin_session> =
    requireNotNull(toCPointer<markdown_core_kotlin_session>())

internal actual fun nativeParse(
    source: ByteArray,
    options: ParseOptions,
): ByteArray =
    payload { output, outputLength ->
        if (source.isEmpty()) {
            markdown_core_kotlin_parse(
                null,
                0u,
                options.toNativeMask().toUInt(),
                output,
                outputLength,
            )
        } else {
            source.usePinned { pinned ->
                markdown_core_kotlin_parse(
                    pinned.addressOf(0).reinterpret(),
                    source.size.toULong(),
                    options.toNativeMask().toUInt(),
                    output,
                    outputLength,
                )
            }
        }
    }

internal actual fun nativeSessionNew(options: ParseOptions): Long {
    val session =
        markdown_core_kotlin_session_new(options.toNativeMask().toUInt())
            ?: throw OutOfMemoryError("native session allocation failed")
    return session.toLong()
}

internal actual fun nativeSessionFeed(
    session: Long,
    chunk: ByteArray,
): ByteArray =
    payload { output, outputLength ->
        if (chunk.isEmpty()) {
            markdown_core_kotlin_session_feed(session.toSession(), null, 0u, output, outputLength)
        } else {
            chunk.usePinned { pinned ->
                markdown_core_kotlin_session_feed(
                    session.toSession(),
                    pinned.addressOf(0).reinterpret(),
                    chunk.size.toULong(),
                    output,
                    outputLength,
                )
            }
        }
    }

internal actual fun nativeSessionFinish(session: Long): ByteArray =
    payload { output, outputLength ->
        markdown_core_kotlin_session_finish(session.toSession(), output, outputLength)
    }

internal actual fun nativeSessionFree(session: Long) {
    markdown_core_kotlin_session_free(session.toSession())
}
