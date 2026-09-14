package com.nouprax.markdown.core

internal fun PayloadReader.document(): Document = Decoder(this).decode()

private const val INITIAL_FRAMES = 32
private const val OBJECT_SLOTS = 4
private const val NUMBER_SLOTS = 3

// What the list a frame is filling admits, checked as each node's kind is
// read, before its fields are: a list of one shape holds nodes of one class,
// so the full array is that list by a cast, not by a check per element.
private const val EXPECT_ROOT = 0
private const val EXPECT_CONTENT = 1
private const val EXPECT_METADATA = 2
private const val EXPECT_FOOTNOTE = 3
private const val EXPECT_SPECIMEN = 4
private const val EXPECT_CITATION = 5
private const val EXPECT_LIST_ITEM = 6
private const val EXPECT_TABLE_ROW = 7
private const val EXPECT_TABLE_CELL = 8
private const val EXPECT_DEFINITION = 9
private const val EXPECT_TABLE_CAPTION = 10
private const val EXPECT_DIRECTIVE_LABEL = 11

private val noElements: Array<Any?> = arrayOfNulls(0)

/**
 * Decodes the depth-first payload with one explicit stack of frames, never
 * the call stack and never a closure per node.
 *
 * A leaf becomes its node as its fields are read. A container is a frame --
 * its header and scalars in parallel arrays -- that reads its lists in
 * payload order: each list is an array the nodes that follow fill, and the
 * frame becomes its node when its last list is full. A node therefore costs
 * the node, its scope, and the arrays its lists wrap once.
 */
