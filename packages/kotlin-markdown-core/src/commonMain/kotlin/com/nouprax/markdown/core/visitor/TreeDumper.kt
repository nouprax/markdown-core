package com.nouprax.markdown.core

/** Produces the canonical debug tree for immutable Markdown markup. */
public object TreeDumper {
    /** Returns the canonical debug dump for [root] and its owned markup. */
    public fun dump(root: Markup): String =
        DumpState().run {
            dump(root)
            result()
        }
}

private class DumpState {
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
        line(kind, node.scope, fields, children)
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
    private val state: DumpState,
) : Visitor<Unit> {
    override fun visitDocument(node: Document) {
        // The footnotes are value lines after the content, each nesting its
        // own content; `children` counts the content alone.
        state.line("Document", node, children = node.content.size)
        state.nested(node.content.size + node.footnotes.size) {
            node.content.forEach(state::dump)
            node.footnotes.forEach { footnote(it) }
        }
    }

    private fun footnote(value: Footnote) {
        state.line("Footnote", value.scope, listOf("id=${jsonString(value.id)}"), value.content.size)
        state.nested(value.content.size) { value.content.forEach(state::dump) }
    }

    override fun visitCallout(node: Callout) {
        state.line(
            "Callout",
            node,
            listOf("variant=${optionalString(node.variant)}", "collapsed=${node.collapsed ?: "null"}"),
            node.content.size,
        )
        // A non-null title is a `Title` group before the content; a
        // null one prints nothing. Neither is counted by `children`.
        state.nested(node.content.size + (if (node.title == null) 0 else 1)) {
            node.title?.let { title ->
                state.group("Title", title.size)
                state.nested(title.size) { title.forEach(state::dump) }
            }
            node.content.forEach(state::dump)
        }
    }

    override fun visitParagraph(node: Paragraph) {
        state.container("Paragraph", node, children = node.content)
    }

    override fun visitHeading(node: Heading) {
        state.container("Heading", node, listOf("level=${node.level}"), node.content)
    }

    override fun visitThematicBreak(node: ThematicBreak) {
        state.line("ThematicBreak", node)
    }

    override fun visitList(node: List) {
        state.container(
            "List",
            node,
            listOf(
                "flavor=${node.flavor.token()}",
                "start=${node.start ?: "null"}",
                "tight=${node.tight}",
            ),
            node.items,
        )
    }

    override fun visitListItem(node: ListItem) {
        state.container("ListItem", node, listOf("checked=${node.checked ?: "null"}"), node.content)
    }

    override fun visitCodeBlock(node: CodeBlock) {
        state.line(
            "CodeBlock",
            node,
            listOf(
                "info=${optionalString(node.info)}",
                "language=${optionalString(node.language)}",
                "literal=${jsonString(node.literal)}",
                "fenced=${node.fenced}",
                "closed=${node.closed}",
            ),
        )
    }

    override fun visitHTMLBlock(node: HTMLBlock) {
        state.line("HTMLBlock", node, listOf("literal=${jsonString(node.literal)}"))
    }

    override fun visitFormulaBlock(node: FormulaBlock) {
        state.line("FormulaBlock", node, listOf("literal=${jsonString(node.literal)}"))
    }

    override fun visitTable(node: Table) {
        state.container(
            "Table",
            node,
            listOf("alignments=[${node.alignments.joinToString(",") { it.token() }}]"),
            listOf(node.header) + node.rows,
        )
    }

    override fun visitTableRow(node: TableRow) {
        state.container("TableRow", node, listOf("isHeader=${node.isHeader}"), node.cells)
    }

    override fun visitTableCell(node: TableCell) {
        state.container("TableCell", node, children = node.content)
    }

    override fun visitDirectiveBlock(node: DirectiveBlock) {
        state.line(
            "DirectiveBlock",
            node,
            directiveFields(node.name, node.attributes),
            children = node.content.size,
        )
        state.nested(node.content.size + if (node.label == null) 0 else 1) {
            node.label?.let(state::dump)
            node.content.forEach(state::dump)
        }
    }

    override fun visitDirectiveLabel(node: DirectiveLabel) {
        state.container("DirectiveLabel", node, children = node.content)
    }

    override fun visitText(node: Text) {
        state.line("Text", node, listOf("literal=${jsonString(node.literal)}"))
    }

    override fun visitSoftBreak(node: SoftBreak) {
        state.line("SoftBreak", node)
    }

    override fun visitLineBreak(node: LineBreak) {
        state.line("LineBreak", node)
    }

    override fun visitCode(node: Code) {
        state.line("Code", node, listOf("literal=${jsonString(node.literal)}"))
    }

    override fun visitHTML(node: HTML) {
        state.line("HTML", node, listOf("literal=${jsonString(node.literal)}"))
    }

    override fun visitComment(node: Comment) {
        state.line("Comment", node, listOf("literal=${jsonString(node.literal)}"))
    }

    override fun visitFormula(node: Formula) {
        state.line("Formula", node, listOf("mode=${node.mode.token()}", "literal=${jsonString(node.literal)}"))
    }

    override fun visitEmphasis(node: Emphasis) {
        state.container("Emphasis", node, children = node.content)
    }

    override fun visitStrong(node: Strong) {
        state.container("Strong", node, children = node.content)
    }

    override fun visitStrikethrough(node: Strikethrough) {
        state.container("Strikethrough", node, children = node.content)
    }

    override fun visitLink(node: Link) {
        state.container(
            "Link",
            node,
            listOf(
                "dest=${destination(node.dest)}",
                "title=${optionalString(node.title)}",
            ),
            node.content,
        )
    }

    override fun visitImage(node: Image) {
        state.container(
            "Image",
            node,
            listOf(
                "dest=${destination(node.dest)}",
                "title=${optionalString(node.title)}",
            ),
            node.content,
        )
    }

    override fun visitDirective(node: Directive) {
        state.line("Directive", node, directiveFields(node.name, node.attributes))
        state.nested(if (node.label == null) 0 else 1) {
            node.label?.let(state::dump)
        }
    }

    override fun visitCite(node: Cite) {
        // The items are value lines under the cite, and `children` counts
        // them; each item's affixes are groups whose nodes nest below them.
        state.line("Cite", node, children = node.citations.size)
        state.nested(node.citations.size) { node.citations.forEach { citation(it) } }
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

    private fun directiveFields(
        name: String,
        attributes: kotlin.collections.List<DirectiveAttribute>?,
    ): kotlin.collections.List<String> =
        listOf(
            "name=${jsonString(name)}",
            "attributes=" +
                (
                    attributes?.joinToString(" ", prefix = "[", postfix = "]") {
                        "${it.name}=${jsonString(it.value)}"
                    } ?: "null"
                ),
        )
}

private fun scope(value: Scope): String =
    "scope=${value.start.line}:${value.start.column}..${value.end.line}:${value.end.column}"

private fun optionalString(value: String?): String = value?.let(::jsonString) ?: "null"

/** A tagged value prints as its branch name applied to its fields, in declaration order. */
private fun destination(value: Destination): String =
    when (value) {
        is Destination.Url -> "url(${jsonString(value.value)})"
        is Destination.Cross -> "cross(path=${jsonString(value.path)},anchor=${optionalString(value.anchor)})"
    }

private fun referent(value: CitationReferent): String =
    when (value) {
        is CitationReferent.Bib -> "bib(key=${jsonString(value.key)},mode=${value.mode.token()})"
        is CitationReferent.Footnote -> "footnote(id=${jsonString(value.id)})"
    }

private fun BibMode.token(): String =
    when (this) {
        BibMode.NORMAL -> "normal"
        BibMode.AUTHOR_IN_TEXT -> "authorInText"
        BibMode.SUPPRESS_AUTHOR -> "suppressAuthor"
    }

private fun PlacementMode.token(): String = name.lowercase()

private fun ListFlavor.token(): String = name.lowercase()

private fun TableAlignment.token(): String = name.lowercase()

private fun jsonString(value: String): String =
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
