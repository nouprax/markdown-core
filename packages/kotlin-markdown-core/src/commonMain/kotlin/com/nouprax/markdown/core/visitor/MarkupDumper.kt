package com.nouprax.markdown.core

/** Produces the canonical debug tree for immutable Markdown markup. */
public object MarkupDumper {
    /** Returns the canonical debug dump for [root] and its owned markup. */
    public fun dump(root: Markup): String =
        State().run {
            dump(root)
            result()
        }
}

private class State {
    // Output groups contain only names and counts, never markup nodes.
    private data class Frame(
        val groups: kotlin.collections.List<Pair<String?, Int>>,
        var index: Int = -1,
        var remaining: Int = 0,
    )

    private val frames = mutableListOf<Frame>()

    // The nodes still to draw at every open nesting level, as a stack of ints.
    private var remainingNodes = IntArray(16)
    private var depth = 0

    // The connector segments of every open nesting level, and where the
    // segments above each depth end: a line copies its lead-in once and
    // extends the segments by the one its own connector decides, instead of
    // deriving every level again per line.
    private val prefix = StringBuilder()
    private var prefixEnds = IntArray(17)
    private val output = StringBuilder()
    private val visitor = DumpVisitor(this)

    fun dump(node: Markup) {
        node.walk(visitor)
    }

    fun result(): String = output.toString()

    fun container(
        kind: String,
        node: Markup,
        fields: kotlin.collections.List<String> = emptyList(),
        children: kotlin.collections.List<Markup>,
    ) {
        line(kind, node, fields, children.size)
    }

    fun line(
        kind: String,
        node: Markup,
        fields: kotlin.collections.List<String> = emptyList(),
        children: Int = 0,
        groups: kotlin.collections.List<Pair<String?, Int>> = listOf(null to children),
    ) {
        line(
            kind,
            node.scope,
            listOf("anchor=${optional(node.anchor)}", "attributes=${attributes(node.attributes)}") + fields,
            children,
        )
        frames += Frame(groups)
        push(groups.sumOf { (name, count) -> if (name == null) count else 1 })
    }

    /** Formats a node line from its common and kind-specific fields. */
    fun line(
        kind: String,
        scope: Scope,
        fields: kotlin.collections.List<String>,
        children: Int,
    ) {
        val fieldText = if (fields.isEmpty()) "" else " ${fields.joinToString(" ")}"
        emit("$kind ${scope(scope)}$fieldText children=$children")
    }

    private fun emit(text: String) {
        if (depth == 0) {
            output.append(text).append('\n')
            return
        }

        val parent = depth - 1
        val remaining = remainingNodes[parent] - 1
        remainingNodes[parent] = remaining
        val above = prefixEnds[parent]
        output
            .append(prefix, 0, above)
            .append(if (remaining == 0) "└── " else "├── ")
            .append(text)
            .append('\n')
        prefix.setLength(above)
        prefix.append(if (remaining > 0) "│   " else "    ")
        prefixEnds[depth] = prefix.length
    }

    private fun push(count: Int) {
        if (depth == remainingNodes.size) {
            remainingNodes = remainingNodes.copyOf(depth * 2)
            prefixEnds = prefixEnds.copyOf(depth * 2 + 1)
        }
        remainingNodes[depth++] = count
    }

    private fun pop(): Int = remainingNodes[--depth]

    fun start() {
        if (frames.isEmpty()) return
        advance()
        check(frames.last().remaining > 0)
        frames.last().remaining -= 1
    }

    fun end() {
        advance()
        check(frames.removeAt(frames.lastIndex).remaining == 0)
        check(pop() == 0)
    }

    private fun advance() {
        val frame = frames.last()
        while (frame.remaining == 0 && frame.index < frame.groups.size) {
            if (frame.index >= 0 && frame.groups[frame.index].first != null) {
                check(pop() == 0)
            }
            frame.index += 1
            if (frame.index == frame.groups.size) return
            val (name, count) = frame.groups[frame.index]
            if (name != null) {
                emit("$name children=$count")
                push(count)
            }
            frame.remaining = count
        }
    }
}

