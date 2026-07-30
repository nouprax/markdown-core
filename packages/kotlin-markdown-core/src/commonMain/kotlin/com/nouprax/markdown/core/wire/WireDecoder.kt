@file:kotlin.jvm.JvmName("FootnoteQueriesKt")
@file:kotlin.jvm.JvmMultifileClass

package com.nouprax.markdown.core

import kotlin.jvm.JvmSynthetic

private object WireDecoder {
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
    fun <Entry> decodeDocument(
        bytes: ByteArray,
        scopeEntry: (ULong, Scope) -> Entry,
        materialize: (
            MarkupID,
            ULong,
            kotlin.collections.List<Markup>,
            Map<ULong, Entry>,
        ) -> Document,
    ): Document {
        val reader = reader(bytes)
        val lineage = reader.ulong()
        val rootId = reader.ulong()
        val mirror = HashMap<ULong, Markup>()
        reader.commitBody(lineage, mirror)
        var rootScopeRevision: ULong? = null
        val scopes =
            reader.scopeMap { id, revision, scope ->
                if (id == rootId) {
                    rootScopeRevision = revision
                }
                scopeEntry(revision, scope)
            }
        require(reader.finished) { "trailing bytes after a native parse payload" }
        val root = mirror[rootId]
        if (root is Document) {
            return materialize(
                root.id,
                root.revision,
                root.content,
                scopes,
            )
        }
        // An empty source commits an empty delta: no record names the root,
        // which simply kept its revision-0 empty shape.
        require(mirror.isEmpty()) { "native bridge returned an invalid document tree" }
        return materialize(
            MarkupID(lineage, rootId),
            requireNotNull(rootScopeRevision) { "native bridge returned an invalid document tree" },
            emptyList(),
            scopes,
        )
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
        val delta = reader.commitBody(lineage, mirror)
        require(reader.finished) { "trailing bytes after a native commit payload" }
        return delta
    }

