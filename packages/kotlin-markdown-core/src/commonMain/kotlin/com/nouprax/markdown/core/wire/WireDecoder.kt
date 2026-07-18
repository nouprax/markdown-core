package com.nouprax.markdown.core

internal object WireDecoder {
    private val magic = byteArrayOf(0x4d, 0x4b, 0x43, 0x33)

    private fun reader(bytes: ByteArray): WireReader {
        val reader = WireReader(bytes)
        magic.forEachIndexed { index, expected ->
            val actual = reader.byte()
            require(actual == expected) {
                "invalid native bridge payload at byte $index: expected ${expected.toUByte()}, got ${actual.toUByte()}"
            }
        }
        when (reader.byte().toInt()) {
            0 -> Unit
            1 -> throw reader.error()
            else -> error("unsupported native bridge status")
        }
        return reader
    }

    /** One-shot parse payload: lineage and root id, the first commit's
     * records, and the eagerly materialized scope table. */
    fun decodeDocument(bytes: ByteArray): Document {
        val reader = reader(bytes)
        val lineage = reader.ulong()
        val rootId = reader.ulong()
        val mirror = HashMap<ULong, Markup>()
        reader.commitBody(lineage, mirror)
        val scopes = reader.scopeTable()
        require(reader.finished) { "trailing bytes after a native parse payload" }
        val root = mirror[rootId]
        if (root is Document) {
            return Document(root.id, root.revision, root.content, ScopeResolver.materialized(scopes))
        }
        // An empty source commits an empty delta: no record names the root,
        // which simply kept its revision-0 empty shape.
        require(mirror.isEmpty()) { "native bridge returned an invalid document tree" }
        val entry = requireNotNull(scopes[rootId]) { "native bridge returned an invalid document tree" }
        return Document(MarkupID(lineage, rootId), entry.revision, emptyList(), ScopeResolver.materialized(scopes))
    }

    /** Edit acknowledgement: magic and status only. */
    fun decodeAck(bytes: ByteArray) {
        require(reader(bytes).finished) { "trailing bytes after a native edit payload" }
    }

    /** Applies one commit payload to [mirror] and returns its delta. Throws
     * before touching the mirror when the native commit reported an error. */
    fun decodeCommit(
        bytes: ByteArray,
        lineage: ULong,
        mirror: MutableMap<ULong, Markup>,
    ): Delta {
        val reader = reader(bytes)
        val changes = reader.commitBody(lineage, mirror)
        require(reader.finished) { "trailing bytes after a native commit payload" }
        return changes
    }

    fun decodeScopes(bytes: ByteArray): Map<ULong, ScopeEntry> {
        val reader = reader(bytes)
        val table = reader.scopeTable()
        require(reader.finished) { "trailing bytes after a native scope payload" }
        return table
    }

    fun decodeFootnoteInfo(
        bytes: ByteArray,
        lineage: ULong,
    ): FootnoteInfo? {
        val reader = reader(bytes)
        if (!reader.boolean()) {
            require(reader.finished) { "trailing bytes after a native footnote payload" }
            return null
        }
        val definition = reader.ulong()
        val number = reader.ulong()
        val referenceOrdinal = reader.ulong()
        val referenceCount = reader.ulong()
        require(reader.finished) { "trailing bytes after a native footnote payload" }
        return FootnoteInfo(
            definition = if (definition == 0UL) null else MarkupID(lineage, definition),
            number = if (number == 0UL) null else number.toInt(),
            referenceOrdinal = if (referenceOrdinal == 0UL) null else referenceOrdinal.toInt(),
            referenceCount = referenceCount.toInt(),
        )
    }

    fun decodeIds(bytes: ByteArray): kotlin.collections.List<ULong> {
        val reader = reader(bytes)
        val count = reader.int()
        require(count >= 0) { "invalid native id count" }
        val ids = immutableList(count) { reader.ulong() }
        require(reader.finished) { "trailing bytes after a native id payload" }
        return ids
    }
}

private fun WireReader.error(): ParseException {
    val code =
        when (int()) {
            1 -> ParseErrorCode.INVALID_ARGUMENT
            2 -> ParseErrorCode.ALLOCATION_FAILED
            else -> ParseErrorCode.INTERNAL
        }
    val message = requiredString()
    val errorScope = if (boolean()) scope() else null
    require(finished) { "invalid native error payload" }
    return ParseException(code, message, errorScope)
}

internal class WireReader(
    private val bytes: ByteArray,
) {
    private var offset = 0
    val finished: Boolean get() = offset == bytes.size

    fun byte(): Byte {
        require(offset < bytes.size) { "truncated native bridge payload" }
        return bytes[offset++]
    }

    fun int(): Int {
        require(offset <= bytes.size - Int.SIZE_BYTES) { "truncated native bridge payload" }
        var value = 0
        repeat(4) { shift -> value = value or ((bytes[offset++].toInt() and 0xff) shl (shift * 8)) }
        return value
    }

    fun long(): Long {
        require(offset <= bytes.size - Long.SIZE_BYTES) { "truncated native bridge payload" }
        var value = 0L
        repeat(8) { shift -> value = value or ((bytes[offset++].toLong() and 0xff) shl (shift * 8)) }
        return value
    }

    fun ulong(): ULong = long().toULong()

    fun string(): String? {
        val size = int()
        if (size == -1) return null
        require(size >= 0 && size <= bytes.size - offset) { "invalid native bridge string" }
        val end = offset + size
        return bytes.decodeToString(offset, end).also { offset = end }
    }

    fun requiredString(): String = requireNotNull(string()) { "missing native field" }

    fun scope(): Scope = Scope(Position(int(), int()), Position(int(), int()))

    fun kind(): WireKind = WireKind.from(byte().toInt() and 0xff)

    fun boolean(): Boolean =
        when (byte().toInt()) {
            0 -> false
            1 -> true
            else -> error("invalid native boolean")
        }

    fun nullableBoolean(): Boolean? =
        when (byte().toInt() and 0xff) {
            0xff -> null
            0 -> false
            1 -> true
            else -> error("invalid native boolean")
        }
}

internal fun WireReader.scopeTable(): Map<ULong, ScopeEntry> {
    val count = int()
    require(count >= 0) { "invalid native scope count" }
    val table = HashMap<ULong, ScopeEntry>(count)
    repeat(count) {
        val id = ulong()
        val revision = ulong()
        table[id] = ScopeEntry(revision, scope())
    }
    return table
}