/** Formats walker callbacks without choosing or visiting descendants. */
private class DumpVisitor(
    private val state: State,
) : MarkupVisitor {
    override fun visit(
        document: Document,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.line(
            "Document",
            document,
            children = document.content.size,
            groups =
                listOf(
                    null to
                        (
                            document.content.size + document.footnotes.size + document.specimens.size +
                                if (document.metadata == null) 0 else 1
                        ),
                ),
        )
    }

    override fun visit(
        metadata: Metadata,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.line(
            "Metadata",
            metadata,
            listOf(
                "name=${metadata.name?.let(::metadataValue) ?: "null"}",
                "title=${metadata.title?.let(::metadataValue) ?: "null"}",
                "subtitle=${metadata.subtitle?.let(::metadataValue) ?: "null"}",
                "time=${metadata.time?.let(::metadataValue) ?: "null"}",
                "date=${metadata.date?.let(::metadataValue) ?: "null"}",
                "authors=${metadata.authors?.let(::metadataValue) ?: "null"}",
                "keywords=${metadata.keywords?.let(::metadataValue) ?: "null"}",
                "abstract=${metadata.`abstract`?.let(::metadataValue) ?: "null"}",
                "state=${metadata.state?.let(::metadataValue) ?: "null"}",
                "comment=${metadata.comment?.let(::metadataValue) ?: "null"}",
            ),
            0,
        )
    }

    override fun visit(
        footnote: Footnote,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.line("Footnote", footnote, listOf("id=${escaped(footnote.id)}"), footnote.content.size)
    }

    override fun visit(
        specimen: Specimen,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.line(
            "Specimen",
            specimen,
            listOf("id=${optional(specimen.id)}", "start=${specimen.start ?: "null"}"),
            specimen.content.size,
        )
    }

    override fun visit(
        callout: Callout,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.line(
            "Callout",
            callout,
            listOf("variant=${optional(callout.variant)}", "collapsed=${callout.collapsed ?: "null"}"),
            callout.content.size,
            groups =
                (callout.title?.let { listOf("Title" to it.size) } ?: emptyList()) +
                    listOf(null to callout.content.size),
        )
    }

    override fun visit(
        paragraph: Paragraph,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.container("Paragraph", paragraph, children = paragraph.content)
    }

    override fun visit(
        heading: Heading,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.container("Heading", heading, listOf("level=${heading.level}"), heading.content)
    }

    override fun visit(
        thematicBreak: ThematicBreak,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.line("ThematicBreak", thematicBreak)
    }

    override fun visit(
        list: List,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.container(
            "List",
            list,
            listOf(
                "flavor=${list.flavor.token()}",
                "start=${list.start ?: "null"}",
                "variant=${list.variant?.token() ?: "null"}",
                "delimiter=${list.delimiter?.token() ?: "null"}",
                "tight=${list.tight}",
            ),
            list.items,
        )
    }

    override fun visit(
        listItem: ListItem,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.container(
            "ListItem",
            listItem,
            listOf("marker=${optional(listItem.marker)}"),
            listItem.content,
        )
    }

    override fun visit(
        codeBlock: CodeBlock,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.line(
            "CodeBlock",
            codeBlock,
            listOf(
                "info=${optional(codeBlock.info)}",
                "language=${optional(codeBlock.language)}",
                "literal=${escaped(codeBlock.literal)}",
                "fenced=${codeBlock.fenced}",
                "closed=${codeBlock.closed}",
            ),
        )
    }

    override fun visit(
        htmlBlock: HTMLBlock,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.line("HTMLBlock", htmlBlock, listOf("literal=${escaped(htmlBlock.literal)}"))
    }

    override fun visit(
        formulaBlock: FormulaBlock,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.line("FormulaBlock", formulaBlock, listOf("literal=${escaped(formulaBlock.literal)}"))
    }

    override fun visit(
        table: Table,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        val columns =
            table.columns.joinToString(
                ",",
            ) { "${it.flow.token()}:${it.relative?.let(::decimal) ?: "null"}" }
        state.line(
            "Table",
            table,
            listOf("columns=[$columns]"),
            table.head.size + table.content.size + table.foot.size,
            groups =
                (if (table.caption == null) emptyList() else listOf(null to 1)) +
                    listOf(
                        "TableHead" to table.head.size,
                        "TableBody" to table.content.size,
                        "TableFoot" to table.foot.size,
                    ),
        )
    }

    override fun visit(
        tableCaption: TableCaption,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.container("TableCaption", tableCaption, emptyList(), tableCaption.content)
    }

    override fun visit(
        tableRow: TableRow,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.container("TableRow", tableRow, emptyList(), tableRow.cells)
    }

    override fun visit(
        tableCell: TableCell,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.container(
            "TableCell",
            tableCell,
            listOf("rowspan=${tableCell.rowspan}", "colspan=${tableCell.colspan}"),
            tableCell.content,
        )
    }

    override fun visit(
        directiveBlock: DirectiveBlock,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.line(
            "DirectiveBlock",
            directiveBlock,
            listOf("name=${optional(directiveBlock.name)}"),
            children = directiveBlock.content.size,
            groups = listOf(null to (directiveBlock.content.size + if (directiveBlock.label == null) 0 else 1)),
        )
    }

    override fun visit(
        directiveLabel: DirectiveLabel,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.container("DirectiveLabel", directiveLabel, children = directiveLabel.content)
    }

    override fun visit(
        text: Text,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.line("Text", text, listOf("literal=${escaped(text.literal)}"))
    }

    override fun visit(
        softBreak: SoftBreak,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.line("SoftBreak", softBreak)
    }

    override fun visit(
        lineBreak: LineBreak,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.line("LineBreak", lineBreak)
    }

    override fun visit(
        code: Code,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.line("Code", code, listOf("literal=${escaped(code.literal)}"))
    }

    override fun visit(
        html: HTML,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.line("HTML", html, listOf("literal=${escaped(html.literal)}"))
    }

    override fun visit(
        crossLink: CrossLink,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.line(
            "CrossLink",
            crossLink,
            listOf("dest=${destination(crossLink.dest)}", "label=${optional(crossLink.label)}"),
        )
    }

    override fun visit(
        crossEmbedded: CrossEmbedded,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.line(
            "CrossEmbedded",
            crossEmbedded,
            listOf(
                "dest=${destination(crossEmbedded.dest)}",
                "label=${optional(crossEmbedded.label)}",
                "dimensions=${dimensions(crossEmbedded.dimensions)}",
            ),
        )
    }

    override fun visit(
        comment: Comment,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.line("Comment", comment, listOf("literal=${escaped(comment.literal)}"))
    }

    override fun visit(
        formula: Formula,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.line("Formula", formula, listOf("mode=${formula.mode.token()}", "literal=${escaped(formula.literal)}"))
    }

    override fun visit(
        emphasis: Emphasis,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.container("Emphasis", emphasis, children = emphasis.content)
    }

    override fun visit(
        strong: Strong,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.container("Strong", strong, children = strong.content)
    }

    override fun visit(
        strikethrough: Strikethrough,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.container("Strikethrough", strikethrough, children = strikethrough.content)
    }

    override fun visit(
        mark: Mark,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.container("Mark", mark, children = mark.content)
    }

    override fun visit(
        insertion: Insertion,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.container("Insertion", insertion, children = insertion.content)
    }

    override fun visit(
        span: Span,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.container("Span", span, children = span.content)
    }

    override fun visit(
        superscript: Superscript,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.container("Superscript", superscript, children = superscript.content)
    }

    override fun visit(
        definitionList: DefinitionList,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.container("DefinitionList", definitionList, children = definitionList.definitions)
    }

    override fun visit(
        definition: Definition,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.line(
            "Definition",
            definition,
            listOf("compact=${definition.compact}"),
            definition.content.size,
            groups =
                listOf("DefinitionTerm" to definition.term.size) +
                    definition.content.map { "DefinitionBody" to it.size },
        )
    }

    override fun visit(
        subscript: Subscript,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.container("Subscript", subscript, children = subscript.content)
    }

    override fun visit(
        link: Link,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.container(
            "Link",
            link,
            listOf(
                "dest=${destination(link.dest)}",
                "title=${optional(link.title)}",
            ),
            link.content,
        )
    }

    override fun visit(
        embedded: Embedded,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.container(
            "Embedded",
            embedded,
            listOf(
                "dest=${destination(embedded.dest)}",
                "title=${optional(embedded.title)}",
                "dimensions=${dimensions(embedded.dimensions)}",
            ),
            embedded.content,
        )
    }

    override fun visit(
        directive: Directive,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.line(
            "Directive",
            directive,
            listOf("name=${escaped(directive.name)}"),
            groups = listOf(null to (if (directive.label == null) 0 else 1)),
        )
    }

    override fun visit(
        cite: Cite,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.line("Cite", cite, children = cite.citations.size)
    }

    override fun visit(
        citation: Citation,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.EXIT) {
            state.end()
            return
        }
        state.start()
        state.line(
            "Citation",
            citation,
            listOf("referent=${referent(citation.referent)}"),
            0,
            groups = listOf("CitationPrefix" to citation.prefix.size, "CitationSuffix" to citation.suffix.size),
        )
    }
}

