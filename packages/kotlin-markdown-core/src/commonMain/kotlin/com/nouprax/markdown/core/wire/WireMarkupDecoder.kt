package com.nouprax.markdown.core

internal fun WireReader.markup(): Markup {
    val kind = kind()
    val nodeScope = scope()
    return when (kind) {
        WireKind.DOCUMENT -> {
            Document(markupList(), nodeScope)
        }

        WireKind.BLOCK_QUOTE -> {
            BlockQuote(markupList(), nodeScope)
        }

        WireKind.PARAGRAPH -> {
            Paragraph(markupList(), nodeScope)
        }

        WireKind.HEADING -> {
            Heading(int(), markupList(), nodeScope)
        }

        WireKind.THEMATIC_BREAK -> {
            ThematicBreak(nodeScope)
        }

        WireKind.LIST -> {
            readList(nodeScope)
        }

        WireKind.LIST_ITEM -> {
            ListItem(nullableBoolean(), markupList(), nodeScope)
        }

        WireKind.CODE_BLOCK -> {
            CodeBlock(
                string(),
                string(),
                requiredString(),
                boolean(),
                boolean(),
                nodeScope,
            )
        }

        WireKind.HTML_BLOCK -> {
            HTMLBlock(requiredString(), nodeScope)
        }

        WireKind.FORMULA_BLOCK -> {
            // A formula block is always standalone: the wire stopped carrying
            // the byte at Q29 and the model no longer repeats the kind.
            FormulaBlock(requiredString(), nodeScope)
        }

        WireKind.TABLE -> {
            readTable(nodeScope)
        }

        WireKind.DIRECTIVE_BLOCK -> {
            val name = requiredString()
            val attributes = directiveAttributes()
            val children = markupList()
            val label = children.firstOrNull() as? DirectiveLabel
            DirectiveBlock(
                name,
                attributes,
                label,
                if (label == null) children else children.drop(1),
                nodeScope,
            )
        }

        WireKind.FOOTNOTE_DEFINITION -> {
            FootnoteDefinition(requiredString(), markupList(), nodeScope)
        }

        WireKind.REFERENCE_DEFINITION -> {
            ReferenceDefinition(requiredString(), requiredString(), string(), nodeScope)
        }

        WireKind.TEXT -> {
            Text(requiredString(), nodeScope)
        }

        WireKind.SOFT_BREAK -> {
            SoftBreak(nodeScope)
        }

        WireKind.LINE_BREAK -> {
            LineBreak(nodeScope)
        }

        WireKind.CODE -> {
            Code(requiredString(), nodeScope)
        }

        WireKind.HTML -> {
            HTML(requiredString(), nodeScope)
        }

        WireKind.FORMULA -> {
            Formula(placement(), requiredString(), nodeScope)
        }

        WireKind.EMPHASIS -> {
            Emphasis(markupList(), nodeScope)
        }

        WireKind.STRONG -> {
            Strong(markupList(), nodeScope)
        }

        WireKind.STRIKETHROUGH -> {
            Strikethrough(markupList(), nodeScope)
        }

        WireKind.LINK -> {
            Link(string(), string(), markupList(), nodeScope)
        }

        WireKind.IMAGE -> {
            Image(string(), string(), markupList(), nodeScope)
        }

        WireKind.DIRECTIVE -> {
            readDirective(nodeScope)
        }

        WireKind.FOOTNOTE_REFERENCE -> {
            FootnoteReference(requiredString(), nodeScope)
        }

        WireKind.TABLE_ROW -> {
            readTableRow(nodeScope)
        }

        WireKind.TABLE_CELL -> {
            readTableCell(nodeScope)
        }

        WireKind.DIRECTIVE_LABEL -> {
            DirectiveLabel(markupList(), nodeScope)
        }
    }
}

private fun WireReader.placement(): PlacementMode =
    when (val rawValue = int()) {
        1 -> PlacementMode.EMBEDDED
        2 -> PlacementMode.STANDALONE
        else -> error("invalid native placement mode $rawValue")
    }

private fun WireReader.markupList(): kotlin.collections.List<Markup> {
    val count = int()
    require(count >= 0) { "invalid native child count" }
    return immutableList(count) { markup() }
}

private fun WireReader.readList(scope: Scope): List {
    val flavor =
        when (val rawValue = int()) {
            1 -> ListFlavor.BULLET
            2 -> ListFlavor.ORDERED
            else -> error("invalid native list flavor $rawValue")
        }
    val startValue = long()
    val start = if (boolean()) startValue else null
    val tight = boolean()
    return List(flavor, start, tight, readListItems(), scope)
}

private fun WireReader.readListItems(): kotlin.collections.List<ListItem> {
    val count = int()
    require(count >= 0) { "invalid native list item count" }
    return immutableList(count) {
        val item = markup()
        require(item is ListItem) { "list contains a non-item node" }
        item
    }
}

private fun WireReader.readDirective(scope: Scope): Directive {
    // An inline directive is always embedded: the wire stopped carrying the
    // byte at Q29 and the model no longer repeats the kind.
    val name = requiredString()
    val attributes = directiveAttributes()
    val children = markupList()
    val label = children.firstOrNull() as? DirectiveLabel
    require(children.size == (if (label == null) 0 else 1)) { "inline directive contains block content" }
    return Directive(name, attributes, label, scope)
}

/**
 * An absent attribute container and an empty one are different things, so the
 * wire carries the presence byte before the count rather than spending -1 on
 * it the way an optional child list does.
 */
private fun WireReader.directiveAttributes(): kotlin.collections.List<DirectiveAttribute>? {
    val present = boolean()
    val count = int()
    require(count >= 0) { "invalid native directive attribute count" }
    if (!present) {
        require(count == 0) { "an absent directive attribute container cannot hold attributes" }
        return null
    }
    return immutableList(count) { DirectiveAttribute(requiredString(), requiredString()) }
}

private fun WireReader.readTable(scope: Scope): Table {
    val alignmentCount = int()
    require(alignmentCount >= 0) { "invalid native table alignment count" }
    val alignments = immutableList(alignmentCount) { tableAlignment(byte().toInt() and 0xff) }
    val rowCount = int()
    require(rowCount >= 0) { "invalid native table row count" }
    val rows =
        immutableList(rowCount) {
            val row = markup()
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
        alignments,
        rows[headerIndex],
        rows
            .filterIndexed { index, _ -> index != headerIndex }
            .immutableMap { it },
        scope,
    )
}

private fun WireReader.readTableRow(scope: Scope): TableRow {
    val header = boolean()
    val cellCount = int()
    require(cellCount >= 0) { "invalid native table cell count" }
    val cells =
        immutableList(cellCount) {
            val cell = markup()
            require(cell is TableCell) { "table row contains a non-cell node" }
            cell
        }
    return TableRow(header, cells, scope)
}

private fun WireReader.readTableCell(scope: Scope): TableCell = TableCell(markupList(), scope)

private fun tableAlignment(rawValue: Int): TableAlignment =
    when (rawValue) {
        0 -> TableAlignment.NONE
        1 -> TableAlignment.LEFT
        2 -> TableAlignment.CENTER
        3 -> TableAlignment.RIGHT
        else -> error("invalid native table alignment $rawValue")
    }
