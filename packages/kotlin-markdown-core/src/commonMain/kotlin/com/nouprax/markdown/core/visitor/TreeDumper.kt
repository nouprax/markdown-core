package com.nouprax.markdown.core

/** Produces the canonical debug tree for immutable Markdown markup. */
public object TreeDumper {
    /** Returns the canonical debug dump for [root] and its owned markup. */
    public fun dump(root: Markup): String =
        State().run {
            dump(root)
            result()
        }
}

private class State {
    private val remainingNodes = mutableListOf<Int>()
    private val lines = mutableListOf<String>()
    private val visitor = DumpVisitor(this)

    fun dump(node: Markup) {
        node.accept(visitor)
    }

    fun result(): String = lines.joinToString(separator = "\n", postfix = "\n")

    fun container(
        kind: String,
        node: Markup,
        fields: kotlin.collections.List<String> = emptyList(),
        children: kotlin.collections.List<Markup>,
    ) {
        line(kind, node, fields, children.size)
        nested(children.size) { children.forEach(::dump) }
    }

    fun line(
        kind: String,
        node: Markup,
        fields: kotlin.collections.List<String> = emptyList(),
        children: Int = 0,
    ) {
        line(
            kind,
            node.scope,
            listOf("anchor=${optional(node.anchor)}", "attributes=${attributes(node.attributes)}") + fields,
            children,
        )
    }

    /** A value line has the node line's shape: a scoped value prints like a node. */
    fun line(
        kind: String,
        scope: Scope,
        fields: kotlin.collections.List<String>,
        children: Int,
    ) {
        val fieldText = if (fields.isEmpty()) "" else " ${fields.joinToString(" ")}"
        emit("$kind ${scope(scope)}$fieldText children=$children")
    }

    /**
     * A group line nests a node-valued list under its owner: `Kind children=N`
     * with no scope and no fields. The caller opens the list's own nesting.
     */
    fun group(
        kind: String,
        children: Int,
    ) {
        emit("$kind children=$children")
    }

    private fun emit(text: String) {
        if (remainingNodes.isEmpty()) {
            lines += text
            return
        }

        val parent = remainingNodes.lastIndex
        val prefix = remainingNodes.dropLast(1).joinToString("") { if (it > 0) "│   " else "    " }
        val connector = if (remainingNodes[parent] == 1) "└── " else "├── "
        lines += prefix + connector + text
        remainingNodes[parent] -= 1
    }

    fun nested(
        count: Int,
        body: () -> Unit,
    ) {
        remainingNodes += count
        body()
        check(remainingNodes.removeAt(remainingNodes.lastIndex) == 0) {
            "node dumper did not emit every owned node"
        }
    }
}

