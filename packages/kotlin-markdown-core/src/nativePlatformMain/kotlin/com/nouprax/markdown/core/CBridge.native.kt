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
import kotlinx.cinterop.toCPointer
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
            val length = outputLength.value
            if (length > Int.MAX_VALUE.toULong()) {
                throw OutOfMemoryError("native payload exceeds the Kotlin ByteArray limit")
            }
            pointer.readBytes(length.toInt())
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

private fun CSessionHandle.nativeSession(): CPointer<markdown_core_kotlin_session> =
    requireNotNull(toCPointer()) { "native session handle is null" }

internal actual fun openCSession(options: ParseOptions): CSessionHandle =
    markdown_core_kotlin_session_open(options.toNativeMask().toUInt())?.rawValue?.toLong()
        ?: throw OutOfMemoryError("native session allocation failed")

internal actual fun CSessionHandle.free(): Unit = markdown_core_kotlin_session_free(nativeSession())

internal actual fun CSessionHandle.lineage(): ULong = markdown_core_kotlin_session_lineage(nativeSession())

internal actual fun CSessionHandle.revision(): ULong = markdown_core_kotlin_session_revision(nativeSession())

internal actual fun CSessionHandle.length(): Long = markdown_core_kotlin_session_length(nativeSession()).toLong()

internal actual fun CSessionHandle.rootId(): ULong = markdown_core_kotlin_session_root(nativeSession())

internal actual fun CSessionHandle.edit(
    byteStart: Long,
    byteEnd: Long,
    replacement: ByteArray,
): ByteArray {
    val handle = nativeSession()
    return payload("native session edit failed") { output, outputLength ->
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
}

internal actual fun CSessionHandle.commit(): ByteArray {
    val handle = nativeSession()
    return payload("native session commit failed") { output, outputLength ->
        markdown_core_kotlin_session_commit(handle, output, outputLength)
    }
}

internal actual fun CSessionHandle.scopes(): ByteArray {
    val handle = nativeSession()
    return payload("native scope table copy failed") { output, outputLength ->
        markdown_core_kotlin_session_scopes(handle, output, outputLength)
    }
}

internal actual fun CSessionHandle.footnoteInfo(id: ULong): ByteArray {
    val handle = nativeSession()
    return payload("native footnote info copy failed") { output, outputLength ->
        markdown_core_kotlin_session_footnote_info(handle, id, output, outputLength)
    }
}

internal actual fun CSessionHandle.footnotes(): ByteArray {
    val handle = nativeSession()
    return payload("native footnote list copy failed") { output, outputLength ->
        markdown_core_kotlin_session_footnotes(handle, output, outputLength)
    }
}

internal actual fun CSessionHandle.footnoteReferences(definition: ULong): ByteArray {
    val handle = nativeSession()
    return payload("native footnote reference copy failed") { output, outputLength ->
        markdown_core_kotlin_session_footnote_references(handle, definition, output, outputLength)
    }
}
