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
        val anchor = reader.string().also { require(it != "") { "empty normalized anchor" } }
        val attributes = attributes()
        when (kind) {
            JniNodeKind.DOCUMENT -> {
                readDocument(scope, anchor, attributes, consume)
            }

            JniNodeKind.CALLOUT -> {
                readCallout(scope, anchor, attributes, consume)
            }

            JniNodeKind.PARAGRAPH -> {
                readChildren { consume(Paragraph(it, scope, anchor, attributes)) }
            }

            JniNodeKind.HEADING -> {
                val level = reader.int()
                readChildren { consume(Heading(level, it, scope, anchor, attributes)) }
            }

            JniNodeKind.THEMATIC_BREAK -> {
                consume(ThematicBreak(scope, anchor, attributes))
            }

            JniNodeKind.LIST -> {
                readList(scope, anchor, attributes, consume)
            }

            JniNodeKind.LIST_ITEM -> {
                val marker = reader.string()
                readChildren { consume(ListItem(marker, it, scope, anchor, attributes)) }
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
                        anchor,
                        attributes,
                    ),
                )
            }

            JniNodeKind.HTML_BLOCK -> {
                consume(HTMLBlock(reader.requiredString(), scope, anchor, attributes))
            }

            JniNodeKind.FORMULA_BLOCK -> {
                consume(FormulaBlock(reader.requiredString(), scope, anchor, attributes))
            }

            JniNodeKind.TABLE -> {
                readTable(scope, anchor, attributes, consume)
            }

            JniNodeKind.DIRECTIVE_BLOCK -> {
                readDirectiveBlock(scope, anchor, attributes, consume)
            }

            JniNodeKind.TEXT -> {
                consume(Text(reader.requiredString(), scope, anchor, attributes))
            }

            JniNodeKind.SOFT_BREAK -> {
                consume(SoftBreak(scope, anchor, attributes))
            }

            JniNodeKind.LINE_BREAK -> {
                consume(LineBreak(scope, anchor, attributes))
            }

            JniNodeKind.CODE -> {
                consume(Code(reader.requiredString(), scope, anchor, attributes))
            }

            JniNodeKind.HTML -> {
                consume(HTML(reader.requiredString(), scope, anchor, attributes))
            }

            JniNodeKind.CROSS_LINK, JniNodeKind.CROSS_EMBEDDED -> {
                val dest = destination()
                require(dest is Destination.Cross) { "cross reference requires a cross destination" }
                val label = reader.string()
                if (kind == JniNodeKind.CROSS_EMBEDDED) {
                    consume(CrossEmbedded(dest, label, dimensions(), scope, anchor, attributes))
                } else {
                    consume(CrossLink(dest, label, scope, anchor, attributes))
                }
            }

            JniNodeKind.COMMENT -> {
                consume(Comment(reader.requiredString(), scope, anchor, attributes))
            }

            JniNodeKind.FORMULA -> {
                consume(Formula(placement(), reader.requiredString(), scope, anchor, attributes))
            }

            JniNodeKind.EMPHASIS -> {
                readChildren { consume(Emphasis(it, scope, anchor, attributes)) }
            }

            JniNodeKind.STRONG -> {
                readChildren { consume(Strong(it, scope, anchor, attributes)) }
            }

            JniNodeKind.STRIKETHROUGH -> {
                readChildren { consume(Strikethrough(it, scope, anchor, attributes)) }
            }

            JniNodeKind.MARK -> {
                readChildren { consume(Mark(it, scope, anchor, attributes)) }
            }

            JniNodeKind.INSERTION -> {
                readChildren { consume(Insertion(it, scope, anchor, attributes)) }
            }

            JniNodeKind.SPAN -> {
                readChildren { consume(Span(it, scope, anchor, attributes)) }
            }

            JniNodeKind.SUPERSCRIPT -> {
                readChildren { consume(Superscript(it, scope, anchor, attributes)) }
            }

            JniNodeKind.SUBSCRIPT -> {
                readChildren { consume(Subscript(it, scope, anchor, attributes)) }
            }

            JniNodeKind.LINK -> {
                val resource = resource()
                readChildren {
                    consume(
                        Link(
                            resource.dest,
                            resource.title,
                            it,
                            scope,
                            anchor ?: resource.anchor,
                            attributes.inheriting(resource.attributes),
                        ),
                    )
                }
            }

            JniNodeKind.MEDIA -> {
                val resource = resource()
                val dimensions = dimensions()
                readChildren {
                    consume(
                        Media(
                            resource.dest,
                            resource.title,
                            dimensions,
                            it,
                            scope,
                            anchor ?: resource.anchor,
                            attributes.inheriting(resource.attributes),
                        ),
                    )
                }
            }

            JniNodeKind.DIRECTIVE -> {
                readDirective(scope, anchor, attributes, consume)
            }

            JniNodeKind.CITE -> {
                readCitations { consume(Cite(it, scope, anchor, attributes)) }
            }

            JniNodeKind.TABLE_ROW -> {
                readTableRow(scope, anchor, attributes, consume)
            }

            JniNodeKind.TABLE_CELL -> {
                val rowspan = reader.long().toTableSpan()
                val colspan = reader.long().toTableSpan()
                readChildren { consume(TableCell(rowspan, colspan, it, scope, anchor, attributes)) }
            }

            JniNodeKind.DIRECTIVE_LABEL -> {
                readChildren { consume(DirectiveLabel(it, scope, anchor, attributes)) }
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
        anchor: String?,
        attributes: Attributes,
        consume: (Markup) -> Unit,
    ) {
        val metadata = metadata()
        var content: kotlin.collections.List<Markup>? = null
        var footnotes: kotlin.collections.List<Footnote>? = null
        actions.addLast {
            readValues("specimen", ::readSpecimen) { specimens ->
                consume(
                    Document(
                        requireNotNull(content),
                        metadata,
                        requireNotNull(footnotes),
                        specimens,
                        scope,
                        anchor,
                        attributes,
                    ),
                )
            }
        }
        actions.addLast { readValues("footnote", ::readFootnote) { footnotes = it } }
        actions.addLast { readChildren { content = it } }
    }

    private fun <T> readValues(
        name: String,
        read: ((T) -> Unit) -> Unit,
        consume: (kotlin.collections.List<T>) -> Unit,
    ) {
        val count = reader.int()
        require(count >= 0) { "invalid native $name count" }
        val values = MutableList<T?>(count) { null }
        actions.addLast {
            consume(immutableList(count) { requireNotNull(values[it]) { "JNI $name was not decoded" } })
        }
        for (index in count - 1 downTo 0) actions.addLast { read { values[index] = it } }
    }

    private fun readSpecimen(consume: (Specimen) -> Unit) {
        val scope = reader.scope()
        val id = reader.string()
        val startValue = reader.long()
        val start = if (reader.boolean()) startValue else null
        readChildren { consume(Specimen(id, start, it, scope)) }
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
                3 -> CitationReferent.Specimen(reader.requiredString())
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
        anchor: String?,
        attributes: Attributes,
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
                4 -> OrderedListVariant.Default
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
            consume(List(flavor, start, variant, delimiter, tight, items, scope, anchor, attributes))
        }
    }

    private fun readDirectiveBlock(
        scope: Scope,
        anchor: String?,
        attributes: Attributes,
        consume: (Markup) -> Unit,
    ) {
        val name = reader.requiredString()
        readDirectiveRelations { label, children ->
            consume(DirectiveBlock(name, label, children, scope, anchor, attributes))
        }
    }

    private fun readDirective(
        scope: Scope,
        anchor: String?,
        attributes: Attributes,
        consume: (Markup) -> Unit,
    ) {
        val name = reader.requiredString()
        readDirectiveRelations { label, children ->
            require(children.isEmpty()) { "inline directive contains block content" }
            consume(Directive(name, label, scope, anchor, attributes))
        }
    }

    /**
     * A callout's metadata leads, then its title -- a node-valued list that the
     * payload sends before the content, as the walk visits it, and whose count
     * is its presence because a present title holds at least one node -- and
     * then the content. An absent title has a zero count.
     */
    private fun readCallout(
        scope: Scope,
        anchor: String?,
        attributes: Attributes,
        consume: (Markup) -> Unit,
    ) {
        val variant = reader.string()
        val collapsed = reader.nullableBoolean()
        val titleCount = reader.int()
        require(titleCount >= 0) { "invalid native callout title count" }
        if (titleCount == 0) {
            readChildren { consume(Callout(variant, collapsed, null, it, scope, anchor, attributes)) }
            return
        }
        var title: kotlin.collections.List<Markup>? = null
        actions.addLast {
            readChildren { children ->
                consume(Callout(variant, collapsed, requireNotNull(title), children, scope, anchor, attributes))
            }
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

    private fun count(name: String): Int = reader.int().also { require(it >= 0) { "invalid native $name count" } }

    private fun attributes(): Attributes {
        val classes = immutableList(count("class")) { reader.requiredString() }
        val records = immutableList(count("record")) { Record(reader.requiredString(), reader.requiredString()) }
        require(classes.none { it.isEmpty() }) { "empty normalized class" }
        require(
            records.none { it.name.isEmpty() || it.name == "id" || it.name == "class" },
        ) { "invalid normalized record" }
        return Attributes(classes, records)
    }

    private fun metadata(): Metadata? {
        if (!reader.boolean()) return null
        val scope = reader.scope()
        return Metadata(
            name = metadataValue(),
            title = metadataValue(),
            subtitle = metadataValue(),
            time = metadataValue(),
            date = metadataValue(),
            authors = metadataValue(),
            keywords = metadataValue(),
            `abstract` = metadataValue(),
            state = metadataValue(),
            comment = metadataValue(),
            scope = scope,
        )
    }

    private fun metadataValue(): MetadataValue? {
        if (!reader.boolean()) return null
        return when (val branch = reader.byte().toInt()) {
            1 -> {
                MetadataValue.Scalar(
                    when (val kind = reader.byte().toInt()) {
                        0 -> MetadataScalar.Null
                        1 -> MetadataScalar.Bool(reader.boolean())
                        2 -> MetadataScalar.Number(reader.requiredString())
                        3 -> MetadataScalar.Text(reader.requiredString())
                        else -> error("invalid native metadata scalar $kind")
                    },
                )
            }

            2 -> {
                MetadataValue.List(
                    immutableList(count("metadata item")) {
                        when (val kind = reader.byte().toInt()) {
                            1 -> MetadataListItem.Number(reader.requiredString())
                            2 -> MetadataListItem.Text(reader.requiredString())
                            else -> error("invalid native metadata item $kind")
                        }
                    },
                )
            }

            else -> {
                error("invalid native metadata value $branch")
            }
        }
    }

    private fun dimensions(): Dimensions? =
        if (reader.boolean()) {
            Dimensions(reader.int(), if (reader.boolean()) reader.int() else null)
        } else {
            null
        }

    private fun readTable(
        scope: Scope,
        anchor: String?,
        attributes: Attributes,
        consume: (Markup) -> Unit,
    ) {
        val columnCount = reader.int()
        require(columnCount > 0) { "invalid native table column count" }
        val columns =
            immutableList(columnCount) {
                val alignment = tableAlignment(reader.byte().toInt() and 0xff)
                val relative = if (reader.boolean()) Double.fromBits(reader.long()) else null
                require(relative == null || (relative.isFinite() && relative > 0)) { "invalid table column width" }
                TableColumn(alignment, relative)
            }
        val head = reader.int()
        val content = reader.int()
        val foot = reader.int()
        require(head >= 0 && content >= 0 && foot >= 0) { "invalid table row groups" }
        readChildren { children ->
            require(head.toLong() + content + foot == children.size.toLong()) { "invalid table row groups" }
            val rows = children.immutableMap { requireNotNull(it as? TableRow) { "table contains a non-row node" } }
            consume(
                Table(
                    columns,
                    immutableList(head) { rows[it] },
                    immutableList(content) { rows[head + it] },
                    immutableList(foot) { rows[head + content + it] },
                    scope,
                    anchor,
                    attributes,
                ),
            )
        }
    }

    private fun Long.toTableSpan(): Int {
        require(this in 1..Int.MAX_VALUE.toLong()) { "invalid table cell span" }
        return toInt()
    }

    private fun readTableRow(
        scope: Scope,
        anchor: String?,
        attributes: Attributes,
        consume: (Markup) -> Unit,
    ) {
        readChildren { children ->
            val cells = children.immutableMap { requireNotNull(it as? TableCell) { "table row contains a non-cell" } }
            consume(TableRow(cells, scope, anchor, attributes))
        }
    }

    /**
     * Every occurrence of one reference definition shares one resource, and the
     * payload sends it once: a resource ordinal leads, and only the first
     * occurrence of an ordinal carries the destination and title, so a long
     * destination referenced many times crosses the boundary once and is
     * materialized once.
     */
    private val resources = ArrayList<DefinitionResource>()

    private fun resource(): DefinitionResource {
        val ordinal = reader.int()
        if (ordinal in resources.indices) return resources[ordinal]
        require(ordinal == resources.size) { "JNI payload names an unknown resource $ordinal" }
        val resource = DefinitionResource(destination(), reader.string(), reader.string(), attributes())
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