private fun scope(value: Scope): String =
    "scope=${value.startLine}:${value.startColumn}..${value.endLine}:${value.endColumn}"

private fun optional(value: String?): String = value?.let(::escaped) ?: "null"

/** A tagged value prints as its branch name applied to its fields, in declaration order. */
private fun destination(value: Destination): String =
    when (value) {
        is Destination.Url -> "url(${escaped(value.value)})"
        is Destination.Cross -> "cross(path=${escaped(value.path)},anchor=${optional(value.anchor)})"
    }

private fun referent(value: CitationReferent): String =
    when (value) {
        is CitationReferent.Bib -> "bib(key=${escaped(value.key)},mode=${value.mode.token()})"
        is CitationReferent.Footnote -> "footnote(id=${escaped(value.id)})"
        is CitationReferent.Specimen -> "specimen(id=${escaped(value.id)})"
    }

private fun BibMode.token(): String =
    when (this) {
        BibMode.NORMAL -> "normal"
        BibMode.AUTHOR_IN_TEXT -> "authorInText"
        BibMode.SUPPRESS_AUTHOR -> "suppressAuthor"
    }

private fun Placement.token(): String = name.lowercase()

private fun ListFlavor.token(): String = name.lowercase()

private fun OrderedListVariant.token(): String =
    when (this) {
        OrderedListVariant.Decimal -> "decimal"
        is OrderedListVariant.Alpha -> "alpha(lowercased=$lowercased)"
        is OrderedListVariant.Roman -> "roman(lowercased=$lowercased)"
        OrderedListVariant.Default -> "default"
    }

