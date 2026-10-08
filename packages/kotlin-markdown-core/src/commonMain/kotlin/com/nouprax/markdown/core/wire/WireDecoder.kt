package com.nouprax.markdown.core

/**
 * The MCB3 reader (docs/architecture/wire-format.md), shared by every Kotlin
 * target. Records arrive in post-order, so each node is built bottom-up from
 * the nodes already on the stack, without recursion and without another call
 * into native code. The reader retains nothing of the message after [decode]
 * returns.
 */
internal object WireDecoder {
    /** Builds the document of [message], whose scope queries count columns in [unit]. */
    fun decode(
        message: ByteArray,
        unit: TextUnit,
    ): Document = Reader(message, unit).decode()

    /** Where the u8 status follows the magic and the u32 message length. */
    private const val STATUS_OFFSET = 8

    /** The status of a parse failure; the other status is a document. */
    private const val FAILURE = 1

    /** Each failure's code by the value of the facade's `markdown_core_status`. */
    private val errorCodes =
        mapOf(
            1 to ErrorCode.ALLOCATION_FAILED,
            2 to ErrorCode.OUT_OF_BOUNDS,
            3 to ErrorCode.KIND_MISMATCH,
            4 to ErrorCode.INSIDE_SCALAR,
        )

    // The contract's enums, in the order of their wire indices.
    private val flavors = listOf(ListFlavor.BULLET, ListFlavor.ORDERED)
    private val flows = listOf(Flow.NONE, Flow.LEFT, Flow.CENTER, Flow.RIGHT)
    private val placements = listOf(Placement.EMBEDDED, Placement.STANDALONE)
    private val bibModes = listOf(BibMode.NORMAL, BibMode.AUTHOR_IN_TEXT, BibMode.SUPPRESS_AUTHOR)

