package com.nouprax.markdown.core

internal fun JniPayloadReader.decodeTree(): Document = JniTreeDecoder(this).decodeDocument()

/** Decodes the depth-first JNI payload with heap-backed actions, never the JVM stack. */
private class JniTreeDecoder(
    private val reader: JniPayloadReader,
) {
    private val actions = ArrayDeque<() -> Unit>()
    private var nodesStarted = 0

    fun decodeDocument(): Document {
        var root: Markup? = null
        actions.addLast { readNode { root = it } }
        while (actions.isNotEmpty()) actions.removeLast().invoke()
        require(reader.finished) { "JNI payload contains trailing data" }
        return requireNotNull(root as? Document) { "JNI payload contains an invalid document tree" }
    }

    private fun readNode(consume: (Markup) -> Unit) {
        val kind = reader.kind()
        val isRoot = nodesStarted++ == 0
        require((kind == JniNodeKind.DOCUMENT) == isRoot) {
            "JNI payload must contain exactly one document at its root"
        }
        val scope = reader.scope()
        when (kind) {
            JniNodeKind.DOCUMENT -> {
                readChildren { consume(Document(it, scope)) }
            }

            JniNodeKind.BLOCK_QUOTE -> {
                readChildren { consume(BlockQuote(it, scope)) }
            }

            JniNodeKind.PARAGRAPH -> {
                readChildren { consume(Paragraph(it, scope)) }
            }

            JniNodeKind.HEADING -> {
                val level = reader.int()
                readChildren { consume(Heading(level, it, scope)) }
            }

            JniNodeKind.THEMATIC_BREAK -> {
                consume(ThematicBreak(scope))
            }

            JniNodeKind.LIST -> {
                readList(scope, consume)
            }

            JniNodeKind.LIST_ITEM -> {
                val checked = reader.nullableBoolean()
                readChildren { consume(ListItem(checked, it, scope)) }
            }

            JniNodeKind.CODE_BLOCK -> {
                consume(
                    CodeBlock(
                        reader.string(),
                        reader.string(),
                        reader.requiredString(),
                        reader.boolean(),
                        reader.boolean(),
                        scope,
                    ),
                )
            }

            JniNodeKind.HTML_BLOCK -> {
                consume(HTMLBlock(reader.requiredString(), scope))
            }

            JniNodeKind.FORMULA_BLOCK -> {
                consume(FormulaBlock(reader.requiredString(), scope))
            }

            JniNodeKind.TABLE -> {
                readTable(scope, consume)
            }

            JniNodeKind.DIRECTIVE_BLOCK -> {
                readDirectiveBlock(scope, consume)
            }

            JniNodeKind.FOOTNOTE_DEFINITION -> {
                val label = reader.requiredString()
                val identifier = reader.requiredString()
                readChildren { consume(FootnoteDefinition(label, identifier, it, scope)) }
            }

            JniNodeKind.REFERENCE_DEFINITION -> {
                consume(
                    ReferenceDefinition(
                        reader.requiredString(),
                        reader.requiredString(),
                        reader.requiredString(),
                        reader.string(),
                        scope,
                    ),
                )
            }

            JniNodeKind.LINK_REFERENCE -> {
                val label = reader.requiredString()
                val identifier = reader.requiredString()
                val form = referenceForm()
                readChildren { consume(LinkReference(label, identifier, form, it, scope)) }
            }

            JniNodeKind.IMAGE_REFERENCE -> {
                val label = reader.requiredString()
                val identifier = reader.requiredString()
                val form = referenceForm()
                readChildren { consume(ImageReference(label, identifier, form, it, scope)) }
            }

            JniNodeKind.TEXT -> {
                consume(Text(reader.requiredString(), scope))
            }

            JniNodeKind.SOFT_BREAK -> {
                consume(SoftBreak(scope))
            }

            JniNodeKind.LINE_BREAK -> {
                consume(LineBreak(scope))
            }

            JniNodeKind.CODE -> {
                consume(Code(reader.requiredString(), scope))
            }

            JniNodeKind.HTML -> {
                consume(HTML(reader.requiredString(), scope))
            }

            JniNodeKind.FORMULA -> {
                consume(Formula(placement(), reader.requiredString(), scope))
            }

            JniNodeKind.EMPHASIS -> {
                readChildren { consume(Emphasis(it, scope)) }
            }

            JniNodeKind.STRONG -> {
                readChildren { consume(Strong(it, scope)) }
            }

            JniNodeKind.STRIKETHROUGH -> {
                readChildren { consume(Strikethrough(it, scope)) }
            }

            JniNodeKind.LINK -> {
                val destination = reader.requiredString()
                val title = reader.string()
                readChildren { consume(Link(destination, title, it, scope)) }
            }

            JniNodeKind.IMAGE -> {
                val source = reader.requiredString()
                val title = reader.string()
                readChildren { consume(Image(source, title, it, scope)) }
            }

            JniNodeKind.DIRECTIVE -> {
                readDirective(scope, consume)
            }

            JniNodeKind.FOOTNOTE_REFERENCE -> {
                consume(FootnoteReference(reader.requiredString(), reader.requiredString(), scope))
            }

            JniNodeKind.TABLE_ROW -> {
                readTableRow(scope, consume)
            }

            JniNodeKind.TABLE_CELL -> {
                readChildren { consume(TableCell(it, scope)) }
            }

            JniNodeKind.DIRECTIVE_LABEL -> {
                readChildren { consume(DirectiveLabel(it, scope)) }
            }
        }
    }

    private fun readChildren(consume: (kotlin.collections.List<Markup>) -> Unit) {
        val count = reader.int()
        require(count >= 0) { "invalid native child count" }
        val values = arrayOfNulls<Markup>(count)
        actions.addLast {
            consume(
                immutableList(count) { index ->
                    requireNotNull(values[index]) { "JNI child was not decoded" }
                },
            )
        }
        for (index in count - 1 downTo 0) {
            actions.addLast { readNode { values[index] = it } }
        }
    }

    private fun readList(
        scope: Scope,
        consume: (Markup) -> Unit,
    ) {
        val flavor =
            when (val rawValue = reader.int()) {
                1 -> ListFlavor.BULLET
                2 -> ListFlavor.ORDERED
                else -> error("invalid native list flavor $rawValue")
            }
        val startValue = reader.long()
        val start = if (reader.boolean()) startValue else null
        val tight = reader.boolean()
        readChildren { children ->
            val items = children.immutableMap { requireNotNull(it as? ListItem) { "list contains a non-item node" } }
            consume(List(flavor, start, tight, items, scope))
        }
    }

    private fun readDirectiveBlock(
        scope: Scope,
        consume: (Markup) -> Unit,
    ) {
        val name = reader.requiredString()
        val attributes = directiveAttributes()
        readDirectiveRelations { label, children ->
            consume(DirectiveBlock(name, attributes, label, children, scope))
        }
    }

    private fun readDirective(
        scope: Scope,
        consume: (Markup) -> Unit,
    ) {
        val name = reader.requiredString()
        val attributes = directiveAttributes()
        readDirectiveRelations { label, children ->
            require(children.isEmpty()) { "inline directive contains block content" }
            consume(Directive(name, attributes, label, scope))
        }
    }

    /** Reads the independent node-valued label field before directive content. */
    private fun readDirectiveRelations(consume: (DirectiveLabel?, kotlin.collections.List<Markup>) -> Unit) {
        if (!reader.boolean()) {
            readChildren { consume(null, it) }
            return
        }
        var label: DirectiveLabel? = null
        actions.addLast {
            readChildren { children -> consume(requireNotNull(label), children) }
        }
        actions.addLast {
            readNode { node ->
                label = requireNotNull(node as? DirectiveLabel) { "directive label field contains a non-label node" }
            }
        }
    }

    private fun directiveAttributes(): kotlin.collections.List<DirectiveAttribute>? {
        val present = reader.boolean()
        val count = reader.int()
        require(count >= 0) { "invalid native directive attribute count" }
        if (!present) {
            require(count == 0) { "an absent directive attribute container cannot hold attributes" }
            return null
        }
        return immutableList(count) { DirectiveAttribute(reader.requiredString(), reader.requiredString()) }
    }

    private fun readTable(
        scope: Scope,
        consume: (Markup) -> Unit,
    ) {
        val alignmentCount = reader.int()
        require(alignmentCount >= 0) { "invalid native table alignment count" }
        val alignments = immutableList(alignmentCount) { tableAlignment(reader.byte().toInt() and 0xff) }
        readChildren { children ->
            val rows = children.immutableMap { requireNotNull(it as? TableRow) { "table contains a non-row node" } }
            val headers = rows.filter(TableRow::isHeader)
            require(headers.size == 1) { "table must contain exactly one header row" }
            consume(Table(alignments, headers.single(), rows.filterNot(TableRow::isHeader).immutableMap { it }, scope))
        }
    }

    private fun readTableRow(
        scope: Scope,
        consume: (Markup) -> Unit,
    ) {
        val header = reader.boolean()
        readChildren { children ->
            val cells = children.immutableMap { requireNotNull(it as? TableCell) { "table row contains a non-cell" } }
            consume(TableRow(header, cells, scope))
        }
    }

    private fun placement(): PlacementMode =
        when (val rawValue = reader.int()) {
            1 -> PlacementMode.EMBEDDED
            2 -> PlacementMode.STANDALONE
            else -> error("invalid native placement mode $rawValue")
        }

    private fun referenceForm(): ReferenceForm =
        when (val rawValue = reader.int()) {
            1 -> ReferenceForm.FULL
            2 -> ReferenceForm.COLLAPSED
            3 -> ReferenceForm.SHORTCUT
            else -> error("unsupported native reference form $rawValue")
        }

    private fun tableAlignment(rawValue: Int): TableAlignment =
        when (rawValue) {
            0 -> TableAlignment.NONE
            1 -> TableAlignment.LEFT
            2 -> TableAlignment.CENTER
            3 -> TableAlignment.RIGHT
            else -> error("invalid native table alignment $rawValue")
        }
}
