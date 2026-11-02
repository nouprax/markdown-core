@file:OptIn(kotlinx.cinterop.ExperimentalForeignApi::class)

package com.nouprax.markdown.core

import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_TEXT_UNIT_UTF16
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_TEXT_UNIT_UTF8
import com.nouprax.markdown.core.internal.capi.markdown_core_lsp_capabilities
import com.nouprax.markdown.core.internal.capi.markdown_core_lsp_close
import com.nouprax.markdown.core.internal.capi.markdown_core_lsp_complete
import com.nouprax.markdown.core.internal.capi.markdown_core_lsp_edit
import com.nouprax.markdown.core.internal.capi.markdown_core_lsp_initialize
import com.nouprax.markdown.core.internal.capi.markdown_core_lsp_list_count
import com.nouprax.markdown.core.internal.capi.markdown_core_lsp_list_free
import com.nouprax.markdown.core.internal.capi.markdown_core_lsp_list_version
import com.nouprax.markdown.core.internal.capi.markdown_core_lsp_open
import com.nouprax.markdown.core.internal.capi.markdown_core_lsp_position
import com.nouprax.markdown.core.internal.capi.markdown_core_lsp_server_free
import com.nouprax.markdown.core.internal.capi.markdown_core_lsp_server_new
import com.nouprax.markdown.core.internal.capi.markdown_core_lsp_server_phase
import com.nouprax.markdown.core.internal.capi.markdown_core_lsp_shutdown
import com.nouprax.markdown.core.internal.capi.markdown_core_lsp_update
import com.nouprax.markdown.core.internal.capi.markdown_core_position
import com.nouprax.markdown.core.internal.capi.markdown_core_session_close
import com.nouprax.markdown.core.internal.capi.markdown_core_session_text
import com.nouprax.markdown.core.internal.capi.markdown_core_session_text_size
import com.nouprax.markdown.core.internal.capi.markdown_core_wire_dialect
import com.nouprax.markdown.core.internal.capi.markdown_core_wire_field
import com.nouprax.markdown.core.internal.capi.markdown_core_wire_free
import com.nouprax.markdown.core.internal.capi.markdown_core_wire_locate
import com.nouprax.markdown.core.internal.capi.markdown_core_wire_markers
import com.nouprax.markdown.core.internal.capi.markdown_core_wire_parse
import com.nouprax.markdown.core.internal.capi.markdown_core_wire_session_append
import com.nouprax.markdown.core.internal.capi.markdown_core_wire_session_edit
import com.nouprax.markdown.core.internal.capi.markdown_core_wire_session_flush
import com.nouprax.markdown.core.internal.capi.markdown_core_wire_session_new
import com.nouprax.markdown.core.internal.capi.markdown_core_wire_spell
import kotlinx.cinterop.COpaquePointer
import kotlinx.cinterop.COpaquePointerVar
import kotlinx.cinterop.CPointer
import kotlinx.cinterop.UByteVar
import kotlinx.cinterop.UIntVar
import kotlinx.cinterop.addressOf
import kotlinx.cinterop.alloc
import kotlinx.cinterop.allocArray
import kotlinx.cinterop.cValue
import kotlinx.cinterop.convert
import kotlinx.cinterop.get
import kotlinx.cinterop.memScoped
import kotlinx.cinterop.plus
import kotlinx.cinterop.ptr
import kotlinx.cinterop.readBytes
import kotlinx.cinterop.reinterpret
import kotlinx.cinterop.set
import kotlinx.cinterop.toLong
import kotlinx.cinterop.usePinned
import kotlinx.cinterop.value

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

internal actual fun platformDialect(): ByteArray = copy(markdown_core_wire_dialect())

/** The engine's position at [line] and [column]. */
private fun position(
    line: Int,
    column: Int,
) = cValue<markdown_core_position> {
    this.line = line
    this.column = column
}

/** The engine's code for this unit. */
private val TextUnit.code
    get() = if (this == TextUnit.UTF16) MARKDOWN_CORE_TEXT_UNIT_UTF16 else MARKDOWN_CORE_TEXT_UNIT_UTF8

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
                                unit.code,
                                address.ptr.reinterpret(),
                            )
                        }.also { session = address.value }
                },
            ),
            unit,
        )
        private set

    override fun append(text: ByteArray): Document =
        publish(text.withAddress { markdown_core_wire_session_append(session?.reinterpret(), it, text.size.convert()) })

    override fun end(): Document = publish(markdown_core_wire_session_flush(session?.reinterpret()))

    override fun edit(
        startLine: Int,
        startColumn: Int,
        endLine: Int,
        endColumn: Int,
        text: ByteArray,
    ): Document =
        publish(
            text.withAddress {
                markdown_core_wire_session_edit(
                    session?.reinterpret(),
                    position(startLine, startColumn),
                    position(endLine, endColumn),
                    it,
                    text.size.convert(),
                )
            },
        )

    override fun text(): ByteArray {
        val bytes = ByteArray(markdown_core_session_text_size(session?.reinterpret()).convert<Int>())
        if (bytes.isNotEmpty()) {
            bytes.usePinned { markdown_core_session_text(session?.reinterpret(), it.addressOf(0).reinterpret()) }
        }
        return bytes
    }

    override fun free() {
        markdown_core_session_close(session?.reinterpret())
        session = null
    }

    override fun locate(
        line: Int,
        column: Int,
    ): ByteArray = copy(markdown_core_wire_locate(session?.reinterpret(), position(line, column)))

    override fun spell(
        slot: Int,
        value: ByteArray,
    ): ByteArray =
        copy(
            value.withAddress {
                markdown_core_wire_spell(session?.reinterpret(), slot.convert(), it, value.size.convert())
            },
        )

    override fun field(
        id: Long,
        line: Int,
        column: Int,
        slot: Int,
    ): ByteArray =
        copy(markdown_core_wire_field(session?.reinterpret(), id.convert(), position(line, column), slot.convert()))

    override fun markers(
        id: Long,
        line: Int,
        column: Int,
    ): ByteArray = copy(markdown_core_wire_markers(session?.reinterpret(), id.convert(), position(line, column)))

    private fun publish(message: CPointer<UByteVar>?): Document =
        WireDecoder.decode(copy(message), document.unit).also { document = it }
}