private fun OrderedListDelimiter.token(): String =
    when (this) {
        OrderedListDelimiter.Period -> "period"
        is OrderedListDelimiter.Parenthesis -> "parenthesis(closed=$closed)"
        OrderedListDelimiter.Default -> "default"
    }

private fun Flow.token(): String = name.lowercase()

private fun escaped(value: String): String =
    buildString {
        append('"')
        value.forEach { character ->
            when (character) {
                '"' -> {
                    append("\\\"")
                }

                '\\' -> {
                    append("\\\\")
                }

                '\b' -> {
                    append("\\b")
                }

                '\u000c' -> {
                    append("\\f")
                }

                '\n' -> {
                    append("\\n")
                }

                '\r' -> {
                    append("\\r")
                }

                '\t' -> {
                    append("\\t")
                }

                else -> {
                    if (character.code < 0x20) {
                        append("\\u")
                        append(character.code.toString(16).padStart(4, '0'))
                    } else {
                        append(character)
                    }
                }
            }
        }
        append('"')
    }

/** Emit shortest round-trip digits with the same notation thresholds on every runtime. */
private fun decimal(value: Double): String {
    val parts = value.toString().lowercase().split('e')
    val mantissa = parts[0].split('.')
    var digits = mantissa.joinToString("")
    var point = mantissa[0].length + if (parts.size == 2) parts[1].toInt() else 0
    while (digits.startsWith('0')) {
        digits = digits.drop(1)
        point--
    }
    digits = digits.trimEnd('0')
    // Some runtimes print two significant digits for subnormal values where
    // one already round-trips. Test rounded prefixes, preserving the value.
    for (length in 1 until digits.length) {
        val rounded = digits.take(length).toLong() + if (digits[length] >= '5') 1 else 0
        val candidate = rounded.toString()
        if ((candidate + "e" + (point - length)).toDouble() == value) {
            point += candidate.length - length
            digits = candidate.trimEnd('0')
            break
        }
    }
    if (point <= -6 || point > 21) {
        return digits.take(1) + (if (digits.length > 1) "." + digits.drop(1) else "") +
            "e" + (if (point > 0) "+" else "") + (point - 1)
    }
    if (point <= 0) return "0." + "0".repeat(-point) + digits
    if (point >= digits.length) return digits + "0".repeat(point - digits.length)
    return digits.take(point) + "." + digits.drop(point)
}

private fun attributes(value: Attributes): String =
    (
        value.classes.map {
            "." + identifier(it)
        } + value.records.map { "${it.name}=${escaped(it.value)}" }
    ).joinToString(" ", "{", "}")

private fun metadataValue(value: MetadataValue): String =
    when (value) {
        is MetadataValue.Scalar -> {
            "scalar(" +
                when (val scalar = value.value) {
                    MetadataScalar.Null -> "null"
                    is MetadataScalar.Bool -> "bool(${scalar.value})"
                    is MetadataScalar.Number -> "number(${escaped(scalar.value)})"
                    is MetadataScalar.Text -> "text(${escaped(scalar.value)})"
                } + ")"
        }

        is MetadataValue.List -> {
            "list([" +
                value.items.joinToString(",") { item ->
                    when (item) {
                        is MetadataListItem.Number -> "number(${escaped(item.value)})"
                        is MetadataListItem.Text -> "text(${escaped(item.value)})"
                    }
                } + "])"
        }
    }

private fun identifier(value: String): String {
    val plain =
        value.isNotEmpty() &&
            value.all { it.code in 33..126 && it !in "\"\\{}[]()=" }
    return if (plain) value else escaped(value)
}

private fun dimensions(value: Dimensions?): String =
    value?.let { "(width=${it.width},height=${it.height ?: "null"})" } ?: "null"
