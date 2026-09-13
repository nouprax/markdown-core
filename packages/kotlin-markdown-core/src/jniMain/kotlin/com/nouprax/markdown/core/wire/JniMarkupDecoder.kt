package com.nouprax.markdown.core

internal fun JniPayloadReader.document(): Document = Decoder(this).decode()

/** Decodes the depth-first JNI payload with heap-backed actions, never the JVM stack. */
private class Decoder(
    private val reader: JniPayloadReader,
) {
    private val actions = ArrayDeque<() -> Unit>()
    private var nodesStarted = 0

    fun decode(): Document {
        var root: Markup? = null
        actions.addLast { node { root = it } }
        while (actions.isNotEmpty()) actions.removeLast().invoke()
        require(reader.finished) { "JNI payload contains trailing data" }
        return requireNotNull(root as? Document) { "JNI payload contains an invalid document tree" }
    }

    private fun node(consume: (Markup) -> Unit) {
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
                document(scope, anchor, attributes, consume)
            }

            JniNodeKind.CALLOUT -> {
                callout(scope, anchor, attributes, consume)
            }

            JniNodeKind.PARAGRAPH -> {
                children { consume(Paragraph(it, scope, anchor, attributes)) }
            }

            JniNodeKind.HEADING -> {
                val level = reader.int()
                children { consume(Heading(level, it, scope, anchor, attributes)) }
            }

            JniNodeKind.THEMATIC_BREAK -> {
                consume(ThematicBreak(scope, anchor, attributes))
            }

            JniNodeKind.LIST -> {
                list(scope, anchor, attributes, consume)
            }

            JniNodeKind.LIST_ITEM -> {
                val marker = reader.string()
                children { consume(ListItem(marker, it, scope, anchor, attributes)) }
            }

            JniNodeKind.CODE_BLOCK -> {
                consume(
                    CodeBlock(
                        reader.string(),
                        reader.string(),
                        reader.required(),
                        reader.boolean(),
                        reader.boolean(),
                        scope,
                        anchor,
                        attributes,
                    ),
                )
            }

            JniNodeKind.HTML_BLOCK -> {
                consume(HTMLBlock(reader.required(), scope, anchor, attributes))
            }

            JniNodeKind.FORMULA_BLOCK -> {
                consume(FormulaBlock(reader.required(), scope, anchor, attributes))
            }

            JniNodeKind.TABLE -> {
                table(scope, anchor, attributes, consume)
            }

            JniNodeKind.DEFINITION_LIST -> {
                children { children ->
                    require(children.isNotEmpty()) { "empty definition list" }
                    val definitions =
                        immutableList(children.size) { index ->
                            val child = children[index]
                            require(child is Definition) { "invalid definition list child" }
                            child
                        }
                    consume(DefinitionList(definitions, scope, anchor, attributes))
                }
            }

            JniNodeKind.DEFINITION -> {
                val compact = reader.boolean()
                var term: kotlin.collections.List<Markup>? = null
                actions.addLast {
                    values("definition body", ::children) { bodies ->
                        require(bodies.isNotEmpty()) { "definition has no bodies" }
                        consume(Definition(requireNotNull(term), bodies, compact, scope, anchor, attributes))
                    }
                }
                actions.addLast { children { term = it } }
            }

            JniNodeKind.DIRECTIVE_BLOCK -> {
                directiveBlock(scope, anchor, attributes, consume)
            }

            JniNodeKind.TEXT -> {
                consume(Text(reader.required(), scope, anchor, attributes))
            }

            JniNodeKind.SOFT_BREAK -> {
                consume(SoftBreak(scope, anchor, attributes))
            }

            JniNodeKind.LINE_BREAK -> {
                consume(LineBreak(scope, anchor, attributes))
            }

            JniNodeKind.CODE -> {
                consume(Code(reader.required(), scope, anchor, attributes))
            }

            JniNodeKind.HTML -> {
                consume(HTML(reader.required(), scope, anchor, attributes))
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
                consume(Comment(reader.required(), scope, anchor, attributes))
            }

            JniNodeKind.FORMULA -> {
                consume(Formula(placement(), reader.required(), scope, anchor, attributes))
            }

            JniNodeKind.EMPHASIS -> {
                children { consume(Emphasis(it, scope, anchor, attributes)) }
            }

            JniNodeKind.STRONG -> {
                children { consume(Strong(it, scope, anchor, attributes)) }
            }

            JniNodeKind.STRIKETHROUGH -> {
                children { consume(Strikethrough(it, scope, anchor, attributes)) }
            }

            JniNodeKind.MARK -> {
                children { consume(Mark(it, scope, anchor, attributes)) }
            }

            JniNodeKind.INSERTION -> {
                children { consume(Insertion(it, scope, anchor, attributes)) }
            }

            JniNodeKind.SPAN -> {
                children { consume(Span(it, scope, anchor, attributes)) }
            }

            JniNodeKind.SUPERSCRIPT -> {
                children { consume(Superscript(it, scope, anchor, attributes)) }
            }

            JniNodeKind.SUBSCRIPT -> {
                children { consume(Subscript(it, scope, anchor, attributes)) }
            }

            JniNodeKind.LINK -> {
                val resource = resource()
                children {
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

            JniNodeKind.EMBEDDED -> {
                val resource = resource()
                val dimensions = dimensions()
                children {
                    consume(
                        Embedded(
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
                directive(scope, anchor, attributes, consume)
            }

            JniNodeKind.CITE -> {
                citations { consume(Cite(it, scope, anchor, attributes)) }
            }

            JniNodeKind.TABLE_CAPTION -> {
                children { consume(TableCaption(it, scope, anchor, attributes)) }
            }

            JniNodeKind.TABLE_ROW -> {
                tableRow(scope, anchor, attributes, consume)
            }

            JniNodeKind.TABLE_CELL -> {
                val rowspan = reader.long().toTableSpan()
                val colspan = reader.long().toTableSpan()
                children { consume(TableCell(rowspan, colspan, it, scope, anchor, attributes)) }
            }

            JniNodeKind.DIRECTIVE_LABEL -> {
                children { consume(DirectiveLabel(it, scope, anchor, attributes)) }
            }
        }
    }

    private fun children(consume: (kotlin.collections.List<Markup>) -> Unit) {
        val count = reader.int()
        require(count >= 0) { "invalid native child count" }
        nodes(count, consume)
    }

    /** Schedules `count` nodes and then the list they form, in payload order. */
    private fun nodes(
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
            actions.addLast { node { values[index] = it } }
        }
    }

    /**
     * The document's content leads, as the walk visits it; its footnotes
     * follow as a counted list of values, each its scope, its id, and its
     * content.
     */
    private fun document(
        scope: Scope,
        anchor: String?,
        attributes: Attributes,
        consume: (Markup) -> Unit,
    ) {
        val metadata = metadata()
        var content: kotlin.collections.List<Markup>? = null
        var footnotes: kotlin.collections.List<Footnote>? = null
        actions.addLast {
            values("specimen", ::specimen) { specimens ->
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
        actions.addLast { values("footnote", ::footnote) { footnotes = it } }
        actions.addLast { children { content = it } }
    }

    private fun <T> values(
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

    private fun specimen(consume: (Specimen) -> Unit) {
        val scope = reader.scope()
        val id = reader.string()
        val startValue = reader.long()
        val start = if (reader.boolean()) startValue else null
        children { consume(Specimen(id, start, it, scope)) }
    }

    private fun footnote(consume: (Footnote) -> Unit) {
        val scope = reader.scope()
        val id = reader.required()
        children { consume(Footnote(id, it, scope)) }
    }

    /**
     * A cite's items are a counted list of values, each its scope, its
     * referent -- the branch ordinal, then only that branch's fields -- and
     * its prefix and suffix content in that order.
     */
    private fun citations(consume: (kotlin.collections.List<Citation>) -> Unit) {
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
            actions.addLast { citation { values[index] = it } }
        }
    }

    private fun citation(consume: (Citation) -> Unit) {
        val scope = reader.scope()
        val referent =
            when (val branch = reader.byte().toInt()) {
                1 -> CitationReferent.Bib(reader.required(), bibMode())
                2 -> CitationReferent.Footnote(reader.required())
                3 -> CitationReferent.Specimen(reader.required())
                else -> error("invalid native citation referent $branch")
            }
        var prefix: kotlin.collections.List<Markup>? = null
        actions.addLast {
            children { suffix -> consume(Citation(referent, requireNotNull(prefix), suffix, scope)) }
        }
        actions.addLast {
            children { prefix = it }
        }
    }

    private fun bibMode(): BibMode =
        when (val rawValue = reader.int()) {
            1 -> BibMode.NORMAL
            2 -> BibMode.AUTHOR_IN_TEXT
            3 -> BibMode.SUPPRESS_AUTHOR
            else -> error("invalid native bib mode $rawValue")
        }

    private fun list(
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
        children { children ->
            val items = children.immutableMap { requireNotNull(it as? ListItem) { "list contains a non-item node" } }
            consume(List(flavor, start, variant, delimiter, tight, items, scope, anchor, attributes))
        }
    }

    private fun directiveBlock(
        scope: Scope,
        anchor: String?,
        attributes: Attributes,
        consume: (Markup) -> Unit,
    ) {
        val name = reader.string()
        relations { field, children ->
            val label = field?.let { requireNotNull(it as? DirectiveLabel) { "invalid directive label kind" } }
            consume(DirectiveBlock(name, label, children, scope, anchor, attributes))
        }
    }

    private fun directive(
        scope: Scope,
        anchor: String?,
        attributes: Attributes,
        consume: (Markup) -> Unit,
    ) {
        val name = reader.required()
        relations { field, children ->
            val label = field?.let { requireNotNull(it as? DirectiveLabel) { "invalid directive label kind" } }
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
    private fun callout(
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
            children { consume(Callout(variant, collapsed, null, it, scope, anchor, attributes)) }
            return
        }
        var title: kotlin.collections.List<Markup>? = null
        actions.addLast {
            children { children ->
                consume(Callout(variant, collapsed, requireNotNull(title), children, scope, anchor, attributes))
            }
        }
        actions.addLast {
            nodes(titleCount) { title = it }
        }
    }

    /** Reads a singular owned field before the ordinary child chain. */
    private fun relations(consume: (Markup?, kotlin.collections.List<Markup>) -> Unit) {
        if (!reader.boolean()) {
            children { consume(null, it) }
            return
        }
        var field: Markup? = null
        actions.addLast {
            children { children -> consume(requireNotNull(field), children) }
        }
        actions.addLast {
            node { node ->
                field = node
            }
        }
    }

    private fun count(name: String): Int = reader.int().also { require(it >= 0) { "invalid native $name count" } }

    private fun attributes(): Attributes {
        val classes = immutableList(count("class")) { reader.required() }
        val records = immutableList(count("record")) { Record(reader.required(), reader.required()) }
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
                        2 -> MetadataScalar.Number(reader.required())
                        3 -> MetadataScalar.Text(reader.required())
                        else -> error("invalid native metadata scalar $kind")
                    },
                )
            }

            2 -> {
                MetadataValue.List(
                    immutableList(count("metadata item")) {
                        when (val kind = reader.byte().toInt()) {
                            1 -> MetadataListItem.Number(reader.required())
                            2 -> MetadataListItem.Text(reader.required())
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

    private fun table(
        scope: Scope,
        anchor: String?,
        attributes: Attributes,
        consume: (Markup) -> Unit,
    ) {
        val columnCount = reader.int()
        require(columnCount > 0) { "invalid native table column count" }
        val columns =
            immutableList(columnCount) {
                val flow = flow(reader.byte().toInt() and 0xff)
                val relative = if (reader.boolean()) Double.fromBits(reader.long()) else null
                require(relative == null || (relative.isFinite() && relative > 0)) { "invalid table column width" }
                TableColumn(flow, relative)
            }
        val head = reader.int()
        val content = reader.int()
        val foot = reader.int()
        require(head >= 0 && content >= 0 && foot >= 0) { "invalid table row groups" }
        relations { field, children ->
            val caption = field?.let { requireNotNull(it as? TableCaption) { "invalid table caption kind" } }
            require(head.toLong() + content + foot == children.size.toLong()) { "invalid table row groups" }
            val rows = children.immutableMap { requireNotNull(it as? TableRow) { "table contains a non-row node" } }
            consume(
                Table(
                    caption,
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

    private fun tableRow(
        scope: Scope,
        anchor: String?,
        attributes: Attributes,
        consume: (Markup) -> Unit,
    ) {
        children { children ->
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
            1 -> Destination.Url(reader.required())
            2 -> Destination.Cross(reader.required(), reader.string())
            else -> error("invalid native destination kind $rawValue")
        }

    private fun placement(): Placement =
        when (val rawValue = reader.int()) {
            1 -> Placement.EMBEDDED
            2 -> Placement.STANDALONE
            else -> error("invalid native placement mode $rawValue")
        }

    private fun flow(rawValue: Int): Flow =
        when (rawValue) {
            0 -> Flow.NONE
            1 -> Flow.LEFT
            2 -> Flow.CENTER
            3 -> Flow.RIGHT
            else -> error("invalid native table flow $rawValue")
        }
}