internal actual fun newPlatformServer(): PlatformServer? =
    memScoped {
        val address = alloc<COpaquePointerVar>()
        if (markdown_core_lsp_server_new(address.ptr.reinterpret()).convert<Int>() == 0) {
            CServer(address.value)
        } else {
            null
        }
    }

/** The engine's protocol position at [line] and [character], read as unsigned. */
private fun place(
    line: Int,
    character: Int,
) = cValue<markdown_core_lsp_position> {
    this.line = line.toUInt()
    this.character = character.toUInt()
}

/** A C language server held through cinterop; null once freed. */
private class CServer(
    private var server: COpaquePointer?,
) : PlatformServer {
    override fun phase(): Int = markdown_core_lsp_server_phase(server?.reinterpret()).convert()

    override fun initialize(
        units: BooleanArray,
        answer: IntArray,
    ): ByteArray =
        memScoped {
            val codes = allocArray<UIntVar>(units.size.coerceAtLeast(1))
            for (index in units.indices) {
                codes[index] = (if (units[index]) TextUnit.UTF16 else TextUnit.UTF8).code.convert()
            }
            val capabilities = alloc<markdown_core_lsp_capabilities>()
            val status =
                markdown_core_lsp_initialize(
                    server?.reinterpret(),
                    codes.reinterpret(),
                    units.size.convert(),
                    capabilities.ptr,
                ).convert<Int>()
            answer[0] = status
            if (status != 0) return@memScoped ByteArray(0)
            answer[1] = if (capabilities.unit == MARKDOWN_CORE_TEXT_UNIT_UTF16) 1 else 0
            val cues = capabilities.cues
            cues.data?.readBytes(cues.length.convert()) ?: ByteArray(0)
        }

    override fun shutdown(): Int = markdown_core_lsp_shutdown(server?.reinterpret()).convert()

    override fun open(
        uri: ByteArray,
        version: Int,
        text: ByteArray,
    ): Int =
        uri.withAddress { name ->
            text.withAddress { bytes ->
                markdown_core_lsp_open(
                    server?.reinterpret(),
                    name,
                    uri.size.convert(),
                    version,
                    bytes,
                    text.size.convert(),
                ).convert()
            }
        }

    override fun update(
        uri: ByteArray,
        version: Int,
        scopes: IntArray,
        text: ByteArray,
        ends: IntArray,
    ): Int =
        memScoped {
            val edits = allocArray<markdown_core_lsp_edit>(ends.size.coerceAtLeast(1))
            uri.withAddress { name ->
                text.withAddress { bytes ->
                    var start = 0
                    for (index in ends.indices) {
                        val edit = edits[index]
                        edit.scope.start.line = scopes[index * 4].toUInt()
                        edit.scope.start.character = scopes[index * 4 + 1].toUInt()
                        edit.scope.end.line = scopes[index * 4 + 2].toUInt()
                        edit.scope.end.character = scopes[index * 4 + 3].toUInt()
                        edit.text = bytes?.plus(start)
                        edit.size = (ends[index] - start).convert()
                        start = ends[index]
                    }
                    markdown_core_lsp_update(
                        server?.reinterpret(),
                        name,
                        uri.size.convert(),
                        version,
                        edits,
                        ends.size.convert(),
                    ).convert()
                }
            }
        }

    override fun close(uri: ByteArray): Int =
        uri.withAddress { markdown_core_lsp_close(server?.reinterpret(), it, uri.size.convert()).convert() }

    override fun complete(
        uri: ByteArray,
        line: Int,
        character: Int,
        answer: LongArray,
    ): Int =
        memScoped {
            val list = alloc<COpaquePointerVar>()
            val status =
                uri
                    .withAddress {
                        markdown_core_lsp_complete(
                            server?.reinterpret(),
                            it,
                            uri.size.convert(),
                            place(line, character),
                            list.ptr.reinterpret(),
                        )
                    }.convert<Int>()
            if (status == 0) {
                answer[0] = markdown_core_lsp_list_version(list.value?.reinterpret()).toLong()
                answer[1] = markdown_core_lsp_list_count(list.value?.reinterpret()).convert()
                markdown_core_lsp_list_free(list.value?.reinterpret())
            }
            status
        }

    override fun free() {
        markdown_core_lsp_server_free(server?.reinterpret())
        server = null
    }
}