    private class Reader(
        private val bytes: ByteArray,
        private val unit: TextUnit,
    ) {
        private var offset = 0
        private val nodes = ArrayList<Markup>()

        /** Every footnote, specimen and reference built so far, by id, for the definition tables. */
        private val footnotes = HashMap<Long, Footnote>()
        private val specimens = HashMap<Long, Specimen>()
        private val references = HashMap<Long, Reference>()

        /** Every reference and heading built so far, by id, for the reference label table. */
        private val resolvable = HashMap<Long, Markup>()

        fun decode(): Document {
            offset = STATUS_OFFSET
            if (u8() == FAILURE) throw failure()
            while (offset < bytes.size) record()
            // The document's record is the last, and it takes every node before it.
            return nodes[0] as Document
        }

        /** A failure's body is its `u32` status code alone. */
        private fun failure(): MarkdownCoreException = MarkdownCoreException(errorCodes.getValue(count()))

        // ---- Records -----------------------------------------------------------

        /** Reads one record, replaces the nodes it names with the node it builds. */
        private fun record() {
            val kind = WireNodeKind.from(u8())
            val id = MarkupID(int())
            val extent = Extent(i32(), u32().toUInt())
            val runs = list { Run(Extent(i32(), u32().toUInt()), u32().toUInt()) }
            val anchor = optional { string() }
            val attributes = attributes()
            val children = Children(nodes.size)
            val node = children.fields(kind, id, extent, runs, anchor, attributes)
            if (node is Footnote) footnotes[id.value] = node
            if (node is Specimen) specimens[id.value] = node
            if (node is Reference) references[id.value] = node
            if (node is Reference || node is Heading) resolvable[id.value] = node
            nodes.subList(children.start, nodes.size).clear()
            nodes += node
        }

        /** A kind's fields in the contract's order; node-valued fields read their counts. */
        private fun Children.fields(
            kind: WireNodeKind,
            id: MarkupID,
            extent: Extent,
            runs: kotlin.collections.List<Run>,
            anchor: String?,
            attributes: Attributes,
        ): Markup =
            when (kind) {
                WireNodeKind.DOCUMENT -> {
                    val content = count()
                    val metadata = presence()
                    take(content, metadata)
                    // The document's record is the last; its definition tables follow it.
                    Document(
                        content(content),
                        optional(metadata),
                        unit,
                        table(footnotes),
                        table(specimens),
                        table(references),
                        labels(),
                        id,
                        extent,
                        runs,
                        anchor,
                        attributes,
                    )
                }

                WireNodeKind.CALLOUT -> {
                    val variant = optional { string() }
                    val collapsed = optional { bool() }
                    val title = optional { count() }
                    val content = count()
                    take(title ?: 0, content)
                    Callout(
                        variant,
                        collapsed,
                        title?.let { content(it) },
                        content(content),
                        id,
                        extent,
                        runs,
                        anchor,
                        attributes,
                    )
                }

                WireNodeKind.PARAGRAPH -> {
                    Paragraph(content(), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.HEADING -> {
                    val level = int().toInt()
                    Heading(level, content(), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.THEMATIC_BREAK -> {
                    ThematicBreak(id, extent, runs, anchor, attributes)
                }

                WireNodeKind.LIST -> {
                    val flavor = index(flavors)
                    val start = optional { int() }
                    val variant = optional { variant() }
                    val delimiter = optional { delimiter() }
                    val tight = bool()
                    val items = count()
                    take(items)
                    List(
                        flavor,
                        start,
                        variant,
                        delimiter,
                        tight,
                        typed(items),
                        id,
                        extent,
                        runs,
                        anchor,
                        attributes,
                    )
                }

                WireNodeKind.LIST_ITEM -> {
                    val marker = optional { string() }
                    ListItem(marker, content(), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.CODE_BLOCK -> {
                    CodeBlock(
                        optional { string() },
                        optional { string() },
                        string(),
                        bool(),
                        bool(),
                        id,
                        extent,
                        runs,
                        anchor,
                        attributes,
                    )
                }

                WireNodeKind.HTML_BLOCK -> {
                    HTMLBlock(string(), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.FORMULA_BLOCK -> {
                    FormulaBlock(string(), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.TABLE -> {
                    val caption = presence()
                    val columns = list { TableColumn(index(flows), optional { double() }) }
                    val head = count()
                    val content = count()
                    val foot = count()
                    take(caption, head, content, foot)
                    Table(
                        optional(caption),
                        columns,
                        typed(head),
                        typed(content),
                        typed(foot),
                        id,
                        extent,
                        runs,
                        anchor,
                        attributes,
                    )
                }

                WireNodeKind.DIRECTIVE_BLOCK -> {
                    val name = optional { string() }
                    val label = presence()
                    val content = count()
                    take(label, content)
                    DirectiveBlock(
                        name,
                        optional(label),
                        content(content),
                        id,
                        extent,
                        runs,
                        anchor,
                        attributes,
                    )
                }

                WireNodeKind.TEXT -> {
                    Text(string(), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.SOFT_BREAK -> {
                    SoftBreak(id, extent, runs, anchor, attributes)
                }

                WireNodeKind.LINE_BREAK -> {
                    LineBreak(id, extent, runs, anchor, attributes)
                }

                WireNodeKind.CODE -> {
                    Code(string(), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.HTML -> {
                    HTML(string(), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.FORMULA -> {
                    Formula(index(placements), string(), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.EMPHASIS -> {
                    Emphasis(content(), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.STRONG -> {
                    Strong(content(), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.STRIKETHROUGH -> {
                    Strikethrough(content(), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.LINK -> {
                    Link(destination(), optional { string() }, content(), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.EMBEDDED -> {
                    Embedded(
                        destination(),
                        optional { string() },
                        optional { dimensions() },
                        content(),
                        id,
                        extent,
                        runs,
                        anchor,
                        attributes,
                    )
                }

                WireNodeKind.DIRECTIVE -> {
                    val name = string()
                    val label = presence()
                    take(label)
                    Directive(name, optional(label), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.CITE -> {
                    val citations = count()
                    take(citations)
                    Cite(typed(citations), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.TABLE_ROW -> {
                    val cells = count()
                    take(cells)
                    TableRow(typed(cells), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.TABLE_CELL -> {
                    val rowspan = int().toInt()
                    val colspan = int().toInt()
                    TableCell(rowspan, colspan, content(), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.DIRECTIVE_LABEL -> {
                    DirectiveLabel(content(), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.COMMENT -> {
                    Comment(string(), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.CROSS_LINK -> {
                    CrossLink(cross(), optional { string() }, id, extent, runs, anchor, attributes)
                }

                WireNodeKind.MARK -> {
                    Mark(content(), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.CROSS_EMBEDDED -> {
                    CrossEmbedded(
                        cross(),
                        optional { string() },
                        optional { dimensions() },
                        id,
                        extent,
                        runs,
                        anchor,
                        attributes,
                    )
                }

                WireNodeKind.INSERTION -> {
                    Insertion(content(), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.SPAN -> {
                    Span(content(), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.SUPERSCRIPT -> {
                    Superscript(content(), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.SUBSCRIPT -> {
                    Subscript(content(), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.DEFINITION_LIST -> {
                    val definitions = count()
                    take(definitions)
                    DefinitionList(typed(definitions), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.DEFINITION -> {
                    val term = count()
                    val bodies = list { count() }
                    val compact = bool()
                    take(term, *bodies.toIntArray())
                    Definition(
                        content(term),
                        bodies.map { content(it) },
                        compact,
                        id,
                        extent,
                        runs,
                        anchor,
                        attributes,
                    )
                }

                WireNodeKind.TABLE_CAPTION -> {
                    TableCaption(content(), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.CITATION -> {
                    // An inline note writes nothing for its footnote, which is
                    // the first node the record takes, ahead of the affixes.
                    val written = referent()
                    val prefix = count()
                    val suffix = count()
                    take(if (written == null) 1 else 0, prefix, suffix)
                    val referent = written ?: CitationReferent.Footnote(FootnoteTarget.Note(typed<Footnote>(1)[0]))
                    Citation(referent, content(prefix), content(suffix), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.FOOTNOTE -> {
                    val label = optional { string() }
                    Footnote(label, content(), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.SPECIMEN -> {
                    val label = optional { string() }
                    val start = optional { int() }
                    Specimen(label, start, content(), id, extent, runs, anchor, attributes)
                }

                WireNodeKind.METADATA -> {
                    Metadata(
                        name = optional { metadataValue() },
                        title = optional { metadataValue() },
                        subtitle = optional { metadataValue() },
                        time = optional { metadataValue() },
                        date = optional { metadataValue() },
                        authors = optional { metadataValue() },
                        keywords = optional { metadataValue() },
                        `abstract` = optional { metadataValue() },
                        state = optional { metadataValue() },
                        comment = optional { metadataValue() },
                        id = id,
                        extent = extent,
                        runs = runs,
                        anchor = anchor,
                        attributes = attributes,
                    )
                }

                WireNodeKind.REFERENCE -> {
                    Reference(
                        string(),
                        destination(),
                        optional { string() },
                        id,
                        extent,
                        runs,
                        anchor,
                        attributes,
                    )
                }
            }

        /** One record's nodes: the top of the stack, handed to its fields in order. */
        private inner class Children(
            var start: Int,
        ) {
            private var next = start

            /** Claims the nodes of every node-valued field, whose counts sum to [counts]. */
            fun take(vararg counts: Int) {
                start = nodes.size - counts.sum()
                next = start
            }

            /** A record whose only node-valued field is `content: [Markup]`. */
            fun content(): kotlin.collections.List<Markup> {
                val count = count()
                take(count)
                return content(count)
            }

            fun content(count: Int): kotlin.collections.List<Markup> = typed(count)

            /** The next [count] nodes, of the class the engine writes in a field of their type. */
            @Suppress("UNCHECKED_CAST")
            fun <T : Markup> typed(count: Int): kotlin.collections.List<T> {
                val first = next
                next += count
                return nodes.subList(first, next).toList() as kotlin.collections.List<T>
            }

            fun <T : Markup> optional(count: Int): T? = if (count == 0) null else typed<T>(1)[0]
        }

        // ---- Definition tables -------------------------------------------------

        /** A `u32` count and that many `u64` ids, each naming one of [definitions]. */
        private fun <T : Markup> table(definitions: Map<Long, T>): kotlin.collections.List<T> =
            list { definitions.getValue(int()) }

        /** A `u32` count and that many labels, each with the `u64` id of the reference or heading it resolves to. */
        private fun labels(): Map<String, Markup> = list { string() to resolvable.getValue(int()) }.toMap()

        // ---- Values ------------------------------------------------------------

        private fun attributes(): Attributes {
            val classes = list { string() }
            val records = list { Record(string(), string()) }
            return if (classes.isEmpty() && records.isEmpty()) Attributes.empty else Attributes(classes, records)
        }

        private fun destination(): Destination =
            when (u8()) {
                0 -> Destination.Url(string())
                1 -> Destination.Cross(string(), optional { string() })
                else -> Destination.Reference(string())
            }

        private fun cross(): Destination.Cross = destination() as Destination.Cross

        /** The referent, or null for an inline note, whose footnote is a node of the record. */
        private fun referent(): CitationReferent? =
            when (u8()) {
                0 -> CitationReferent.Bib(string(), index(bibModes))
                1 -> if (u8() == 0) CitationReferent.Footnote(FootnoteTarget.Label(string())) else null
                else -> CitationReferent.Specimen(string())
            }

        private fun variant(): OrderedListVariant =
            when (u8()) {
                0 -> OrderedListVariant.Decimal
                1 -> OrderedListVariant.Alpha(bool())
                2 -> OrderedListVariant.Roman(bool())
                else -> OrderedListVariant.Default
            }

        private fun delimiter(): OrderedListDelimiter =
            when (u8()) {
                0 -> OrderedListDelimiter.Period
                1 -> OrderedListDelimiter.Parenthesis(bool())
                else -> OrderedListDelimiter.Default
            }

        private fun dimensions(): Dimensions = Dimensions(int().toInt(), optional { int().toInt() })

        private fun metadataValue(): MetadataValue =
            when (u8()) {
                0 -> MetadataValue.Scalar(metadataScalar())
                else -> MetadataValue.List(list { metadataListItem() })
            }

        private fun metadataScalar(): MetadataScalar =
            when (u8()) {
                0 -> MetadataScalar.Null
                1 -> MetadataScalar.Bool(bool())
                2 -> MetadataScalar.Number(string())
                else -> MetadataScalar.Text(string())
            }

        private fun metadataListItem(): MetadataListItem =
            when (u8()) {
                0 -> MetadataListItem.Number(string())
                else -> MetadataListItem.Text(string())
            }

        // ---- Primitives --------------------------------------------------------

        private inline fun <T> optional(read: () -> T): T? = if (bool()) read() else null

        /** The node count of a `K?` field. */
        private fun presence(): Int = if (bool()) 1 else 0

        private inline fun <T> list(read: () -> T): kotlin.collections.List<T> =
            kotlin.collections.List(count()) { read() }

        /** A u32 count; no message holds more nodes or items than an Int counts. */
        private fun count(): Int = u32().toInt()

        private fun <T> index(values: kotlin.collections.List<T>): T = values[u8()]

        private fun bool(): Boolean = u8() == 1

        private fun string(): String {
            val length = count()
            val start = take(length)
            return bytes.decodeToString(start, start + length)
        }

        private fun int(): Long = little(take(Long.SIZE_BYTES), Long.SIZE_BYTES)

        private fun double(): Double = Double.fromBits(int())

        private fun u8(): Int = bytes[take(1)].toInt() and 0xff

        private fun u32(): Long = little(take(Int.SIZE_BYTES), Int.SIZE_BYTES)

        private fun i32(): Int = u32().toInt()

        private fun little(
            start: Int,
            size: Int,
        ): Long {
            var value = 0L
            for (index in 0 until size) value = value or ((bytes[start + index].toLong() and 0xff) shl (index * 8))
            return value
        }

        /** Advances past [length] bytes and returns where they start. */
        private fun take(length: Int): Int {
            val start = offset
            offset = start + length
            return start
        }
    }
}
