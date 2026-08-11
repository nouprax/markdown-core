@file:kotlin.jvm.JvmName("MarkdownCoreKt")
@file:kotlin.jvm.JvmMultifileClass

package com.nouprax.markdown.core

import kotlin.jvm.JvmSynthetic

private val magic = byteArrayOf(0x4d, 0x4b, 0x43, 0x34)

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

/**
 * Decodes one payload: the whole tree, then — for an edit — the delta.
 *
 * Every record's child ids resolve against entries this same pass decoded,
 * because the tree arrives children-before-parents. Nothing is carried over
 * from a previous document: a document's projection is a function of its text
 * and its options, not of how the caller reached it.
 *
 * The decoded pieces leave through [build] rather than in a payload type of
 * this file's own, so no wire type crosses a file boundary — which is what
 * keeps the whole wire layer out of the Java-visible surface.
 */
private fun <Result> decode(
    bytes: ByteArray,
    edit: Boolean,
    build: (
        handle: Long,
        id: MarkupID,
        revision: ULong,
        scope: Scope,
        content: kotlin.collections.List<Markup>,
        index: Map<ULong, Markup>,
        diagnostics: kotlin.collections.List<Diagnostic>,
    ) -> Result,
): Pair<Result, Delta?> {
    val reader = reader(bytes)
    val handle = reader.long()
    val series = reader.ulong()
    val rootId = reader.ulong()
    val revision = reader.ulong()
    val scope = reader.scope()
    val index = HashMap<ULong, Markup>()
    val root = RootSink()
    reader.treeBody(series, rootId, index, root)
    val delta = if (edit) reader.deltaBody(series) else null
    val diagnostics = reader.diagnostics()
    require(reader.finished) { "trailing bytes after a native payload" }
    return build(
        handle,
        MarkupID(series, rootId),
        revision,
        scope,
        checkNotNull(root.content) { "a payload carried no document root" },
        index,
        diagnostics,
    ) to delta
}

@JvmSynthetic
internal fun <Result> decodeWireOpen(
    bytes: ByteArray,
    build: (
        Long,
        MarkupID,
        ULong,
        Scope,
        kotlin.collections.List<Markup>,
        Map<ULong, Markup>,
        kotlin.collections.List<Diagnostic>,
    ) -> Result,
): Result = decode(bytes, edit = false, build).first

@JvmSynthetic
internal fun <Result> decodeWireEdit(
    bytes: ByteArray,
    build: (
        Long,
        MarkupID,
        ULong,
        Scope,
        kotlin.collections.List<Markup>,
        Map<ULong, Markup>,
        kotlin.collections.List<Diagnostic>,
    ) -> Result,
): Pair<Result, Delta> {
    val (result, delta) = decode(bytes, edit = true, build)
    return result to checkNotNull(delta) { "an edit payload carried no delta" }
}

/** Catches the one record that is not an index entry. The root has no parent
 * to resolve it as a child, and it is a `Document` on the Kotlin side —
 * which owns a native parse and cannot be minted from a record. */
private class RootSink {
    var content: kotlin.collections.List<Markup>? = null
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

