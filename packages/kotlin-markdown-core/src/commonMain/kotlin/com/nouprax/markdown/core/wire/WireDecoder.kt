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
 * Decodes one payload into [mirror], which it OWNS: an open payload fills an
 * empty mirror from the whole tree, an edit payload applies the delta to a
 * copy of the predecessor's. Either way every record's child ids resolve
 * against already-decoded entries in one pass, because both bodies deliver
 * records children-before-parents.
 *
 * The decoded pieces leave through [build] rather than in a payload type of
 * this file's own, so no wire type crosses a file boundary — the whole wire
 * layer stays file-private, which is what keeps it out of the Java-visible
 * surface. [scopeEntry] builds whatever the caller wants to remember about
 * one scope row, for the same reason.
 *
 * `content` is null exactly when no record named the document root, which
 * happens only for an edit whose delta is empty: nothing differs, so the
 * caller's own content is still the answer.
 */
private fun <Entry, Result> decode(
    bytes: ByteArray,
    mirror: MutableMap<ULong, Markup>,
    edit: Boolean,
    scopeEntry: (ULong, Scope) -> Entry,
    build: (
        handle: Long,
        id: MarkupID,
        revision: ULong,
        content: kotlin.collections.List<Markup>?,
        scopes: Map<ULong, Entry>,
        diagnostics: kotlin.collections.List<Diagnostic>,
    ) -> Result,
): Pair<Result, Delta?> {
    val reader = reader(bytes)
    val handle = reader.long()
    val lineage = reader.ulong()
    val rootId = reader.ulong()
    val revision = reader.ulong()
    val root = RootSink()
    val delta =
        if (edit) {
            reader.deltaBody(lineage, rootId, mirror, root)
        } else {
            reader.treeBody(lineage, rootId, mirror, root)
        }
    val scopes = reader.scopeMap(scopeEntry)
    val diagnostics = reader.diagnostics()
    require(reader.finished) { "trailing bytes after a native payload" }
    return build(
        handle,
        MarkupID(lineage, rootId),
        revision,
        root.content,
        scopes,
        diagnostics,
    ) to delta
}

@JvmSynthetic
internal fun <Entry, Result> decodeWireOpen(
    bytes: ByteArray,
    mirror: MutableMap<ULong, Markup>,
    scopeEntry: (ULong, Scope) -> Entry,
    build: (
        Long,
        MarkupID,
        ULong,
        kotlin.collections.List<Markup>?,
        Map<ULong, Entry>,
        kotlin.collections.List<Diagnostic>,
    ) -> Result,
): Result = decode(bytes, mirror, edit = false, scopeEntry, build).first

@JvmSynthetic
internal fun <Entry, Result> decodeWireEdit(
    bytes: ByteArray,
    mirror: MutableMap<ULong, Markup>,
    scopeEntry: (ULong, Scope) -> Entry,
    build: (
        Long,
        MarkupID,
        ULong,
        kotlin.collections.List<Markup>?,
        Map<ULong, Entry>,
        kotlin.collections.List<Diagnostic>,
    ) -> Result,
): Pair<Result, Delta> {
    val (result, delta) = decode(bytes, mirror, edit = true, scopeEntry, build)
    return result to checkNotNull(delta) { "an edit payload carried no delta" }
}

/** Catches the one record that is not a mirror entry. The root has no parent
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

/**
 * Reads a whole tree into [mirror]: every record, children before parents.
 * The root record is diverted to [root] rather than stored, and returns no
 * delta — an open payload has no predecessor to differ from.
 */
private fun WireReader.treeBody(
    lineage: ULong,
    rootId: ULong,
    mirror: MutableMap<ULong, Markup>,
    root: RootSink,
): Delta? {
    val count = int()
    require(count >= 0) { "invalid native record count" }
    repeat(count) { readInto(lineage, rootId, mirror, root) }
    requireNotNull(root.content) { "native payload carried no document root" }
    return null
}

/**
 * Applies one delta body to [mirror] and returns it.
 *
 * A row with no parts retires an id; every other row is that node's full
 * record. The rows arrive in the new document's postorder with each retired
 * node where it was found, so a record's child ids always resolve — against a
 * node this body just decoded, or against the unchanged value the mirror
 * already holds, which keeps its exact platform object across revisions.
 */
private fun WireReader.deltaBody(
    lineage: ULong,
    rootId: ULong,
    mirror: MutableMap<ULong, Markup>,
    root: RootSink,
): Delta {
    val beforeRevision = ulong()
    val afterRevision = ulong()
    val count = int()
    require(count >= 0) { "invalid native diff count" }
    val diffs =
        immutableList(count) {
            val parts = DiffParts(int())
            if (parts.isRetired) {
                val id = MarkupID(lineage, ulong())
                mirror.remove(id.rawValue)
                Diff(id, parts)
            } else {
                Diff(readInto(lineage, rootId, mirror, root), parts)
            }
        }
    return Delta(beforeRevision, afterRevision, diffs)
}

/** Decodes one record, stores it, and returns its identity. The root's record
 * is the one that never becomes a mirror entry: nothing resolves it as a
 * child, and on this side it is a [Document], which owns a native parse and
 * cannot be minted from a record. */
private fun WireReader.readInto(
    lineage: ULong,
    rootId: ULong,
    mirror: MutableMap<ULong, Markup>,
    root: RootSink,
): MarkupID {
    val kind = kind()
    val id = MarkupID(lineage, ulong())
    val revision = ulong()
    if (kind == WireKind.DOCUMENT) {
        require(id.rawValue == rootId) { "native payload carried a nested document record" }
        root.content = children(mirror)
        return id
    }
    val node = record(kind, id, revision, mirror)
    mirror[id.rawValue] = node
    return id
}

private fun WireReader.record(
    kind: Int,
    id: MarkupID,
    revision: ULong,
    mirror: Map<ULong, Markup>,
): Markup =
    when (kind) {
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
            HTMLBlock(id, revision, boolean(), requiredString())
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
            HTML(id, revision, boolean(), requiredString())
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

        WireKind.REFERENCE_DEFINITION -> {
            ReferenceDefinition(id, revision, requiredString(), string(), string())
        }

        WireKind.LINK_REFERENCE -> {
            LinkReference(id, revision, requiredString(), referenceForm(), children(mirror))
        }

        WireKind.IMAGE_REFERENCE -> {
            ImageReference(id, revision, requiredString(), referenceForm(), children(mirror))
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

    // Appended, not inserted: the numbering is positional and shared with the
    // C enum, so a kind added in the middle would renumber every kind after
    // it in all four bindings.
    const val REFERENCE_DEFINITION = 32
    const val LINK_REFERENCE = 33
    const val IMAGE_REFERENCE = 34
}