private class Decoder(
    private val reader: PayloadReader,
) {
    private var kinds = arrayOfNulls<PayloadNodeKind>(INITIAL_FRAMES)
    private var phases = IntArray(INITIAL_FRAMES)
    private var scopes = arrayOfNulls<Scope>(INITIAL_FRAMES)
    private var anchors = arrayOfNulls<String>(INITIAL_FRAMES)
    private var attributes = arrayOfNulls<Attributes>(INITIAL_FRAMES)
    private var objects = arrayOfNulls<Any>(INITIAL_FRAMES * OBJECT_SLOTS)
    private var numbers = LongArray(INITIAL_FRAMES * NUMBER_SLOTS)

    // The list each frame is filling: its elements, how many are in, and
    // what it admits.
    private var elements = arrayOfNulls<Array<Any?>>(INITIAL_FRAMES)
    private var filled = IntArray(INITIAL_FRAMES)
    private var expectations = IntArray(INITIAL_FRAMES)
    private var depth = 0

    private var nodesStarted = 0
    private var root: Markup? = null

    fun decode(): Document {
        // The root frame holds the one document as a list of one.
        elements[0] = arrayOfNulls(1)
        expectations[0] = EXPECT_ROOT
        depth = 1
        while (depth > 0) {
            node()
            settle()
        }
        require(reader.finished) { "payload contains trailing data" }
        return requireNotNull(root as? Document) { "payload contains an invalid document tree" }
    }

    /** Reads one node: a leaf into the open list, a container as a new frame with its first list open. */
    private fun node() {
        val kind = reader.kind()
        val isRoot = nodesStarted++ == 0
        require((kind == PayloadNodeKind.DOCUMENT) == isRoot) {
            "payload must contain exactly one document at its root"
        }
        expect(expectations[depth - 1], kind)
        val scope = Scope(reader.int(), reader.int(), reader.int(), reader.int())
        val anchor = reader.string()
        require(anchor != "") { "empty normalized anchor" }
        val attributes = attributes()
        when (kind) {
            PayloadNodeKind.DOCUMENT -> {
                val hasMetadata = reader.boolean()
                val frame = push(kind, scope, anchor, attributes)
                if (hasMetadata) {
                    field(frame, EXPECT_METADATA)
                } else {
                    phases[frame] = 1
                    counted(frame, EXPECT_CONTENT, "child")
                }
            }

            PayloadNodeKind.CALLOUT -> {
                val variant = reader.string()
                val collapsed =
                    when (reader.nullableBoolean()) {
                        null -> -1L
                        false -> 0L
                        true -> 1L
                    }
                val titleCount = reader.int()
                require(titleCount >= 0) { "invalid native callout title count" }
                val frame = push(kind, scope, anchor, attributes)
                objects[frame * OBJECT_SLOTS] = variant
                numbers[frame * NUMBER_SLOTS] = collapsed
                if (titleCount == 0) {
                    objects[frame * OBJECT_SLOTS + 1] = null
                    phases[frame] = 1
                    counted(frame, EXPECT_CONTENT, "child")
                } else {
                    open(frame, titleCount, EXPECT_CONTENT)
                }
            }

            PayloadNodeKind.PARAGRAPH,
            PayloadNodeKind.EMPHASIS,
            PayloadNodeKind.STRONG,
            PayloadNodeKind.STRIKETHROUGH,
            PayloadNodeKind.MARK,
            PayloadNodeKind.INSERTION,
            PayloadNodeKind.SPAN,
            PayloadNodeKind.SUPERSCRIPT,
            PayloadNodeKind.SUBSCRIPT,
            PayloadNodeKind.TABLE_CAPTION,
            PayloadNodeKind.DIRECTIVE_LABEL,
            -> {
                counted(push(kind, scope, anchor, attributes), EXPECT_CONTENT, "child")
            }

            PayloadNodeKind.HEADING -> {
                val level = reader.int()
                val frame = push(kind, scope, anchor, attributes)
                numbers[frame * NUMBER_SLOTS] = level.toLong()
                counted(frame, EXPECT_CONTENT, "child")
            }

            PayloadNodeKind.THEMATIC_BREAK -> {
                deliver(ThematicBreak(scope, anchor, attributes))
            }

            PayloadNodeKind.LIST -> {
                list(scope, anchor, attributes)
            }

            PayloadNodeKind.LIST_ITEM -> {
                val marker = reader.string()
                val frame = push(kind, scope, anchor, attributes)
                objects[frame * OBJECT_SLOTS] = marker
                counted(frame, EXPECT_CONTENT, "child")
            }

            PayloadNodeKind.CODE_BLOCK -> {
                deliver(
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

            PayloadNodeKind.HTML_BLOCK -> {
                deliver(HTMLBlock(reader.required(), scope, anchor, attributes))
            }

            PayloadNodeKind.FORMULA_BLOCK -> {
                deliver(FormulaBlock(reader.required(), scope, anchor, attributes))
            }

            PayloadNodeKind.TABLE -> {
                table(scope, anchor, attributes)
            }

            PayloadNodeKind.DEFINITION_LIST -> {
                val frame = push(kind, scope, anchor, attributes)
                val count = count("child")
                require(count > 0) { "empty definition list" }
                open(frame, count, EXPECT_DEFINITION)
            }

            PayloadNodeKind.DEFINITION -> {
                val compact = reader.boolean()
                val frame = push(kind, scope, anchor, attributes)
                numbers[frame * NUMBER_SLOTS] = if (compact) 1L else 0L
                counted(frame, EXPECT_CONTENT, "child")
            }

            PayloadNodeKind.DIRECTIVE_BLOCK, PayloadNodeKind.DIRECTIVE -> {
                val name = if (kind == PayloadNodeKind.DIRECTIVE) reader.required() else reader.string()
                val hasLabel = reader.boolean()
                val frame = push(kind, scope, anchor, attributes)
                objects[frame * OBJECT_SLOTS] = name
                if (hasLabel) {
                    field(frame, EXPECT_DIRECTIVE_LABEL)
                } else {
                    objects[frame * OBJECT_SLOTS + 1] = null
                    phases[frame] = 1
                    directiveContent(frame)
                }
            }

            PayloadNodeKind.TEXT -> {
                deliver(Text(reader.required(), scope, anchor, attributes))
            }

            PayloadNodeKind.SOFT_BREAK -> {
                deliver(SoftBreak(scope, anchor, attributes))
            }

            PayloadNodeKind.LINE_BREAK -> {
                deliver(LineBreak(scope, anchor, attributes))
            }

            PayloadNodeKind.CODE -> {
                deliver(Code(reader.required(), scope, anchor, attributes))
            }

            PayloadNodeKind.HTML -> {
                deliver(HTML(reader.required(), scope, anchor, attributes))
            }

            PayloadNodeKind.COMMENT -> {
                deliver(Comment(reader.required(), scope, anchor, attributes))
            }

            PayloadNodeKind.FORMULA -> {
                deliver(Formula(placement(), reader.required(), scope, anchor, attributes))
            }

            PayloadNodeKind.CROSS_LINK, PayloadNodeKind.CROSS_EMBEDDED -> {
                val dest = destination()
                require(dest is Destination.Cross) { "cross reference requires a cross destination" }
                val label = reader.string()
                if (kind == PayloadNodeKind.CROSS_EMBEDDED) {
                    deliver(CrossEmbedded(dest, label, dimensions(), scope, anchor, attributes))
                } else {
                    deliver(CrossLink(dest, label, scope, anchor, attributes))
                }
            }

            PayloadNodeKind.LINK -> {
                val resource = resource()
                val frame = push(kind, scope, anchor, attributes)
                objects[frame * OBJECT_SLOTS] = resource
                counted(frame, EXPECT_CONTENT, "child")
            }

            PayloadNodeKind.EMBEDDED -> {
                val resource = resource()
                val dimensions = dimensions()
                val frame = push(kind, scope, anchor, attributes)
                objects[frame * OBJECT_SLOTS] = resource
                objects[frame * OBJECT_SLOTS + 1] = dimensions
                counted(frame, EXPECT_CONTENT, "child")
            }

            PayloadNodeKind.CITE -> {
                val frame = push(kind, scope, anchor, attributes)
                val count = count("citation")
                require(count > 0) { "invalid native citation count" }
                open(frame, count, EXPECT_CITATION)
            }

            PayloadNodeKind.CITATION -> {
                val referent =
                    when (val branch = reader.byte().toInt()) {
                        1 -> CitationReferent.Bib(reader.required(), bibMode())
                        2 -> CitationReferent.Footnote(reader.required())
                        3 -> CitationReferent.Specimen(reader.required())
                        else -> error("invalid native citation referent $branch")
                    }
                val frame = push(kind, scope, anchor, attributes)
                objects[frame * OBJECT_SLOTS] = referent
                counted(frame, EXPECT_CONTENT, "child")
            }

            PayloadNodeKind.TABLE_ROW -> {
                counted(push(kind, scope, anchor, attributes), EXPECT_TABLE_CELL, "child")
            }

            PayloadNodeKind.TABLE_CELL -> {
                val rowspan = reader.long().toTableSpan()
                val colspan = reader.long().toTableSpan()
                val frame = push(kind, scope, anchor, attributes)
                numbers[frame * NUMBER_SLOTS] = rowspan.toLong()
                numbers[frame * NUMBER_SLOTS + 1] = colspan.toLong()
                counted(frame, EXPECT_CONTENT, "child")
            }

            PayloadNodeKind.FOOTNOTE -> {
                val id = reader.required()
                val frame = push(kind, scope, anchor, attributes)
                objects[frame * OBJECT_SLOTS] = id
                counted(frame, EXPECT_CONTENT, "child")
            }

            PayloadNodeKind.SPECIMEN -> {
                val id = reader.string()
                val startValue = reader.long()
                val start = if (reader.boolean()) startValue else null
                val frame = push(kind, scope, anchor, attributes)
                objects[frame * OBJECT_SLOTS] = id
                objects[frame * OBJECT_SLOTS + 1] = start
                counted(frame, EXPECT_CONTENT, "child")
            }

            PayloadNodeKind.METADATA -> {
                deliver(metadata(scope, anchor, attributes))
            }
        }
    }

    /** Completes every frame whose open list is full, from the top down. */
    private fun settle() {
        while (depth > 0) {
            val top = depth - 1
            val list = elements[top]!!
            if (filled[top] < list.size) return
            val node = advance(top, list) ?: continue
            if (depth == 0) {
                root = node
                return
            }
            deliver(node)
        }
    }

    /**
     * Takes the full list of the frame at [top]: the frame opens its next
     * list and answers null, or becomes its node, which is then the caller's
     * to deliver.
     */
    private fun advance(
        top: Int,
        list: Array<Any?>,
    ): Markup? {
        val objects = top * OBJECT_SLOTS
        val numbers = top * NUMBER_SLOTS
        val phase = phases[top]
        val kind = kinds[top]
        if (kind == null) {
            pop(top)
            return list[0] as Markup
        }
        val scope = scopes[top]!!
        val anchor = anchors[top]
        val attributes = this.attributes[top]!!
        when (kind) {
            PayloadNodeKind.DOCUMENT -> {
                when (phase) {
                    0 -> {
                        this.objects[objects] = list[0]
                        phases[top] = 1
                        counted(top, EXPECT_CONTENT, "child")
                    }

                    1 -> {
                        this.objects[objects + 1] = wrap<Markup>(list)
                        phases[top] = 2
                        counted(top, EXPECT_FOOTNOTE, "footnote")
                    }

                    2 -> {
                        this.objects[objects + 2] = wrap<Footnote>(list)
                        phases[top] = 3
                        counted(top, EXPECT_SPECIMEN, "specimen")
                    }

                    else -> {
                        return finish(
                            top,
                            Document(
                                slot(objects + 1),
                                this.objects[objects] as Metadata?,
                                slot(objects + 2),
                                wrap(list),
                                scope,
                                anchor,
                                attributes,
                            ),
                        )
                    }
                }
                return null
            }

            PayloadNodeKind.CALLOUT -> {
                if (phase == 0) {
                    this.objects[objects + 1] = wrap<Markup>(list)
                    phases[top] = 1
                    counted(top, EXPECT_CONTENT, "child")
                    return null
                }
                val collapsed = this.numbers[numbers]
                return finish(
                    top,
                    Callout(
                        this.objects[objects] as String?,
                        if (collapsed == -1L) null else collapsed == 1L,
                        this.objects[objects + 1]?.let { slot(objects + 1) },
                        wrap(list),
                        scope,
                        anchor,
                        attributes,
                    ),
                )
            }

            PayloadNodeKind.PARAGRAPH -> {
                return finish(top, Paragraph(wrap(list), scope, anchor, attributes))
            }

            PayloadNodeKind.EMPHASIS -> {
                return finish(top, Emphasis(wrap(list), scope, anchor, attributes))
            }

            PayloadNodeKind.STRONG -> {
                return finish(top, Strong(wrap(list), scope, anchor, attributes))
            }

            PayloadNodeKind.STRIKETHROUGH -> {
                return finish(top, Strikethrough(wrap(list), scope, anchor, attributes))
            }

            PayloadNodeKind.MARK -> {
                return finish(top, Mark(wrap(list), scope, anchor, attributes))
            }

            PayloadNodeKind.INSERTION -> {
                return finish(top, Insertion(wrap(list), scope, anchor, attributes))
            }

            PayloadNodeKind.SPAN -> {
                return finish(top, Span(wrap(list), scope, anchor, attributes))
            }

            PayloadNodeKind.SUPERSCRIPT -> {
                return finish(top, Superscript(wrap(list), scope, anchor, attributes))
            }

            PayloadNodeKind.SUBSCRIPT -> {
                return finish(top, Subscript(wrap(list), scope, anchor, attributes))
            }

            PayloadNodeKind.TABLE_CAPTION -> {
                return finish(top, TableCaption(wrap(list), scope, anchor, attributes))
            }

            PayloadNodeKind.DIRECTIVE_LABEL -> {
                return finish(top, DirectiveLabel(wrap(list), scope, anchor, attributes))
            }

            PayloadNodeKind.HEADING -> {
                val level = this.numbers[numbers].toInt()
                return finish(top, Heading(level, wrap(list), scope, anchor, attributes))
            }

            PayloadNodeKind.LIST -> {
                return finish(
                    top,
                    List(
                        this.objects[objects] as ListFlavor,
                        this.objects[objects + 1] as Long?,
                        this.objects[objects + 2] as OrderedListVariant?,
                        this.objects[objects + 3] as OrderedListDelimiter?,
                        this.numbers[numbers] != 0L,
                        wrap(list),
                        scope,
                        anchor,
                        attributes,
                    ),
                )
            }

            PayloadNodeKind.LIST_ITEM -> {
                val marker = this.objects[objects] as String?
                return finish(top, ListItem(marker, wrap(list), scope, anchor, attributes))
            }

            PayloadNodeKind.TABLE -> {
                if (phase == 0) {
                    this.objects[objects + 1] = list[0]
                    phases[top] = 1
                    rows(top)
                    return null
                }
                val head = this.numbers[numbers].toInt()
                val content = this.numbers[numbers + 1].toInt()
                return finish(
                    top,
                    Table(
                        this.objects[objects + 1] as TableCaption?,
                        slot(objects),
                        group(list, 0, head),
                        group(list, head, head + content),
                        group(list, head + content, list.size),
                        scope,
                        anchor,
                        attributes,
                    ),
                )
            }

            PayloadNodeKind.TABLE_ROW -> {
                return finish(top, TableRow(wrap(list), scope, anchor, attributes))
            }

            PayloadNodeKind.TABLE_CELL -> {
                val rowspan = this.numbers[numbers].toInt()
                val colspan = this.numbers[numbers + 1].toInt()
                return finish(
                    top,
                    TableCell(rowspan, colspan, wrap(list), scope, anchor, attributes),
                )
            }

            PayloadNodeKind.DEFINITION_LIST -> {
                return finish(top, DefinitionList(wrap(list), scope, anchor, attributes))
            }

            PayloadNodeKind.DEFINITION -> {
                if (phase == 0) {
                    this.objects[objects] = wrap<Markup>(list)
                    val bodies = count("definition body")
                    require(bodies > 0) { "definition has no bodies" }
                    this.objects[objects + 1] = arrayOfNulls<Any?>(bodies)
                    this.numbers[numbers + 1] = bodies.toLong()
                    phases[top] = 1
                    counted(top, EXPECT_CONTENT, "child")
                    return null
                }
                @Suppress("UNCHECKED_CAST")
                val bodies = this.objects[objects + 1] as Array<Any?>
                bodies[phase - 1] = wrap<Markup>(list)
                if (phase < bodies.size) {
                    phases[top] = phase + 1
                    counted(top, EXPECT_CONTENT, "child")
                    return null
                }
                return finish(
                    top,
                    Definition(
                        slot(objects),
                        ownedList(bodies),
                        this.numbers[numbers] != 0L,
                        scope,
                        anchor,
                        attributes,
                    ),
                )
            }

            PayloadNodeKind.DIRECTIVE_BLOCK, PayloadNodeKind.DIRECTIVE -> {
                if (phase == 0) {
                    this.objects[objects + 1] = list[0]
                    phases[top] = 1
                    directiveContent(top)
                    return null
                }
                val label = this.objects[objects + 1] as DirectiveLabel?
                if (kind == PayloadNodeKind.DIRECTIVE) {
                    val name = this.objects[objects] as String
                    return finish(top, Directive(name, label, scope, anchor, attributes))
                }
                val name = this.objects[objects] as String?
                return finish(
                    top,
                    DirectiveBlock(name, label, wrap(list), scope, anchor, attributes),
                )
            }

            PayloadNodeKind.LINK -> {
                val resource = this.objects[objects] as DefinitionResource
                return finish(
                    top,
                    Link(
                        resource.dest,
                        resource.title,
                        wrap(list),
                        scope,
                        anchor ?: resource.anchor,
                        attributes.inheriting(resource.attributes),
                    ),
                )
            }

            PayloadNodeKind.EMBEDDED -> {
                val resource = this.objects[objects] as DefinitionResource
                return finish(
                    top,
                    Embedded(
                        resource.dest,
                        resource.title,
                        this.objects[objects + 1] as Dimensions?,
                        wrap(list),
                        scope,
                        anchor ?: resource.anchor,
                        attributes.inheriting(resource.attributes),
                    ),
                )
            }

            PayloadNodeKind.CITE -> {
                return finish(top, Cite(wrap(list), scope, anchor, attributes))
            }

            PayloadNodeKind.CITATION -> {
                if (phase == 0) {
                    this.objects[objects + 1] = wrap<Markup>(list)
                    phases[top] = 1
                    counted(top, EXPECT_CONTENT, "child")
                    return null
                }
                return finish(
                    top,
                    Citation(
                        this.objects[objects] as CitationReferent,
                        slot(objects + 1),
                        wrap(list),
                        scope,
                        anchor,
                        attributes,
                    ),
                )
            }

            PayloadNodeKind.FOOTNOTE -> {
                val id = this.objects[objects] as String
                return finish(top, Footnote(id, wrap(list), scope, anchor, attributes))
            }

            PayloadNodeKind.SPECIMEN -> {
                val id = this.objects[objects] as String?
                val start = this.objects[objects + 1] as Long?
                return finish(top, Specimen(id, start, wrap(list), scope, anchor, attributes))
            }

            else -> {
                error("invalid payload decoder frame $kind")
            }
        }
    }

    // -- frames --------------------------------------------------------------

    private fun push(
        kind: PayloadNodeKind,
        scope: Scope,
        anchor: String?,
        attributes: Attributes,
    ): Int {
        val frame = depth
        if (frame == kinds.size) grow()
        kinds[frame] = kind
        phases[frame] = 0
        scopes[frame] = scope
        anchors[frame] = anchor
        this.attributes[frame] = attributes
        depth = frame + 1
        return frame
    }

    private fun grow() {
        val capacity = kinds.size * 2
        kinds = kinds.copyOf(capacity)
        phases = phases.copyOf(capacity)
        scopes = scopes.copyOf(capacity)
        anchors = anchors.copyOf(capacity)
        attributes = attributes.copyOf(capacity)
        objects = objects.copyOf(capacity * OBJECT_SLOTS)
        numbers = numbers.copyOf(capacity * NUMBER_SLOTS)
        elements = elements.copyOf(capacity)
        filled = filled.copyOf(capacity)
        expectations = expectations.copyOf(capacity)
    }

    /** The frame at [top] is [node]; the frame is gone. */
    private fun finish(
        top: Int,
        node: Markup,
    ): Markup {
        pop(top)
        return node
    }

    private fun pop(top: Int) {
        kinds[top] = null
        scopes[top] = null
        anchors[top] = null
        attributes[top] = null
        elements[top] = null
        objects.fill(null, top * OBJECT_SLOTS, top * OBJECT_SLOTS + OBJECT_SLOTS)
        depth = top
    }

    @Suppress("UNCHECKED_CAST")
    private fun <Element> slot(index: Int): kotlin.collections.List<Element> =
        objects[index] as kotlin.collections.List<Element>

    // -- lists ---------------------------------------------------------------

    private fun deliver(node: Markup) {
        val top = depth - 1
        elements[top]!![filled[top]++] = node
    }

    private fun open(
        frame: Int,
        count: Int,
        expectation: Int,
    ) {
        elements[frame] = if (count == 0) noElements else arrayOfNulls(count)
        filled[frame] = 0
        expectations[frame] = expectation
    }

    /** Opens a list whose count the payload states next. */
    private fun counted(
        frame: Int,
        expectation: Int,
        name: String,
    ) {
        open(frame, count(name), expectation)
    }

    /** Opens a node-valued field: a list of exactly one, with no count on the wire. */
    private fun field(
        frame: Int,
        expectation: Int,
    ) {
        open(frame, 1, expectation)
    }

    private fun directiveContent(frame: Int) {
        counted(frame, EXPECT_CONTENT, "child")
        if (kinds[frame] == PayloadNodeKind.DIRECTIVE) {
            require(elements[frame]!!.isEmpty()) { "inline directive contains block content" }
        }
    }

    private fun rows(frame: Int) {
        val count = count("child")
        val numbers = frame * NUMBER_SLOTS
        require(
            this.numbers[numbers] + this.numbers[numbers + 1] + this.numbers[numbers + 2] == count.toLong(),
        ) { "invalid table row groups" }
        open(frame, count, EXPECT_TABLE_ROW)
    }

    private fun expect(
        expectation: Int,
        kind: PayloadNodeKind,
    ) {
        when (expectation) {
            EXPECT_CONTENT -> {
                require(
                    kind != PayloadNodeKind.CITATION &&
                        kind != PayloadNodeKind.FOOTNOTE &&
                        kind != PayloadNodeKind.SPECIMEN &&
                        kind != PayloadNodeKind.METADATA,
                ) { "owned node in ordinary content" }
            }

            EXPECT_METADATA -> {
                require(kind == PayloadNodeKind.METADATA) { "invalid metadata node" }
            }

            EXPECT_FOOTNOTE -> {
                require(kind == PayloadNodeKind.FOOTNOTE) { "invalid footnote node" }
            }

            EXPECT_SPECIMEN -> {
                require(kind == PayloadNodeKind.SPECIMEN) { "invalid specimen node" }
            }

            EXPECT_CITATION -> {
                require(kind == PayloadNodeKind.CITATION) { "invalid citation node" }
            }

            EXPECT_LIST_ITEM -> {
                require(kind == PayloadNodeKind.LIST_ITEM) { "list contains a non-item node" }
            }

            EXPECT_TABLE_ROW -> {
                require(kind == PayloadNodeKind.TABLE_ROW) { "table contains a non-row node" }
            }

            EXPECT_TABLE_CELL -> {
                require(kind == PayloadNodeKind.TABLE_CELL) { "table row contains a non-cell" }
            }

            EXPECT_DEFINITION -> {
                require(kind == PayloadNodeKind.DEFINITION) { "invalid definition list child" }
            }

            EXPECT_TABLE_CAPTION -> {
                require(kind == PayloadNodeKind.TABLE_CAPTION) { "invalid table caption kind" }
            }

            EXPECT_DIRECTIVE_LABEL -> {
                require(kind == PayloadNodeKind.DIRECTIVE_LABEL) { "invalid directive label kind" }
            }

            else -> {
                // The root admits the one document, which the caller has checked.
                return
            }
        }
    }

    /** The full array as the list it is; the decoder never touches it again. */
    private fun <Element> wrap(list: Array<Any?>): kotlin.collections.List<Element> = ownedList(list)

    /** The rows in `[from, to)` as one group; a group that is the whole list is the list. */
    private fun group(
        rows: Array<Any?>,
        from: Int,
        to: Int,
    ): kotlin.collections.List<TableRow> =
        if (from == 0 && to == rows.size) ownedList(rows) else ownedList(rows.copyOfRange(from, to))

    // -- fields --------------------------------------------------------------

    private fun count(name: String): Int = reader.int().also { require(it >= 0) { "invalid native $name count" } }

    private fun attributes(): Attributes {
        val classCount = count("class")
        val classes: kotlin.collections.List<String> =
            if (classCount == 0) {
                emptyOwnedList()
            } else {
                val values = arrayOfNulls<Any?>(classCount)
                for (index in 0 until classCount) {
                    val value = reader.required()
                    require(value.isNotEmpty()) { "empty normalized class" }
                    values[index] = value
                }
                ownedList(values)
            }
        val recordCount = count("record")
        val records: kotlin.collections.List<Record> =
            if (recordCount == 0) {
                emptyOwnedList()
            } else {
                val values = arrayOfNulls<Any?>(recordCount)
                for (index in 0 until recordCount) {
                    val name = reader.required()
                    val value = reader.required()
                    require(name.isNotEmpty() && name != "id" && name != "class") { "invalid normalized record" }
                    values[index] = Record(name, value)
                }
                ownedList(values)
            }
        return Attributes.owned(classes, records)
    }

    private fun list(
        scope: Scope,
        anchor: String?,
        attributes: Attributes,
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
        val frame = push(PayloadNodeKind.LIST, scope, anchor, attributes)
        val objects = frame * OBJECT_SLOTS
        this.objects[objects] = flavor
        this.objects[objects + 1] = start
        this.objects[objects + 2] = variant
        this.objects[objects + 3] = delimiter
        numbers[frame * NUMBER_SLOTS] = if (tight) 1L else 0L
        counted(frame, EXPECT_LIST_ITEM, "child")
    }

    private fun table(
        scope: Scope,
        anchor: String?,
        attributes: Attributes,
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
        val hasCaption = reader.boolean()
        val frame = push(PayloadNodeKind.TABLE, scope, anchor, attributes)
        val numbers = frame * NUMBER_SLOTS
        objects[frame * OBJECT_SLOTS] = columns
        this.numbers[numbers] = head.toLong()
        this.numbers[numbers + 1] = content.toLong()
        this.numbers[numbers + 2] = foot.toLong()
        if (hasCaption) {
            field(frame, EXPECT_TABLE_CAPTION)
        } else {
            objects[frame * OBJECT_SLOTS + 1] = null
            phases[frame] = 1
            rows(frame)
        }
    }

    private fun Long.toTableSpan(): Int {
        require(this in 1..Int.MAX_VALUE.toLong()) { "invalid table cell span" }
        return toInt()
    }

    private fun bibMode(): BibMode =
        when (val rawValue = reader.int()) {
            1 -> BibMode.NORMAL
            2 -> BibMode.AUTHOR_IN_TEXT
            3 -> BibMode.SUPPRESS_AUTHOR
            else -> error("invalid native bib mode $rawValue")
        }

    private fun metadata(
        scope: Scope,
        anchor: String?,
        attributes: Attributes,
    ): Metadata =
        Metadata(
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
            anchor = anchor,
            attributes = attributes,
        )

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
        require(ordinal == resources.size) { "payload names an unknown resource $ordinal" }
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
