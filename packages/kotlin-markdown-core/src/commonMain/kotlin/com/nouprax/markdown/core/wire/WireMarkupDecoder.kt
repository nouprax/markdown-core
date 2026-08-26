package com.nouprax.markdown.core

internal fun WireReader.markup(): Markup {
    val kind = kind()
    val id = identity()
    val nodeScope = scope()
    return when (kind) {
        WireKind.DOCUMENT -> {
            // Only ever the ROOT, and the root is read by `document()`, which is
            // the only place the concrete view exists to build it with.
            error("a document node cannot be a child")
        }

        WireKind.BLOCK_QUOTE -> {
            BlockQuote(markupList(), id, nodeScope)
        }

        WireKind.PARAGRAPH -> {
            Paragraph(markupList(), id, nodeScope)
        }

        WireKind.HEADING -> {
            Heading(int(), markupList(), id, nodeScope)
        }

        WireKind.THEMATIC_BREAK -> {
            ThematicBreak(id, nodeScope)
        }

        WireKind.LIST -> {
            readList(id, nodeScope)
        }

        WireKind.LIST_ITEM -> {
            ListItem(nullableBoolean(), markupList(), id, nodeScope)
        }

        WireKind.CODE_BLOCK -> {
            CodeBlock(
                string(),
                string(),
                requiredString(),
                boolean(),
                boolean(),
                id,
                nodeScope,
            )
        }

        WireKind.HTML_BLOCK -> {
            HTMLBlock(requiredString(), id, nodeScope)
        }

        WireKind.FORMULA_BLOCK -> {
            // A formula block is always standalone: the wire stopped carrying
            // the byte at Q29 and the model no longer repeats the kind.
            FormulaBlock(requiredString(), id, nodeScope)
        }

        WireKind.TABLE -> {
            readTable(id, nodeScope)
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
                id,
                nodeScope,
            )
        }

        WireKind.FOOTNOTE_DEFINITION -> {
            FootnoteDefinition(requiredString(), requiredString(), markupList(), id, nodeScope)
        }

        WireKind.REFERENCE_DEFINITION -> {
            ReferenceDefinition(requiredString(), requiredString(), requiredString(), string(), id, nodeScope)
        }

        WireKind.LINK_REFERENCE -> {
            LinkReference(requiredString(), referenceForm(), identity(), markupList(), id, nodeScope)
        }

        WireKind.IMAGE_REFERENCE -> {
            ImageReference(requiredString(), referenceForm(), identity(), markupList(), id, nodeScope)
        }

        WireKind.TEXT -> {
            Text(requiredString(), id, nodeScope)
        }

        WireKind.SOFT_BREAK -> {
            SoftBreak(id, nodeScope)
        }

        WireKind.LINE_BREAK -> {
            LineBreak(id, nodeScope)
        }

        WireKind.CODE -> {
            Code(requiredString(), id, nodeScope)
        }

        WireKind.HTML -> {
            HTML(requiredString(), id, nodeScope)
        }

        WireKind.FORMULA -> {
            Formula(placement(), requiredString(), id, nodeScope)
        }

        WireKind.EMPHASIS -> {
            Emphasis(markupList(), id, nodeScope)
        }

        WireKind.STRONG -> {
            Strong(markupList(), id, nodeScope)
        }

        WireKind.STRIKETHROUGH -> {
            Strikethrough(markupList(), id, nodeScope)
        }

        WireKind.LINK -> {
            Link(requiredString(), string(), markupList(), id, nodeScope)
        }

        WireKind.IMAGE -> {
            Image(requiredString(), string(), markupList(), id, nodeScope)
        }

        WireKind.DIRECTIVE -> {
            readDirective(id, nodeScope)
        }

        WireKind.FOOTNOTE_REFERENCE -> {
            FootnoteReference(requiredString(), identity(), id, nodeScope)
        }

        WireKind.TABLE_ROW -> {
            readTableRow(id, nodeScope)
        }

        WireKind.TABLE_CELL -> {
            readTableCell(id, nodeScope)
        }

        WireKind.DIRECTIVE_LABEL -> {
            DirectiveLabel(markupList(), id, nodeScope)
        }
    }
}

private fun WireReader.placement(): PlacementMode =
    when (val rawValue = int()) {
        1 -> PlacementMode.EMBEDDED
        2 -> PlacementMode.STANDALONE
        else -> error("invalid native placement mode $rawValue")
    }

private fun WireReader.referenceForm(): ReferenceForm =
    when (val rawValue = int()) {
        1 -> ReferenceForm.FULL
        2 -> ReferenceForm.COLLAPSED
        3 -> ReferenceForm.SHORTCUT
        else -> error("unsupported native reference form $rawValue")
    }

internal fun WireReader.markupList(): kotlin.collections.List<Markup> {
    val count = int()
    require(count >= 0) { "invalid native child count" }
    return immutableList(count) { markup() }
}

private fun WireReader.readList(
    id: Identity,
    scope: Scope,
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
    return List(flavor, start, tight, readListItems(), id, scope)
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

private fun WireReader.readDirective(
    id: Identity,
    scope: Scope,
): Directive {
    // An inline directive is always embedded: the wire stopped carrying the
    // byte at Q29 and the model no longer repeats the kind.
    val name = requiredString()
    val attributes = directiveAttributes()
    val children = markupList()
    val label = children.firstOrNull() as? DirectiveLabel
    require(children.size == (if (label == null) 0 else 1)) { "inline directive contains block content" }
    return Directive(name, attributes, label, id, scope)
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

private fun WireReader.readTable(
    id: Identity,
    scope: Scope,
): Table {
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
        id,
        scope,
    )
}

private fun WireReader.readTableRow(
    id: Identity,
    scope: Scope,
): TableRow {
    val header = boolean()
    val cellCount = int()
    require(cellCount >= 0) { "invalid native table cell count" }
    val cells =
        immutableList(cellCount) {
            val cell = markup()
            require(cell is TableCell) { "table row contains a non-cell node" }
            cell
        }
    return TableRow(header, cells, id, scope)
}

private fun WireReader.readTableCell(
    id: Identity,
    scope: Scope,
): TableCell = TableCell(markupList(), id, scope)

private fun tableAlignment(rawValue: Int): TableAlignment =
    when (rawValue) {
        0 -> TableAlignment.NONE
        1 -> TableAlignment.LEFT
        2 -> TableAlignment.CENTER
        3 -> TableAlignment.RIGHT
        else -> error("invalid native table alignment $rawValue")
    }
