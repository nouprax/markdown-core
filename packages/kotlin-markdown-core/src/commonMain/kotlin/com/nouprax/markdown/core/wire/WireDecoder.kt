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

    private val magic = byteArrayOf(0x4d, 0x43, 0x42, 0x33)

    /** The magic, the u32 message length and the u8 status. */
    private const val HEADER_SIZE = 9

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
        private val kinds = ArrayList<WireNodeKind>()
        private val resources = ArrayList<DefinitionResource>()

        /** Every footnote and specimen built so far, by id, for the definition tables. */
        private val footnotes = HashMap<Long, Footnote>()
        private val specimens = HashMap<Long, Specimen>()

        fun decode(): Document {
            header()
            while (offset < bytes.size) record()
            val document = nodes.singleOrNull()
            require(document is Document) { "native result is not one document tree" }
            return document
        }

        private fun header() {
            require(bytes.size >= HEADER_SIZE) { "truncated native result header" }
            magic.forEachIndexed { index, expected ->
                val actual = u8()
                require(actual == expected.toInt()) {
                    "invalid native result at byte $index: expected $expected, got $actual"
                }
            }
            require(u32() == bytes.size.toLong()) { "native result length does not match its header" }
            val status = u8()
            if (status == 1) throw failure()
            require(status == 0) { "unsupported native result status $status" }
        }

        private fun failure(): ParseException {
            val code =
                when (u32()) {
                    1L -> ParseErrorCode.INVALID_ARGUMENT
                    2L -> ParseErrorCode.ALLOCATION_FAILED
                    else -> ParseErrorCode.INTERNAL
                }
            val message = string()
            require(offset == bytes.size) { "invalid native result error payload" }
            return ParseException(code, message)
        }

        // ---- Records -----------------------------------------------------------

        /** Reads one record, replaces the nodes it names with the node it builds. */
        private fun record() {
            val kind = WireNodeKind.from(u8())
            val id = MarkupID(id())
            val extent = Extent(i32(), u32().toUInt())
            val anchor = optional { string() }
            val attributes = attributes()
            val children = Children(nodes.size)
            val node = children.fields(kind, id, extent, anchor, attributes)
            if (node is Footnote) footnotes[id.value] = node
            if (node is Specimen) specimens[id.value] = node
            nodes.subList(children.start, nodes.size).clear()
            kinds.subList(children.start, kinds.size).clear()
            nodes += node
            kinds += kind
        }

        /** A kind's fields in the contract's order; node-valued fields read their counts. */
        private fun Children.fields(
            kind: WireNodeKind,
            id: MarkupID,
            extent: Extent,
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
                        optional<Metadata>(WireNodeKind.METADATA, metadata),
                        unit,
                        table(footnotes, WireNodeKind.FOOTNOTE),
                        table(specimens, WireNodeKind.SPECIMEN),
                        id,
                        extent,
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
                        anchor,
                        attributes,
                    )
                }

                WireNodeKind.PARAGRAPH -> {
                    Paragraph(content(), id, extent, anchor, attributes)
                }

                WireNodeKind.HEADING -> {
                    val level = int().toInt32("heading level")
                    Heading(level, content(), id, extent, anchor, attributes)
                }

                WireNodeKind.THEMATIC_BREAK -> {
                    ThematicBreak(id, extent, anchor, attributes)
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
                        typed(WireNodeKind.LIST_ITEM, items),
                        id,
                        extent,
                        anchor,
                        attributes,
                    )
                }

                WireNodeKind.LIST_ITEM -> {
                    val marker = optional { string() }
                    ListItem(marker, content(), id, extent, anchor, attributes)
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
                        anchor,
                        attributes,
                    )
                }

                WireNodeKind.HTML_BLOCK -> {
                    HTMLBlock(string(), id, extent, anchor, attributes)
                }

                WireNodeKind.FORMULA_BLOCK -> {
                    FormulaBlock(string(), id, extent, anchor, attributes)
                }

                WireNodeKind.TABLE -> {
                    val caption = presence()
                    val columns = list { TableColumn(index(flows), optional { double() }) }
                    val head = count()
                    val content = count()
                    val foot = count()
                    take(caption, head, content, foot)
                    Table(
                        optional(WireNodeKind.TABLE_CAPTION, caption),
                        columns,
                        typed(WireNodeKind.TABLE_ROW, head),
                        typed(WireNodeKind.TABLE_ROW, content),
                        typed(WireNodeKind.TABLE_ROW, foot),
                        id,
                        extent,
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
                        optional(WireNodeKind.DIRECTIVE_LABEL, label),
                        content(content),
                        id,
                        extent,
                        anchor,
                        attributes,
                    )
                }

                WireNodeKind.TEXT -> {
                    Text(string(), id, extent, anchor, attributes)
                }

                WireNodeKind.SOFT_BREAK -> {
                    SoftBreak(id, extent, anchor, attributes)
                }

                WireNodeKind.LINE_BREAK -> {
                    LineBreak(id, extent, anchor, attributes)
                }

                WireNodeKind.CODE -> {
                    Code(string(), id, extent, anchor, attributes)
                }

                WireNodeKind.HTML -> {
                    HTML(string(), id, extent, anchor, attributes)
                }

                WireNodeKind.FORMULA -> {
                    Formula(index(placements), string(), id, extent, anchor, attributes)
                }

                WireNodeKind.EMPHASIS -> {
                    Emphasis(content(), id, extent, anchor, attributes)
                }

                WireNodeKind.STRONG -> {
                    Strong(content(), id, extent, anchor, attributes)
                }

                WireNodeKind.STRIKETHROUGH -> {
                    Strikethrough(content(), id, extent, anchor, attributes)
                }

                WireNodeKind.LINK -> {
                    val resource = resource()
                    Link(
                        resource.dest,
                        resource.title,
                        content(),
                        id,
                        extent,
                        anchor ?: resource.anchor,
                        attributes.inheriting(resource.attributes),
                    )
                }

                WireNodeKind.EMBEDDED -> {
                    val resource = resource()
                    val dimensions = optional { dimensions() }
                    Embedded(
                        resource.dest,
                        resource.title,
                        dimensions,
                        content(),
                        id,
                        extent,
                        anchor ?: resource.anchor,
                        attributes.inheriting(resource.attributes),
                    )
                }

                WireNodeKind.DIRECTIVE -> {
                    val name = string()
                    val label = presence()
                    take(label)
                    Directive(name, optional(WireNodeKind.DIRECTIVE_LABEL, label), id, extent, anchor, attributes)
                }

                WireNodeKind.CITE -> {
                    val citations = count()
                    take(citations)
                    Cite(typed(WireNodeKind.CITATION, citations), id, extent, anchor, attributes)
                }

                WireNodeKind.TABLE_ROW -> {
                    val cells = count()
                    take(cells)
                    TableRow(typed(WireNodeKind.TABLE_CELL, cells), id, extent, anchor, attributes)
                }

                WireNodeKind.TABLE_CELL -> {
                    val rowspan = int().toInt32("table cell rowspan")
                    val colspan = int().toInt32("table cell colspan")
                    TableCell(rowspan, colspan, content(), id, extent, anchor, attributes)
                }

                WireNodeKind.DIRECTIVE_LABEL -> {
                    DirectiveLabel(content(), id, extent, anchor, attributes)
                }

                WireNodeKind.COMMENT -> {
                    Comment(string(), id, extent, anchor, attributes)
                }

                WireNodeKind.CROSS_LINK -> {
                    CrossLink(cross(), optional { string() }, id, extent, anchor, attributes)
                }

                WireNodeKind.MARK -> {
                    Mark(content(), id, extent, anchor, attributes)
                }

                WireNodeKind.CROSS_EMBEDDED -> {
                    CrossEmbedded(
                        cross(),
                        optional { string() },
                        optional { dimensions() },
                        id,
                        extent,
                        anchor,
                        attributes,
                    )
                }

                WireNodeKind.INSERTION -> {
                    Insertion(content(), id, extent, anchor, attributes)
                }

                WireNodeKind.SPAN -> {
                    Span(content(), id, extent, anchor, attributes)
                }

                WireNodeKind.SUPERSCRIPT -> {
                    Superscript(content(), id, extent, anchor, attributes)
                }

                WireNodeKind.SUBSCRIPT -> {
                    Subscript(content(), id, extent, anchor, attributes)
                }

                WireNodeKind.DEFINITION_LIST -> {
                    val definitions = count()
                    take(definitions)
                    DefinitionList(typed(WireNodeKind.DEFINITION, definitions), id, extent, anchor, attributes)
                }

                WireNodeKind.DEFINITION -> {
                    val term = count()
                    val bodies = list { count() }
                    val compact = bool()
                    take(term, *bodies.toIntArray())
                    Definition(
                        content(term),
                        bodies.immutableMap { content(it) },
                        compact,
                        id,
                        extent,
                        anchor,
                        attributes,
                    )
                }

                WireNodeKind.TABLE_CAPTION -> {
                    TableCaption(content(), id, extent, anchor, attributes)
                }

                WireNodeKind.CITATION -> {
                    // An inline note writes nothing for its footnote, which is
                    // the first node the record takes, ahead of the affixes.
                    val written = referent()
                    val prefix = count()
                    val suffix = count()
                    take(if (written == null) 1 else 0, prefix, suffix)
                    val note = if (written == null) typed<Footnote>(WireNodeKind.FOOTNOTE, 1)[0] else null
                    val referent = written ?: CitationReferent.Footnote(FootnoteTarget.Note(checkNotNull(note)))
                    Citation(referent, content(prefix), content(suffix), id, extent, anchor, attributes)
                }

                WireNodeKind.FOOTNOTE -> {
                    val label = optional { string() }
                    Footnote(label, content(), id, extent, anchor, attributes)
                }

                WireNodeKind.SPECIMEN -> {
                    val label = optional { string() }
                    val start = optional { int() }
                    Specimen(label, start, content(), id, extent, anchor, attributes)
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
                        anchor = anchor,
                        attributes = attributes,
                    )
                }
            }

        /**
         * One record's nodes: the top of the stack, handed to its fields in order.
         * Each field checks the kinds it accepts.
         */
        private inner class Children(
            var start: Int,
        ) {
            private var next = start

            /** Claims the nodes of every node-valued field, whose counts sum to [counts]. */
            fun take(vararg counts: Int) {
                val total = counts.fold(0L) { sum, count -> sum + count }
                require(total <= nodes.size) { "native result record names more nodes than precede it" }
                start = nodes.size - total.toInt()
                next = start
            }

            /** A record whose only node-valued field is `content: [Markup]`. */
            fun content(): kotlin.collections.List<Markup> {
                val count = count()
                take(count)
                return content(count)
            }

            fun content(count: Int): kotlin.collections.List<Markup> = claim(count, "content") { it.content }

            /** A field typed with [kind]; the kind check is what makes the cast to its class safe. */
            @Suppress("UNCHECKED_CAST")
            fun <T : Markup> typed(
                kind: WireNodeKind,
                count: Int,
            ): kotlin.collections.List<T> = claim(count, kind.name) { it == kind } as kotlin.collections.List<T>

            fun <T : Markup> optional(
                kind: WireNodeKind,
                count: Int,
            ): T? = if (count == 0) null else typed<T>(kind, 1)[0]

            private inline fun claim(
                count: Int,
                field: String,
                accepts: (WireNodeKind) -> Boolean,
            ): kotlin.collections.List<Markup> {
                val first = next
                next += count
                for (index in first until next) {
                    require(accepts(kinds[index])) { "native result places a ${kinds[index]} node in a $field field" }
                }
                return slice(first, count)
            }

            private fun slice(
                first: Int,
                count: Int,
            ): kotlin.collections.List<Markup> = immutableList(count) { nodes[first + it] }
        }

        // ---- Definition tables -------------------------------------------------

        /** A `u32` count and that many `u64` ids, each naming a node of [kind] built before. */
        private fun <T : Markup> table(
            definitions: Map<Long, T>,
            kind: WireNodeKind,
        ): kotlin.collections.List<T> =
            list {
                val id = id()
                requireNotNull(definitions[id]) { "native result names no $kind node with id $id" }
            }.immutableMap { it }

        // ---- Shared resources --------------------------------------------------

        private fun resource(): DefinitionResource {
            val ordinal = u32()
            if (ordinal < resources.size) return resources[ordinal.toInt()]
            require(ordinal == resources.size.toLong()) { "native result names unknown resource $ordinal" }
            val resource = DefinitionResource(destination(), optional { string() }, optional { string() }, attributes())
            resources += resource
            return resource
        }

        // ---- Values ------------------------------------------------------------

        private fun attributes(): Attributes {
            val classes = list { string() }
            val records = list { Record(string(), string()) }
            return if (classes.isEmpty() && records.isEmpty()) Attributes.empty else Attributes(classes, records)
        }

        private fun destination(): Destination =
            when (branch(2)) {
                0 -> Destination.Url(string())
                else -> Destination.Cross(string(), optional { string() })
            }

        private fun cross(): Destination.Cross {
            val destination = destination()
            require(destination is Destination.Cross) { "native result gives a cross reference a URL destination" }
            return destination
        }

        /** The referent, or null for an inline note, whose footnote is a node of the record. */
        private fun referent(): CitationReferent? =
            when (branch(3)) {
                0 -> CitationReferent.Bib(string(), index(bibModes))
                1 -> if (branch(2) == 0) CitationReferent.Footnote(FootnoteTarget.Label(string())) else null
                else -> CitationReferent.Specimen(string())
            }

        private fun variant(): OrderedListVariant =
            when (branch(4)) {
                0 -> OrderedListVariant.Decimal
                1 -> OrderedListVariant.Alpha(bool())
                2 -> OrderedListVariant.Roman(bool())
                else -> OrderedListVariant.Default
            }

        private fun delimiter(): OrderedListDelimiter =
            when (branch(3)) {
                0 -> OrderedListDelimiter.Period
                1 -> OrderedListDelimiter.Parenthesis(bool())
                else -> OrderedListDelimiter.Default
            }

        private fun dimensions(): Dimensions =
            Dimensions(int().toInt32("dimension width"), optional { int().toInt32("dimension height") })

        private fun metadataValue(): MetadataValue =
            when (branch(2)) {
                0 -> MetadataValue.Scalar(metadataScalar())
                else -> MetadataValue.List(list { metadataListItem() })
            }

        private fun metadataScalar(): MetadataScalar =
            when (branch(4)) {
                0 -> MetadataScalar.Null
                1 -> MetadataScalar.Bool(bool())
                2 -> MetadataScalar.Number(string())
                else -> MetadataScalar.Text(string())
            }

        private fun metadataListItem(): MetadataListItem =
            when (branch(2)) {
                0 -> MetadataListItem.Number(string())
                else -> MetadataListItem.Text(string())
            }

        // ---- Primitives --------------------------------------------------------

        private inline fun <T> optional(read: () -> T): T? = if (bool()) read() else null

        /** The node count of a `K?` field. */
        private fun presence(): Int = if (bool()) 1 else 0

        private inline fun <T> list(read: () -> T): kotlin.collections.List<T> {
            val count = count()
            // Every item occupies at least one byte, so a count the message cannot
            // hold is rejected before anything is allocated for it.
            require(count <= bytes.size - offset) { "native result count exceeds the message" }
            val items = ArrayList<T>(count)
            repeat(count) { items += read() }
            return items
        }

        /** A u32 count; no message can hold more nodes or items than an Int counts. */
        private fun count(): Int {
            val count = u32()
            require(count <= Int.MAX_VALUE) { "native result count exceeds the message" }
            return count.toInt()
        }

        private fun branch(count: Int): Int {
            val branch = u8()
            require(branch < count) { "native result contains invalid branch $branch" }
            return branch
        }

        private fun <T> index(values: kotlin.collections.List<T>): T {
            val index = u8()
            require(index < values.size) { "native result contains invalid enum index $index" }
            return values[index]
        }

        private fun bool(): Boolean {
            val value = u8()
            require(value <= 1) { "native result contains invalid boolean $value" }
            return value == 1
        }

        private fun string(): String {
            val length = count()
            val start = take(length)
            return bytes.decodeToString(start, start + length)
        }

        private fun int(): Long = little(take(Long.SIZE_BYTES), Long.SIZE_BYTES)

        /** A u64 node id, which the model holds in a Long. */
        private fun id(): Long {
            val id = int()
            require(id >= 0) { "native node id exceeds a 64-bit signed integer" }
            return id
        }

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
            require(length <= bytes.size - start) { "truncated native result" }
            offset = start + length
            return start
        }

        /** An `Int` field whose Kotlin model is a 32-bit integer. */
        private fun Long.toInt32(field: String): Int {
            require(this in Int.MIN_VALUE..Int.MAX_VALUE) { "native $field exceeds a 32-bit integer" }
            return toInt()
        }
    }
}