/** Each visit emits exactly that node and chooses its children and fields. */
private class DumpVisitor(
    private val state: State,
) : Visitor<Unit> {
    override fun visit(document: Document) {
        // The footnotes are value lines after the content, each nesting its
        // own content; `children` counts the content alone.
        state.line("Document", document, children = document.content.size)
        state.nested(
            document.content.size + document.footnotes.size + document.specimens.size +
                (if (document.metadata == null) 0 else 1),
        ) {
            document.metadata?.let { metadata(it) }
            document.content.forEach(state::dump)
            document.footnotes.forEach { footnote(it) }
            document.specimens.forEach { specimen(it) }
        }
    }

    private fun metadata(value: Metadata) {
        state.line(
            "Metadata",
            value.scope,
            listOf(
                "name=${value.name?.let(::metadataValue) ?: "null"}",
                "title=${value.title?.let(::metadataValue) ?: "null"}",
                "subtitle=${value.subtitle?.let(::metadataValue) ?: "null"}",
                "time=${value.time?.let(::metadataValue) ?: "null"}",
                "date=${value.date?.let(::metadataValue) ?: "null"}",
                "authors=${value.authors?.let(::metadataValue) ?: "null"}",
                "keywords=${value.keywords?.let(::metadataValue) ?: "null"}",
                "abstract=${value.`abstract`?.let(::metadataValue) ?: "null"}",
                "state=${value.state?.let(::metadataValue) ?: "null"}",
                "comment=${value.comment?.let(::metadataValue) ?: "null"}",
            ),
            0,
        )
    }

    private fun footnote(value: Footnote) {
        state.line("Footnote", value.scope, listOf("id=${escaped(value.id)}"), value.content.size)
        state.nested(value.content.size) { value.content.forEach(state::dump) }
    }

    private fun specimen(value: Specimen) {
        state.line(
            "Specimen",
            value.scope,
            listOf("id=${optional(value.id)}", "start=${value.start ?: "null"}"),
            value.content.size,
        )
        state.nested(value.content.size) { value.content.forEach(state::dump) }
    }

    override fun visit(callout: Callout) {
        state.line(
            "Callout",
            callout,
            listOf("variant=${optional(callout.variant)}", "collapsed=${callout.collapsed ?: "null"}"),
            callout.content.size,
        )
        // A non-null title is a `Title` group before the content; a
        // null one prints nothing. Neither is counted by `children`.
        state.nested(callout.content.size + (if (callout.title == null) 0 else 1)) {
            callout.title?.let { title ->
                state.group("Title", title.size)
                state.nested(title.size) { title.forEach(state::dump) }
            }
            callout.content.forEach(state::dump)
        }
    }

    override fun visit(paragraph: Paragraph) {
        state.container("Paragraph", paragraph, children = paragraph.content)
    }

    override fun visit(heading: Heading) {
        state.container("Heading", heading, listOf("level=${heading.level}"), heading.content)
    }

    override fun visit(thematicBreak: ThematicBreak) {
        state.line("ThematicBreak", thematicBreak)
    }

    override fun visit(list: List) {
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

    override fun visit(listItem: ListItem) {
        state.container(
            "ListItem",
            listItem,
            listOf("marker=${optional(listItem.marker)}"),
            listItem.content,
        )
    }

    override fun visit(codeBlock: CodeBlock) {
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

    override fun visit(htmlBlock: HTMLBlock) {
        state.line("HTMLBlock", htmlBlock, listOf("literal=${escaped(htmlBlock.literal)}"))
    }

    override fun visit(formulaBlock: FormulaBlock) {
        state.line("FormulaBlock", formulaBlock, listOf("literal=${escaped(formulaBlock.literal)}"))
    }

    override fun visit(table: Table) {
        val columns =
            table.columns.joinToString(
                ",",
            ) { "${it.flow.token()}:${it.relative?.let(::decimal) ?: "null"}" }
        state.line("Table", table, listOf("columns=[$columns]"), table.head.size + table.content.size + table.foot.size)
        state.nested(3 + if (table.caption == null) 0 else 1) {
            table.caption?.let(state::dump)
            for ((name, rows) in listOf(
                "TableHead" to table.head,
                "TableBody" to table.content,
                "TableFoot" to table.foot,
            )) {
                state.group(name, rows.size)
                state.nested(rows.size) { rows.forEach(state::dump) }
            }
        }
    }

    override fun visit(tableCaption: TableCaption): Unit =
        state.container("TableCaption", tableCaption, emptyList(), tableCaption.content)

    override fun visit(tableRow: TableRow): Unit = state.container("TableRow", tableRow, emptyList(), tableRow.cells)

    override fun visit(tableCell: TableCell): Unit =
        state.container(
            "TableCell",
            tableCell,
            listOf("rowspan=${tableCell.rowspan}", "colspan=${tableCell.colspan}"),
            tableCell.content,
        )

    override fun visit(directiveBlock: DirectiveBlock) {
        state.line(
            "DirectiveBlock",
            directiveBlock,
            listOf("name=${optional(directiveBlock.name)}"),
            children = directiveBlock.content.size,
        )
        state.nested(directiveBlock.content.size + if (directiveBlock.label == null) 0 else 1) {
            directiveBlock.label?.let(state::dump)
            directiveBlock.content.forEach(state::dump)
        }
    }

    override fun visit(directiveLabel: DirectiveLabel) {
        state.container("DirectiveLabel", directiveLabel, children = directiveLabel.content)
    }

    override fun visit(text: Text) {
        state.line("Text", text, listOf("literal=${escaped(text.literal)}"))
    }

    override fun visit(softBreak: SoftBreak) {
        state.line("SoftBreak", softBreak)
    }

    override fun visit(lineBreak: LineBreak) {
        state.line("LineBreak", lineBreak)
    }

    override fun visit(code: Code) {
        state.line("Code", code, listOf("literal=${escaped(code.literal)}"))
    }

    override fun visit(html: HTML) {
        state.line("HTML", html, listOf("literal=${escaped(html.literal)}"))
    }

    override fun visit(crossLink: CrossLink) {
        state.line(
            "CrossLink",
            crossLink,
            listOf("dest=${destination(crossLink.dest)}", "label=${optional(crossLink.label)}"),
        )
    }

    override fun visit(crossEmbedded: CrossEmbedded) {
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

    override fun visit(comment: Comment) {
        state.line("Comment", comment, listOf("literal=${escaped(comment.literal)}"))
    }

    override fun visit(formula: Formula) {
        state.line("Formula", formula, listOf("mode=${formula.mode.token()}", "literal=${escaped(formula.literal)}"))
    }

    override fun visit(emphasis: Emphasis) {
        state.container("Emphasis", emphasis, children = emphasis.content)
    }

    override fun visit(strong: Strong) {
        state.container("Strong", strong, children = strong.content)
    }

    override fun visit(strikethrough: Strikethrough) {
        state.container("Strikethrough", strikethrough, children = strikethrough.content)
    }

    override fun visit(mark: Mark) {
        state.container("Mark", mark, children = mark.content)
    }

    override fun visit(insertion: Insertion) {
        state.container("Insertion", insertion, children = insertion.content)
    }

    override fun visit(span: Span) {
        state.container("Span", span, children = span.content)
    }

    override fun visit(superscript: Superscript) {
        state.container("Superscript", superscript, children = superscript.content)
    }

    override fun visit(definitionList: DefinitionList) {
        state.container("DefinitionList", definitionList, children = definitionList.definitions)
    }

    override fun visit(definition: Definition) {
        state.line("Definition", definition, listOf("compact=${definition.compact}"), definition.content.size)
        state.nested(definition.content.size + 1) {
            state.group("DefinitionTerm", definition.term.size)
            state.nested(definition.term.size) { definition.term.forEach(state::dump) }
            for (body in definition.content) {
                state.group("DefinitionBody", body.size)
                state.nested(body.size) { body.forEach(state::dump) }
            }
        }
    }

    override fun visit(subscript: Subscript) {
        state.container("Subscript", subscript, children = subscript.content)
    }

    override fun visit(link: Link) {
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

    override fun visit(embedded: Embedded) {
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

    override fun visit(directive: Directive) {
        state.line("Directive", directive, listOf("name=${escaped(directive.name)}"))
        state.nested(if (directive.label == null) 0 else 1) {
            directive.label?.let(state::dump)
        }
    }

    override fun visit(cite: Cite) {
        // The items are value lines under the cite, and `children` counts
        // them; each item's affixes are groups whose nodes nest below them.
        state.line("Cite", cite, children = cite.citations.size)
        state.nested(cite.citations.size) { cite.citations.forEach { citation(it) } }
    }

    private fun citation(value: Citation) {
        state.line("Citation", value.scope, listOf("referent=${referent(value.referent)}"), 0)
        state.nested(2) {
            state.group("CitationPrefix", value.prefix.size)
            state.nested(value.prefix.size) { value.prefix.forEach(state::dump) }
            state.group("CitationSuffix", value.suffix.size)
            state.nested(value.suffix.size) { value.suffix.forEach(state::dump) }
        }
    }
}

private fun scope(value: Scope): String =
    "scope=${value.start.line}:${value.start.column}..${value.end.line}:${value.end.column}"

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
            value.all { it.code in 33..126 && it.code !in listOf(34, 92, 123, 125, 91, 93, 40, 41, 61) }
    return if (plain) value else escaped(value)
}

private fun dimensions(value: Dimensions?): String =
    value?.let { "(width=${it.width},height=${it.height ?: "null"})" } ?: "null"