    /** The form a reference was written in, as the bridge encodes it. */
    fun referenceForm(): ReferenceForm =
        when (val raw = byte().toInt()) {
            0 -> ReferenceForm.FULL
            1 -> ReferenceForm.COLLAPSED
            2 -> ReferenceForm.SHORTCUT
            else -> error("unknown reference form $raw")
        }

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

private fun <Entry> WireReader.scopeMap(transform: (ULong, Scope) -> Entry): Map<ULong, Entry> {
    val count = int()
    require(count >= 0) { "invalid native scope count" }
    return buildMap(capacity = count) {
        repeat(count) { put(ulong(), transform(ulong(), scope())) }
    }
}

private fun WireReader.diagnostics(): kotlin.collections.List<Diagnostic> {
    val count = int()
    require(count >= 0) { "invalid native diagnostic count" }
    return immutableList(count) { Diagnostic(diagnosticCode(int()), scope()) }
}

private fun diagnosticCode(rawValue: Int): DiagnosticCode =
    when (rawValue) {
        1 -> DiagnosticCode.DIRECTIVE_ATTRIBUTES
        else -> error("unknown native diagnostic code $rawValue")
    }

/** Reads the whole tree. The root record is diverted to [root] rather than
 * stored: nothing resolves it as a child, and on this side it is a [Document],
 * which owns a native parse and cannot be minted from a record. */
private fun WireReader.treeBody(
    series: ULong,
    rootId: ULong,
    index: MutableMap<ULong, Markup>,
    root: RootSink,
) {
    val count = int()
    require(count >= 0) { "invalid native record count" }
    repeat(count) {
        val kind = kind()
        val id = MarkupID(series, ulong())
        val revision = ulong()
        val scope = scope()
        if (kind == WireKind.DOCUMENT) {
            require(id.rawValue == rootId) { "native payload carried a nested document record" }
            root.content = children(index)
            return@repeat
        }
        index[id.rawValue] = record(kind, id, revision, scope, index)
    }
    requireNotNull(root.content) { "native payload carried no document root" }
}

/** One (id, parts) row per differing node, in the order the facade defines.
 * The rows name what changed; they do not carry it, because the tree above
 * carries everything. */
private fun WireReader.deltaBody(series: ULong): Delta {
    val beforeRevision = ulong()
    val afterRevision = ulong()
    val count = int()
    require(count >= 0) { "invalid native diff count" }
    return Delta(
        beforeRevision,
        afterRevision,
        immutableList(count) { Diff(MarkupID(series, ulong()), DiffParts(int())) },
    )
}

private fun WireReader.record(
    kind: Int,
    id: MarkupID,
    revision: ULong,
    scope: Scope,
    mirror: Map<ULong, Markup>,
): Markup =
    when (kind) {
        WireKind.BLOCK_QUOTE -> {
            BlockQuote(id, revision, scope, children(mirror))
        }

        WireKind.PARAGRAPH -> {
            Paragraph(id, revision, scope, children(mirror))
        }

        WireKind.HEADING -> {
            Heading(id, revision, scope, int(), children(mirror))
        }

        WireKind.THEMATIC_BREAK -> {
            ThematicBreak(id, revision, scope)
        }

        WireKind.LIST -> {
            readList(id, revision, scope, mirror)
        }

        WireKind.LIST_ITEM -> {
            ListItem(id, revision, scope, nullableBoolean(), children(mirror))
        }

        WireKind.CODE_BLOCK -> {
            CodeBlock(
                id,
                revision,
                scope,
                PlacementMode.STANDALONE,
                string(),
                string(),
                requiredString(),
                boolean(),
                boolean(),
            )
        }

        WireKind.HTML_BLOCK -> {
            HTMLBlock(id, revision, scope, boolean(), requiredString())
        }

        WireKind.FORMULA_BLOCK -> {
            FormulaBlock(id, revision, scope, placement(), requiredString())
        }

        WireKind.TABLE -> {
            readTable(id, revision, scope, mirror)
        }

        WireKind.TABLE_ROW -> {
            readTableRow(id, revision, scope, mirror)
        }

        WireKind.TABLE_CELL -> {
            TableCell(id, revision, scope, children(mirror))
        }

        WireKind.DIRECTIVE_BLOCK -> {
            readDirectiveBlock(id, revision, scope, mirror)
        }

        WireKind.DIRECTIVE_LABEL -> {
            DirectiveLabel(id, revision, scope, children(mirror))
        }

        WireKind.FOOTNOTE_DEFINITION -> {
            FootnoteDefinition(id, revision, scope, requiredString(), children(mirror))
        }

        WireKind.TEXT -> {
            Text(id, revision, scope, requiredString())
        }

        WireKind.SOFT_BREAK -> {
            SoftBreak(id, revision, scope)
        }

        WireKind.LINE_BREAK -> {
            LineBreak(id, revision, scope)
        }

        WireKind.CODE -> {
            Code(id, revision, scope, PlacementMode.EMBEDDED, requiredString())
        }

        WireKind.HTML -> {
            HTML(id, revision, scope, boolean(), requiredString())
        }

        WireKind.FORMULA -> {
            Formula(id, revision, scope, placement(), requiredString())
        }

        WireKind.EMPHASIS -> {
            Emphasis(id, revision, scope, children(mirror))
        }

        WireKind.STRONG -> {
            Strong(id, revision, scope, children(mirror))
        }

        WireKind.STRIKETHROUGH -> {
            Strikethrough(id, revision, scope, children(mirror))
        }

        WireKind.LINK -> {
            Link(id, revision, scope, string(), string(), children(mirror))
        }

        WireKind.IMAGE -> {
            Image(id, revision, scope, string(), string(), children(mirror))
        }

        WireKind.DIRECTIVE -> {
            readDirective(id, revision, scope, mirror)
        }

        WireKind.FOOTNOTE_REFERENCE -> {
            FootnoteReference(id, revision, scope, requiredString())
        }

        WireKind.CROSS_LINK -> {
            CrossLink(id, revision, scope, requiredString())
        }

        WireKind.EMBED -> {
            Embed(id, revision, scope, requiredString())
        }

        WireKind.REFERENCE_DEFINITION -> {
            ReferenceDefinition(id, revision, scope, requiredString(), string(), string())
        }

        WireKind.LINK_REFERENCE -> {
            LinkReference(id, revision, scope, requiredString(), referenceForm(), children(mirror))
        }

        WireKind.IMAGE_REFERENCE -> {
            ImageReference(id, revision, scope, requiredString(), referenceForm(), children(mirror))
        }

        else -> {
            error("unsupported native node kind $kind")
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
    scope: Scope,
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
    return List(id, revision, scope, flavor, start, tight, items)
}

private fun WireReader.readDirective(
    id: MarkupID,
    revision: ULong,
    scope: Scope,
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
    return Directive(id, revision, scope, mode, name, attributes, label)
}

private fun WireReader.readDirectiveBlock(
    id: MarkupID,
    revision: ULong,
    scope: Scope,
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
    return DirectiveBlock(id, revision, scope, mode, name, attributes, label, content)
}

private fun WireReader.readTable(
    id: MarkupID,
    revision: ULong,
    scope: Scope,
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
        scope,
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
    scope: Scope,
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
    return TableRow(id, revision, scope, header, cells)
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

    // Appended, not inserted: the numbering is positional and shared with the
    // C enum, so a kind added in the middle would renumber every kind after
    // it in all four bindings.
    const val REFERENCE_DEFINITION = 32
    const val LINK_REFERENCE = 33
    const val IMAGE_REFERENCE = 34
}