    fun <Entry> decodeScopeMap(
        bytes: ByteArray,
        transform: (ULong, Scope) -> Entry,
    ): Map<ULong, Entry> {
        val reader = reader(bytes)
        val entries =
            reader.scopeMap { _, revision, scope ->
                transform(revision, scope)
            }
        require(reader.finished) { "trailing bytes after a native scope payload" }
        return entries
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

@JvmSynthetic
internal fun <Entry> decodeWireDocument(
    bytes: ByteArray,
    scopeEntry: (ULong, Scope) -> Entry,
    materialize: (
        MarkupID,
        ULong,
        kotlin.collections.List<Markup>,
        Map<ULong, Entry>,
    ) -> Document,
): Document = WireDecoder.decodeDocument(bytes, scopeEntry, materialize)

@JvmSynthetic
internal fun decodeWireAck(bytes: ByteArray) {
    WireDecoder.decodeAck(bytes)
}

@JvmSynthetic
internal fun decodeWireCommit(
    bytes: ByteArray,
    lineage: ULong,
    mirror: MutableMap<ULong, Markup>,
): Delta = WireDecoder.decodeCommit(bytes, lineage, mirror)

@JvmSynthetic
internal fun <Entry> decodeWireScopeMap(
    bytes: ByteArray,
    transform: (ULong, Scope) -> Entry,
): Map<ULong, Entry> = WireDecoder.decodeScopeMap(bytes, transform)

@JvmSynthetic
internal fun decodeWireFootnoteInfo(
    bytes: ByteArray,
    lineage: ULong,
): FootnoteInfo? = WireDecoder.decodeFootnoteInfo(bytes, lineage)

@JvmSynthetic
internal fun decodeWireIds(bytes: ByteArray): kotlin.collections.List<ULong> = WireDecoder.decodeIds(bytes)

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

private class WireReader(
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

    fun kind(): Int = byte().toInt() and 0xff

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

private fun <Entry> WireReader.scopeMap(transform: (ULong, ULong, Scope) -> Entry): Map<ULong, Entry> {
    val count = int()
    require(count >= 0) { "invalid native scope count" }
    return buildMap(capacity = count) {
        repeat(count) {
            val id = ulong()
            put(id, transform(id, ulong(), scope()))
        }
    }
}

/**
 * Applies one MKC3 commit body to [mirror] and returns its delta.
 *
 * The body lists removed ids and then full records for added, changed, and
 * bubbled nodes ordered children-before-parents, so every record's child ids
 * resolve against already-decoded mirror entries in one pass. Unchanged
 * children keep their exact platform object across snapshots.
 */
private fun WireReader.commitBody(
    lineage: ULong,
    mirror: MutableMap<ULong, Markup>,
): Delta {
    val beforeRevision = ulong()
    val afterRevision = ulong()

    val removedCount = int()
    require(removedCount >= 0) { "invalid native removed count" }
    val removed = immutableList(removedCount) { MarkupID(lineage, ulong()) }
    removed.forEach { mirror.remove(it.rawValue) }

    val recordCount = int()
    require(recordCount >= 0) { "invalid native record count" }
    val added = ArrayList<MarkupID>()
    val changed = ArrayList<MarkupID>()
    val bubbled = ArrayList<MarkupID>()
    repeat(recordCount) {
        val verdict = byte().toInt()
        val node = record(lineage, mirror)
        mirror[node.id.rawValue] = node
        when (verdict) {
            0 -> added += node.id
            1 -> changed += node.id
            2 -> bubbled += node.id
            else -> error("invalid native delta verdict $verdict")
        }
    }
    return Delta(
        beforeRevision,
        afterRevision,
        immutableList(added.size) { added[it] },
        removed,
        immutableList(changed.size) { changed[it] },
        immutableList(bubbled.size) { bubbled[it] },
    )
}

private fun WireReader.record(
    lineage: ULong,
    mirror: Map<ULong, Markup>,
): Markup {
    val kind = kind()
    val id = MarkupID(lineage, ulong())
    val revision = ulong()
    return when (kind) {
        WireKind.DOCUMENT -> {
            Document.unresolved(id, revision, children(mirror))
        }

        WireKind.BLOCK_QUOTE -> {
            BlockQuote(id, revision, children(mirror))
        }

        WireKind.PARAGRAPH -> {
            Paragraph(id, revision, children(mirror))
        }

        WireKind.HEADING -> {
            Heading(id, revision, int(), children(mirror))
        }

        WireKind.THEMATIC_BREAK -> {
            ThematicBreak(id, revision)
        }

        WireKind.LIST -> {
            readList(id, revision, mirror)
        }

        WireKind.LIST_ITEM -> {
            ListItem(id, revision, nullableBoolean(), children(mirror))
        }

        WireKind.CODE_BLOCK -> {
            CodeBlock(
                id,
                revision,
                PlacementMode.STANDALONE,
                string(),
                string(),
                requiredString(),
                boolean(),
                boolean(),
            )
        }

        WireKind.HTML_BLOCK -> {
            HTMLBlock(id, revision, requiredString())
        }

        WireKind.FORMULA_BLOCK -> {
            FormulaBlock(id, revision, placement(), requiredString())
        }

        WireKind.TABLE -> {
            readTable(id, revision, mirror)
        }

        WireKind.TABLE_ROW -> {
            readTableRow(id, revision, mirror)
        }

        WireKind.TABLE_CELL -> {
            TableCell(id, revision, children(mirror))
        }

        WireKind.DIRECTIVE_BLOCK -> {
            readDirectiveBlock(id, revision, mirror)
        }

        WireKind.DIRECTIVE_LABEL -> {
            DirectiveLabel(id, revision, children(mirror))
        }

        WireKind.FOOTNOTE_DEFINITION -> {
            FootnoteDefinition(id, revision, requiredString(), children(mirror))
        }

        WireKind.TEXT -> {
            Text(id, revision, requiredString())
        }

        WireKind.SOFT_BREAK -> {
            SoftBreak(id, revision)
        }

        WireKind.LINE_BREAK -> {
            LineBreak(id, revision)
        }

        WireKind.CODE -> {
            Code(id, revision, PlacementMode.EMBEDDED, requiredString())
        }

        WireKind.HTML -> {
            HTML(id, revision, requiredString())
        }

        WireKind.FORMULA -> {
            Formula(id, revision, placement(), requiredString())
        }

        WireKind.EMPHASIS -> {
            Emphasis(id, revision, children(mirror))
        }

        WireKind.STRONG -> {
            Strong(id, revision, children(mirror))
        }

        WireKind.STRIKETHROUGH -> {
            Strikethrough(id, revision, children(mirror))
        }

        WireKind.LINK -> {
            Link(id, revision, string(), string(), children(mirror))
        }

        WireKind.IMAGE -> {
            Image(id, revision, string(), string(), children(mirror))
        }

        WireKind.DIRECTIVE -> {
            readDirective(id, revision, mirror)
        }

        WireKind.FOOTNOTE_REFERENCE -> {
            FootnoteReference(id, revision, requiredString())
        }

        WireKind.CROSS_LINK -> {
            CrossLink(id, revision, requiredString())
        }

        WireKind.EMBED -> {
            Embed(id, revision, requiredString())
        }

        else -> {
            error("unsupported native node kind $kind")
        }
    }
}

private fun WireReader.placement(): PlacementMode =
    when (val rawValue = int()) {
        1 -> PlacementMode.EMBEDDED
        2 -> PlacementMode.STANDALONE
        else -> error("invalid native placement mode $rawValue")
    }

private fun WireReader.child(mirror: Map<ULong, Markup>): Markup {
    val rawValue = ulong()
    return mirror[rawValue] ?: error("native record referenced an undecoded child")
}

private fun WireReader.children(mirror: Map<ULong, Markup>): kotlin.collections.List<Markup> {
    val count = int()
    require(count >= 0) { "invalid native child count" }
    return immutableList(count) { child(mirror) }
}

private fun WireReader.readList(
    id: MarkupID,
    revision: ULong,
    mirror: Map<ULong, Markup>,
): List {
    val flavor =
        when (val rawValue = int()) {
            1 -> ListFlavor.BULLET
            2 -> ListFlavor.ORDERED
            else -> error("invalid native list flavor $rawValue")
        }
    val startValue = long()
    val start = if (boolean()) startValue else null
    val tight = boolean()
    val itemCount = int()
    require(itemCount >= 0) { "invalid native list item count" }
    val items =
        immutableList(itemCount) {
            val item = child(mirror)
            require(item is ListItem) { "list contains a non-item node" }
            item
        }
    return List(id, revision, flavor, start, tight, items)
}

private fun WireReader.readDirective(
    id: MarkupID,
    revision: ULong,
    mirror: Map<ULong, Markup>,
): Directive {
    val mode = placement()
    val name = requiredString()
    val attributes = string()
    val directChildren = children(mirror)
    require(directChildren.size <= 1 && directChildren.all { it is DirectiveLabel }) {
        "inline directive contains a non-label child"
    }
    val label = directChildren.firstOrNull() as DirectiveLabel?
    return Directive(id, revision, mode, name, attributes, label)
}

private fun WireReader.readDirectiveBlock(
    id: MarkupID,
    revision: ULong,
    mirror: Map<ULong, Markup>,
): DirectiveBlock {
    val mode = placement()
    val name = requiredString()
    val attributes = string()
    val directChildren = children(mirror)
    val label = directChildren.firstOrNull() as? DirectiveLabel
    val contentStart = if (label == null) 0 else 1
    for (index in contentStart..<directChildren.size) {
        require(directChildren[index] !is DirectiveLabel) {
            "block directive contains a misplaced label"
        }
    }
    val content = immutableList(directChildren.size - contentStart) { directChildren[it + contentStart] }
    return DirectiveBlock(id, revision, mode, name, attributes, label, content)
}

private fun WireReader.readTable(
    id: MarkupID,
    revision: ULong,
    mirror: Map<ULong, Markup>,
): Table {
    val alignmentCount = int()
    require(alignmentCount >= 0) { "invalid native table alignment count" }
    val alignments = immutableList(alignmentCount) { tableAlignment(byte().toInt() and 0xff) }
    val rowCount = int()
    require(rowCount >= 0) { "invalid native table row count" }
    val rows =
        immutableList(rowCount) {
            val row = child(mirror)
            require(row is TableRow) { "table contains a non-row node" }
            row
        }
    var headerIndex = -1
    rows.forEachIndexed { index, row ->
        if (row.isHeader) {
            require(headerIndex == -1) { "table has multiple header rows" }
            headerIndex = index
        }
    }
    require(headerIndex >= 0) { "table has no header" }
    return Table(
        id,
        revision,
        alignments,
        rows[headerIndex],
        rows
            .filterIndexed { index, _ -> index != headerIndex }
            .immutableMap { it },
    )
}

private fun WireReader.readTableRow(
    id: MarkupID,
    revision: ULong,
    mirror: Map<ULong, Markup>,
): TableRow {
    val header = boolean()
    val cellCount = int()
    require(cellCount >= 0) { "invalid native table cell count" }
    val cells =
        immutableList(cellCount) {
            val cell = child(mirror)
            require(cell is TableCell) { "table row contains a non-cell node" }
            cell
        }
    return TableRow(id, revision, header, cells)
}

private fun tableAlignment(rawValue: Int): TableAlignment =
    when (rawValue) {
        0 -> TableAlignment.NONE
        1 -> TableAlignment.LEFT
        2 -> TableAlignment.CENTER
        3 -> TableAlignment.RIGHT
        else -> error("invalid native table alignment $rawValue")
    }

private object WireKind {
    const val DOCUMENT = 1
    const val BLOCK_QUOTE = 2
    const val PARAGRAPH = 3
    const val HEADING = 4
    const val THEMATIC_BREAK = 5
    const val LIST = 6
    const val LIST_ITEM = 7
    const val CODE_BLOCK = 8
    const val HTML_BLOCK = 9
    const val FORMULA_BLOCK = 10
    const val TABLE = 11
    const val TABLE_ROW = 12
    const val TABLE_CELL = 13
    const val DIRECTIVE_BLOCK = 14
    const val DIRECTIVE_LABEL = 15
    const val FOOTNOTE_DEFINITION = 16
    const val TEXT = 17
    const val SOFT_BREAK = 18
    const val LINE_BREAK = 19
    const val CODE = 20
    const val HTML = 21
    const val FORMULA = 22
    const val EMPHASIS = 23
    const val STRONG = 24
    const val STRIKETHROUGH = 25
    const val LINK = 26
    const val IMAGE = 27
    const val DIRECTIVE = 28
    const val FOOTNOTE_REFERENCE = 29
    const val CROSS_LINK = 30
    const val EMBED = 31
}
