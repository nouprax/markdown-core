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
                readDocument(scope, consume)
            }

            JniNodeKind.CALLOUT -> {
                readCallout(scope, consume)
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
                val marker = reader.string()
                val exampleLabel = reader.string()
                readChildren { consume(ListItem(marker, exampleLabel, it, scope)) }
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

            JniNodeKind.COMMENT -> {
                consume(Comment(reader.requiredString(), scope))
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
                val resource = resource()
                readChildren { consume(Link(resource.first, resource.second, it, scope)) }
            }

            JniNodeKind.IMAGE -> {
                val resource = resource()
                readChildren { consume(Image(resource.first, resource.second, it, scope)) }
            }

            JniNodeKind.DIRECTIVE -> {
                readDirective(scope, consume)
            }

            JniNodeKind.CITE -> {
                readCitations { consume(Cite(it, scope)) }
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
        readNodes(count, consume)
    }

    /** Schedules `count` nodes and then the list they form, in payload order. */
    private fun readNodes(
        count: Int,
        consume: (kotlin.collections.List<Markup>) -> Unit,
    ) {
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

    /**
     * The document's content leads, as the walk visits it; its footnotes
     * follow as a counted list of values, each its scope, its id, and its
     * content.
     */
    private fun readDocument(
        scope: Scope,
        consume: (Markup) -> Unit,
    ) {
        var content: kotlin.collections.List<Markup>? = null
        actions.addLast {
            val count = reader.int()
            require(count >= 0) { "invalid native footnote count" }
            val values = arrayOfNulls<Footnote>(count)
            actions.addLast {
                val footnotes =
                    immutableList(count) { index ->
                        requireNotNull(values[index]) { "JNI footnote was not decoded" }
                    }
                consume(Document(requireNotNull(content), footnotes, scope))
            }
            for (index in count - 1 downTo 0) {
                actions.addLast { readFootnote { values[index] = it } }
            }
        }
        actions.addLast {
            readChildren { content = it }
        }
    }

    private fun readFootnote(consume: (Footnote) -> Unit) {
        val scope = reader.scope()
        val id = reader.requiredString()
        readChildren { consume(Footnote(id, it, scope)) }
    }

    /**
     * A cite's items are a counted list of values, each its scope, its
     * referent -- the branch ordinal, then only that branch's fields -- and
     * its prefix and suffix content in that order.
     */
    private fun readCitations(consume: (kotlin.collections.List<Citation>) -> Unit) {
        val count = reader.int()
        require(count >= 1) { "invalid native citation count" }
        val values = arrayOfNulls<Citation>(count)
        actions.addLast {
            consume(
                immutableList(count) { index ->
                    requireNotNull(values[index]) { "JNI citation was not decoded" }
                },
            )
        }
        for (index in count - 1 downTo 0) {
            actions.addLast { readCitation { values[index] = it } }
        }
    }

    private fun readCitation(consume: (Citation) -> Unit) {
        val scope = reader.scope()
        val referent =
            when (val branch = reader.byte().toInt()) {
                1 -> CitationReferent.Bib(reader.requiredString(), bibMode())
                2 -> CitationReferent.Footnote(reader.requiredString())
                else -> error("invalid native citation referent $branch")
            }
        var prefix: kotlin.collections.List<Markup>? = null
        actions.addLast {
            readChildren { suffix -> consume(Citation(referent, requireNotNull(prefix), suffix, scope)) }
        }
        actions.addLast {
            readChildren { prefix = it }
        }
    }

    private fun bibMode(): BibMode =
        when (val rawValue = reader.int()) {
            1 -> BibMode.NORMAL
            2 -> BibMode.AUTHOR_IN_TEXT
            3 -> BibMode.SUPPRESS_AUTHOR
            else -> error("invalid native bib mode $rawValue")
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
        val variantKind = reader.int()
        val variantLowercased = reader.boolean()
        val variant =
            when (variantKind) {
                0 -> null
                1 -> OrderedListVariant.Decimal
                2 -> OrderedListVariant.Alpha(variantLowercased)
                3 -> OrderedListVariant.Roman(variantLowercased)
                4 -> OrderedListVariant.Example
                5 -> OrderedListVariant.Default
                else -> error("invalid native list variant $variantKind")
            }
        val delimiterKind = reader.int()
        val delimiterClosed = reader.boolean()
        val delimiter =
            when (delimiterKind) {
                0 -> null
                1 -> OrderedListDelimiter.Period
                2 -> OrderedListDelimiter.Parenthesis(delimiterClosed)
                3 -> OrderedListDelimiter.Default
                else -> error("invalid native list delimiter $delimiterKind")
            }
        val tight = reader.boolean()
        readChildren { children ->
            val items = children.immutableMap { requireNotNull(it as? ListItem) { "list contains a non-item node" } }
            consume(List(flavor, start, variant, delimiter, tight, items, scope))
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

    /**
     * A callout's metadata leads, then its title -- a node-valued list that the
     * payload sends before the content, as the walk visits it, and whose count
     * is its presence because a present title holds at least one node -- and
     * then the content. Every callout is metadata-free until O8.
     */
    private fun readCallout(
        scope: Scope,
        consume: (Markup) -> Unit,
    ) {
        val variant = reader.string()
        val collapsed = reader.nullableBoolean()
        val titleCount = reader.int()
        require(titleCount >= 0) { "invalid native callout title count" }
        if (titleCount == 0) {
            readChildren { consume(Callout(variant, collapsed, null, it, scope)) }
            return
        }
        var title: kotlin.collections.List<Markup>? = null
        actions.addLast {
            readChildren { children -> consume(Callout(variant, collapsed, requireNotNull(title), children, scope)) }
        }
        actions.addLast {
            readNodes(titleCount) { title = it }
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

    /**
     * Every occurrence of one reference definition shares one resource, and the
     * payload sends it once: a resource ordinal leads, and only the first
     * occurrence of an ordinal carries the destination and title, so a long
     * destination referenced many times crosses the boundary once and is
     * materialized once.
     */
    private val resources = ArrayList<Pair<Destination, String?>>()

    private fun resource(): Pair<Destination, String?> {
        val ordinal = reader.int()
        if (ordinal in resources.indices) return resources[ordinal]
        require(ordinal == resources.size) { "JNI payload names an unknown resource $ordinal" }
        val resource = destination() to reader.string()
        resources += resource
        return resource
    }

    /** The branch ordinal leads; only that branch's fields follow it. */
    private fun destination(): Destination =
        when (val rawValue = reader.int()) {
            1 -> Destination.Url(reader.requiredString())
            2 -> Destination.Cross(reader.requiredString(), reader.string())
            else -> error("invalid native destination kind $rawValue")
        }

    private fun placement(): PlacementMode =
        when (val rawValue = reader.int()) {
            1 -> PlacementMode.EMBEDDED
            2 -> PlacementMode.STANDALONE
            else -> error("invalid native placement mode $rawValue")
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
