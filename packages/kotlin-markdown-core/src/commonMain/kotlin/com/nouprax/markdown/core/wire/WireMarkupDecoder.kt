package com.nouprax.markdown.core

/**
 * Applies one MKC3 commit body to [mirror] and returns its delta.
 *
 * The body lists removed ids and then full records for added, changed, and
 * bubbled nodes ordered children-before-parents, so every record's child ids
 * resolve against already-decoded mirror entries in one pass. Unchanged
 * children keep their exact platform object across snapshots.
 */
internal fun WireReader.commitBody(
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
            Document(id, revision, children(mirror), ScopeResolver.unresolvable)
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

        WireKind.DIRECTIVE_BLOCK -> {
            DirectiveBlock(
                id,
                revision,
                placement(),
                requiredString(),
                string(),
                optionalChildren(mirror),
                children(mirror),
            )
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

        WireKind.TABLE_ROW -> {
            readTableRow(id, revision, mirror)
        }

        WireKind.TABLE_CELL -> {
            TableCell(id, revision, children(mirror))
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

private fun WireReader.optionalChildren(mirror: Map<ULong, Markup>): kotlin.collections.List<Markup>? {
    val count = int()
    require(count >= -1) { "invalid native child count" }
    return if (count == -1) null else immutableList(count) { child(mirror) }
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
    val label = optionalChildren(mirror)
    val content = children(mirror)
    require(content.isEmpty()) { "inline directive contains block content" }
    return Directive(id, revision, mode, name, attributes, label)
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
