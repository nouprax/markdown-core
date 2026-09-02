package com.nouprax.markdown.core

internal fun WireReader.markup(): Markup {
    val kind = kind()
    val id = identity()
    val nodeScope = scope()
    return markupOf(kind, id, nodeScope)
}

/**
 * A SPINE op's node (#162), [previous] being the previous read's node at
 * the same position: the fields are read afresh and the children come from
 * the ops that follow, against the previous node's children.
 */
internal fun WireReader.spine(previous: Markup): Markup {
    val kind = kind()
    val (expected, children) = positionalChildren(previous)
    require(kind == expected) { "native parser rewrote a $expected as a $kind" }
    val id = identity()
    require(id == previous.id) { "native parser rewrote a node under another identity" }
    val nodeScope = scope()
    // The context is always consumed: every kind `positionalChildren` knows
    // reads its children through `markupList`.
    spine = children
    return if (kind == WireKind.DOCUMENT) Semantic(markupList(), id, nodeScope) else markupOf(kind, id, nodeScope)
}

/**
 * The previous read's children of a node a SPINE rewrites, in WIRE ORDER --
 * the positions the op stream counts in -- with the kind the rewrite must
 * name. Only the block containers the native side ever rewrites have one:
 * a list's items, a table's header then its rows, a directive block's label
 * then its content, and the content of the rest. A spine on any other kind
 * is refused.
 */
private fun positionalChildren(node: Markup): Pair<WireKind, kotlin.collections.List<Markup>> =
    when (node) {
        is Semantic -> {
            WireKind.DOCUMENT to node.content
        }

        is BlockQuote -> {
            WireKind.BLOCK_QUOTE to node.content
        }

        is ListItem -> {
            WireKind.LIST_ITEM to node.content
        }

        is FootnoteDefinition -> {
            WireKind.FOOTNOTE_DEFINITION to node.content
        }

        is List -> {
            WireKind.LIST to node.items
        }

        is Table -> {
            WireKind.TABLE to listOf<Markup>(node.header) + node.rows
        }

        is DirectiveBlock -> {
            WireKind.DIRECTIVE_BLOCK to
                (node.label?.let { label -> listOf<Markup>(label) + node.content } ?: node.content)
        }

        else -> {
            error("native parser rewrote a ${node::class.simpleName}, which has no children to reuse")
        }
    }

/**
 * The ops that turn [previous] -- the previous read's children of the node
 * being rewritten -- into the new node's children: SAME reuses the next run
 * of them as they are, SPINE rewrites the next one, and any other tag is a
 * kind byte opening a node written whole. Two passes: the ops are read
 * into PARTS, a reused run's length or a node read afresh, and counted;
 * the children are then filled into a list of exactly that capacity. The
 * reused run is every closed block of the document on every feed that
 * grows it -- the reference copy the delta leaves the reader
 * (docs/STREAMING.md §6) -- and a list grown by halves under an append per
 * reused child copied itself on the way.
 */
private fun WireReader.ops(previous: kotlin.collections.List<Markup>): kotlin.collections.List<Markup> {
    val count = int()
    require(count >= 0) { "invalid native op count" }
    val parts = ArrayList<Any>(count)
    var total = 0
    var position = 0
    repeat(count) {
        when (val tag = rawTag()) {
            WireReader.OP_SAME -> {
                val run = int()
                require(run >= 0 && run <= previous.size - position) {
                    "native parser reused children the previous read does not have"
                }
                parts += run
                total += run
                position += run
            }

            WireReader.OP_SPINE -> {
                require(position < previous.size) { "native parser rewrote a child the previous read does not have" }
                parts += spine(previous[position])
                total++
                position++
            }

            else -> {
                val kind = WireKind.from(tag)
                val id = identity()
                val nodeScope = scope()
                parts += markupOf(kind, id, nodeScope)
                total++
                position++
            }
        }
    }
    val children = ArrayList<Markup>(total)
    position = 0
    for (part in parts) {
        if (part is Int) {
            for (offset in 0 until part) children += previous[position + offset]
            position += part
        } else {
            children += part as Markup
            position++
        }
    }
    return children.asImmutable()
}

private fun WireReader.markupOf(
    kind: WireKind,
    id: Identity,
    nodeScope: Scope,
): Markup =
    when (kind) {
        WireKind.DOCUMENT -> {
            // Only ever the ROOT, and the root is read by `read()`.
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
    val previous = spine
    if (previous != null) {
        spine = null
        return ops(previous)
    }
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

/**
 * Through `markupList`, not a loop of its own: a list is a container of
 * blocks, so a delta may rewrite it as a SPINE whose ops address its items
 * by position (#162). The cast is sound once every element is checked: the
 * list is read-only and erased.
 */
private fun WireReader.readListItems(): kotlin.collections.List<ListItem> {
    val items = markupList()
    items.forEach { item -> require(item is ListItem) { "list contains a non-item node" } }
    @Suppress("UNCHECKED_CAST")
    return items as kotlin.collections.List<ListItem>
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
    // Through `markupList` for the same reason as a list's items. The
    // header row is the table's FIRST child on the wire -- the engine opens
    // a table with it -- which is what lets a delta address
    // `[header, ...rows]` by position.
    val children = markupList()
    children.forEach { row -> require(row is TableRow) { "table contains a non-row node" } }
    @Suppress("UNCHECKED_CAST")
    val rows = children as kotlin.collections.List<TableRow>
    val headers = rows.count { row -> row.isHeader }
    require(headers <= 1) { "table has multiple header rows" }
    require(headers == 1) { "table has no header" }
    require(rows.first().isHeader) { "table does not open with its header row" }
    return Table(
        alignments,
        rows.first(),
        rows.subList(1, rows.size).asImmutable(),
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
