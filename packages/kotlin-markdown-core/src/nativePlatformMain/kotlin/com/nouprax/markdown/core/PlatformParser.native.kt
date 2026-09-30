@file:OptIn(kotlinx.cinterop.ExperimentalForeignApi::class)

package com.nouprax.markdown.core

import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_TEXT_UNIT_UTF16
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_TEXT_UNIT_UTF8
import com.nouprax.markdown.core.internal.capi.markdown_core_session_free
import com.nouprax.markdown.core.internal.capi.markdown_core_session_text
import com.nouprax.markdown.core.internal.capi.markdown_core_session_text_size
import com.nouprax.markdown.core.internal.capi.markdown_core_wire_free
import com.nouprax.markdown.core.internal.capi.markdown_core_wire_parse
import com.nouprax.markdown.core.internal.capi.markdown_core_wire_session_append
import com.nouprax.markdown.core.internal.capi.markdown_core_wire_session_edit
import com.nouprax.markdown.core.internal.capi.markdown_core_wire_session_new
import kotlinx.cinterop.CPointer
import kotlinx.cinterop.COpaquePointer
import kotlinx.cinterop.COpaquePointerVar
import kotlinx.cinterop.UByteVar
import kotlinx.cinterop.addressOf
import kotlinx.cinterop.alloc
import kotlinx.cinterop.allocArray
import kotlinx.cinterop.convert
import kotlinx.cinterop.get
import kotlinx.cinterop.memScoped
import kotlinx.cinterop.ptr
import kotlinx.cinterop.readBytes
import kotlinx.cinterop.reinterpret
import kotlinx.cinterop.set
import kotlinx.cinterop.usePinned
import kotlinx.cinterop.value
import platform.posix.size_tVar

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
): Document = WireDecoder.decode(copy(source.withAddress { markdown_core_wire_parse(it, source.size.convert()) }), unit)

/**
 * Runs [block] with the address of this array's bytes, pinned for the call;
 * an empty array has none and passes null.
 */
private inline fun <R> ByteArray.withAddress(block: (CPointer<UByteVar>?) -> R): R =
    if (isEmpty()) block(null) else usePinned { block(it.addressOf(0).reinterpret()) }

/**
 * Copies the engine's [message] into the Kotlin heap and releases it. A null
 * message, or one longer than a byte array can hold, is
 * [ErrorCode.ALLOCATION_FAILED].
 */
private fun copy(message: CPointer<UByteVar>?): ByteArray {
    if (message == null) throw MarkdownCoreException(ErrorCode.ALLOCATION_FAILED)
    try {
        var length = 0L
        for (index in 0 until Int.SIZE_BYTES) {
            length = length or (message[LENGTH_OFFSET + index].toLong() shl (index * 8))
        }
        if (length > Int.MAX_VALUE) throw MarkdownCoreException(ErrorCode.ALLOCATION_FAILED)
        return message.readBytes(length.toInt())
    } finally {
        markdown_core_wire_free(message)
    }
}

internal actual fun newPlatformSession(
    source: ByteArray,
    unit: TextUnit,
): PlatformSession = CSession(source, unit)

/** A C session held through cinterop. */
private class CSession(
    source: ByteArray,
    unit: TextUnit,
) : PlatformSession {
    /**
     * The C session, which the engine makes with the first document; null
     * once freed. Each call reinterprets it as the facade's session type.
     */
    private var session: COpaquePointer? = null

    override var document: Document =
        WireDecoder.decode(
            copy(
                memScoped {
                    val address = alloc<COpaquePointerVar>()
                    source
                        .withAddress {
                            markdown_core_wire_session_new(
                                it,
                                source.size.convert(),
                                if (unit == TextUnit.UTF16) MARKDOWN_CORE_TEXT_UNIT_UTF16 else MARKDOWN_CORE_TEXT_UNIT_UTF8,
                                address.ptr.reinterpret(),
                            )
                        }.also { session = address.value }
                },
            ),
            unit,
        )
        private set

    override fun edit(
        fields: IntArray,
        texts: ByteArray,
    ): Document =
        publish(
            memScoped {
                val edits = allocArray<size_tVar>(fields.size)
                for (index in fields.indices) {
                    // A negative offset converts to a size past the end of any text.
                    edits[index] = fields[index].convert()
                }
                texts.withAddress {
                    markdown_core_wire_session_edit(session?.reinterpret(), edits, (fields.size / 3).convert(), it)
                }
            },
        )

    override fun append(text: ByteArray): Document =
        publish(text.withAddress { markdown_core_wire_session_append(session?.reinterpret(), it, text.size.convert()) })

    override fun text(): ByteArray {
        val bytes = ByteArray(markdown_core_session_text_size(session?.reinterpret()).convert<Int>())
        if (bytes.isNotEmpty()) {
            bytes.usePinned { markdown_core_session_text(session?.reinterpret(), it.addressOf(0).reinterpret()) }
        }
        return bytes
    }

    override fun free() {
        markdown_core_session_free(session?.reinterpret())
        session = null
    }

    private fun publish(message: CPointer<UByteVar>?): Document =
        WireDecoder.decode(copy(message), document.unit).also { document = it }
}
