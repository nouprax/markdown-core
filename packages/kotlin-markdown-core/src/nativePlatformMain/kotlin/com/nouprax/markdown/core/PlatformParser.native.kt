@file:OptIn(kotlinx.cinterop.ExperimentalForeignApi::class)

package com.nouprax.markdown.core

import com.nouprax.markdown.core.internal.capi.markdown_core_wire_free
import com.nouprax.markdown.core.internal.capi.markdown_core_wire_parse
import kotlinx.cinterop.addressOf
import kotlinx.cinterop.get
import kotlinx.cinterop.readBytes
import kotlinx.cinterop.reinterpret
import kotlinx.cinterop.usePinned

/** The byte offset of the MCB3 message length (docs/architecture/wire-format.md). */
private const val LENGTH_OFFSET = 4

/**
 * Parses through the core's MCB3 encoder and copies its one message into the
 * Kotlin heap; the shared [WireDecoder] builds the tree, as on the JVM. A
 * message the engine could not allocate, or one longer than a byte array can
 * hold, is [ErrorCode.ALLOCATION_FAILED].
 */
internal actual fun parsePlatformDocument(
    source: ByteArray,
    unit: TextUnit,
): Document {
    val message =
        if (source.isEmpty()) {
            markdown_core_wire_parse(null, 0u)
        } else {
            source.usePinned { pinned ->
                markdown_core_wire_parse(pinned.addressOf(0).reinterpret(), source.size.toULong())
            }
        } ?: throw MarkdownCoreException(ErrorCode.ALLOCATION_FAILED)
    val bytes =
        try {
            var length = 0L
            for (index in 0 until Int.SIZE_BYTES) {
                length = length or (message[LENGTH_OFFSET + index].toLong() shl (index * 8))
            }
            if (length > Int.MAX_VALUE) throw MarkdownCoreException(ErrorCode.ALLOCATION_FAILED)
            message.readBytes(length.toInt())
        } finally {
            markdown_core_wire_free(message)
        }
    return WireDecoder.decode(bytes, unit)
}
