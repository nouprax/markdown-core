@file:OptIn(kotlinx.cinterop.ExperimentalForeignApi::class)

package com.nouprax.markdown.core

import cnames.structs.markdown_core_kotlin_session
import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_free
import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_parse
import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_session_commit
import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_session_edit
import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_session_footnote_info
import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_session_footnote_references
import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_session_footnotes
import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_session_free
import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_session_length
import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_session_lineage
import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_session_open
import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_session_revision
import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_session_root
import com.nouprax.markdown.core.internal.nativebridge.markdown_core_kotlin_session_scopes
import kotlinx.cinterop.CPointer
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
            pointer.readBytes(outputLength.value.toInt())
        } finally {
            markdown_core_kotlin_free(pointer)
        }
    }

internal actual fun cParse(
    source: ByteArray,
    options: ParseOptions,
): ByteArray =
    payload("native AST copy failed") { output, outputLength ->
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

internal actual class CSession actual constructor(
    options: ParseOptions,
) {
    private val handle: CPointer<markdown_core_kotlin_session> =
        markdown_core_kotlin_session_open(options.toNativeMask().toUInt())
            ?: throw OutOfMemoryError("native session allocation failed")

    actual fun free(): Unit = markdown_core_kotlin_session_free(handle)

    actual fun lineage(): ULong = markdown_core_kotlin_session_lineage(handle)

    actual fun revision(): ULong = markdown_core_kotlin_session_revision(handle)

    actual fun length(): Long = markdown_core_kotlin_session_length(handle).toLong()

    actual fun rootId(): ULong = markdown_core_kotlin_session_root(handle)

    actual fun edit(
        byteStart: Long,
        byteEnd: Long,
        replacement: ByteArray,
    ): ByteArray =
        payload("native session edit failed") { output, outputLength ->
            if (replacement.isEmpty()) {
                markdown_core_kotlin_session_edit(
                    handle,
                    byteStart.toULong(),
                    byteEnd.toULong(),
                    null,
                    0u,
                    output,
                    outputLength,
                )
            } else {
                replacement.usePinned { pinned ->
                    markdown_core_kotlin_session_edit(
                        handle,
                        byteStart.toULong(),
                        byteEnd.toULong(),
                        pinned.addressOf(0).reinterpret(),
                        replacement.size.toULong(),
                        output,
                        outputLength,
                    )
                }
            }
        }

    actual fun commit(): ByteArray =
        payload("native session commit failed") { output, outputLength ->
            markdown_core_kotlin_session_commit(handle, output, outputLength)
        }

    actual fun scopes(): ByteArray =
        payload("native scope table copy failed") { output, outputLength ->
            markdown_core_kotlin_session_scopes(handle, output, outputLength)
        }

    actual fun footnoteInfo(id: ULong): ByteArray =
        payload("native footnote info copy failed") { output, outputLength ->
            markdown_core_kotlin_session_footnote_info(handle, id, output, outputLength)
        }

    actual fun footnotes(): ByteArray =
        payload("native footnote list copy failed") { output, outputLength ->
            markdown_core_kotlin_session_footnotes(handle, output, outputLength)
        }

    actual fun footnoteReferences(definition: ULong): ByteArray =
        payload("native footnote reference copy failed") { output, outputLength ->
            markdown_core_kotlin_session_footnote_references(handle, definition, output, outputLength)
        }
}
