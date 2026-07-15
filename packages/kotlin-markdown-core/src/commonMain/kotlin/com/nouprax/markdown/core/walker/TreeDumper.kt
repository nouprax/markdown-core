package com.nouprax.markdown.core

/** Produces the canonical diagnostic tree for immutable Markdown markup. */
public object TreeDumper {
    /** Returns the canonical diagnostic dump for [root] and its descendants. */
    public fun dump(root: Markup): String {
        val visitor = DumpVisitor()
        val remainingChildren = mutableListOf<Int>()
        val lines = mutableListOf<String>()

        Walker.walk(root) { event, node ->
            when (event) {
                WalkEvent.ENTERING -> {
                    val record = node.accept(visitor)
                    if (remainingChildren.isEmpty()) {
                        lines += record.line
                    } else {
                        val parent = remainingChildren.lastIndex
                        val prefix =
                            remainingChildren
                                .dropLast(1)
                                .joinToString(separator = "") { if (it > 0) "│   " else "    " }
                        val connector = if (remainingChildren[parent] == 1) "└── " else "├── "
                        lines += prefix + connector + record.line
                        remainingChildren[parent] -= 1
                    }
                    remainingChildren += record.children
                }

                WalkEvent.EXITING -> {
                    check(remainingChildren.removeAt(remainingChildren.lastIndex) == 0)
                }
            }
        }
        return lines.joinToString(separator = "\n", postfix = "\n")
    }
}

private data class DumpRecord(
    val line: String,
    val children: Int,
)

private class DumpVisitor : Visitor<DumpRecord> {
    override fun visitDocument(node: Document): DumpRecord = record("Document", node, children = node.content.size)

    override fun visitBlockQuote(node: BlockQuote): DumpRecord =
        record("BlockQuote", node, children = node.content.size)

    override fun visitParagraph(node: Paragraph): DumpRecord = record("Paragraph", node, children = node.content.size)

    override fun visitHeading(node: Heading): DumpRecord =
        record("Heading", node, fields = listOf("level=${node.level}"), children = node.content.size)

    override fun visitThematicBreak(node: ThematicBreak): DumpRecord = record("ThematicBreak", node)

    override fun visitList(node: List): DumpRecord =
        record(
            "List",
            node,
            fields =
                listOf(
                    "flavor=${node.flavor.token()}",
                    "start=${node.start ?: "null"}",
                    "tight=${node.tight}",
                ),
            children = node.items.size,
        )

    override fun visitListItem(node: ListItem): DumpRecord =
        record(
            "ListItem",
            node,
            fields = listOf("checked=${node.checked ?: "null"}"),
            children = node.content.size,
        )

    override fun visitCodeBlock(node: CodeBlock): DumpRecord =
        record(
            "CodeBlock",
            node,
            fields =
                listOf(
                    "mode=${node.mode.token()}",
                    "info=${optionalString(node.info)}",
                    "language=${optionalString(node.language)}",
                    "literal=${jsonString(node.literal)}",
                    "fenced=${node.fenced}",
                    "closed=${node.closed}",
                ),
        )

    override fun visitHTMLBlock(node: HTMLBlock): DumpRecord =
        record("HTMLBlock", node, fields = listOf("literal=${jsonString(node.literal)}"))

    override fun visitFormulaBlock(node: FormulaBlock): DumpRecord =
        record(
            "FormulaBlock",
            node,
            fields = listOf("mode=${node.mode.token()}", "literal=${jsonString(node.literal)}"),
        )

    override fun visitTable(node: Table): DumpRecord =
        record(
            "Table",
            node,
            fields = listOf("alignments=[${node.alignments.joinToString(",") { it.token() }}]"),
            children = 1 + node.rows.size,
        )

    override fun visitTableRow(node: TableRow): DumpRecord =
        record(
            "TableRow",
            node,
            fields = listOf("isHeader=${node.isHeader}"),
            children = node.cells.size,
        )

    override fun visitTableCell(node: TableCell): DumpRecord = record("TableCell", node, children = node.content.size)

    override fun visitDirectiveBlock(node: DirectiveBlock): DumpRecord =
        record(
            "DirectiveBlock",
            node,
            fields = directiveFields(node.mode, node.name, node.attributes, node.label?.size),
            children = node.label.orEmpty().size + node.content.size,
        )

    override fun visitFootnoteDefinition(node: FootnoteDefinition): DumpRecord =
        record(
            "FootnoteDefinition",
            node,
            fields = listOf("id=${jsonString(node.id)}"),
            children = node.content.size,
        )

    override fun visitText(node: Text): DumpRecord =
        record("Text", node, fields = listOf("literal=${jsonString(node.literal)}"))

    override fun visitSoftBreak(node: SoftBreak): DumpRecord = record("SoftBreak", node)

    override fun visitLineBreak(node: LineBreak): DumpRecord = record("LineBreak", node)

    override fun visitCode(node: Code): DumpRecord =
        record(
            "Code",
            node,
            fields = listOf("mode=${node.mode.token()}", "literal=${jsonString(node.literal)}"),
        )

    override fun visitHTML(node: HTML): DumpRecord =
        record("HTML", node, fields = listOf("literal=${jsonString(node.literal)}"))

    override fun visitFormula(node: Formula): DumpRecord =
        record(
            "Formula",
            node,
            fields = listOf("mode=${node.mode.token()}", "literal=${jsonString(node.literal)}"),
        )

    override fun visitEmphasis(node: Emphasis): DumpRecord = record("Emphasis", node, children = node.content.size)

    override fun visitStrong(node: Strong): DumpRecord = record("Strong", node, children = node.content.size)

    override fun visitStrikethrough(node: Strikethrough): DumpRecord =
        record("Strikethrough", node, children = node.content.size)

    override fun visitLink(node: Link): DumpRecord =
        record(
            "Link",
            node,
            fields =
                listOf(
                    "destination=${optionalString(node.destination)}",
                    "title=${optionalString(node.title)}",
                ),
            children = node.content.size,
        )

    override fun visitImage(node: Image): DumpRecord =
        record(
            "Image",
            node,
            fields =
                listOf(
                    "source=${optionalString(node.source)}",
                    "title=${optionalString(node.title)}",
                ),
            children = node.content.size,
        )

    override fun visitDirective(node: Directive): DumpRecord =
        record(
            "Directive",
            node,
            fields = directiveFields(node.mode, node.name, node.attributes, node.label?.size),
            children = node.label.orEmpty().size,
        )

    override fun visitFootnoteReference(node: FootnoteReference): DumpRecord =
        record("FootnoteReference", node, fields = listOf("id=${jsonString(node.id)}"))
}

private fun record(
    kind: String,
    node: Markup,
    fields: kotlin.collections.List<String> = emptyList(),
    children: Int = 0,
): DumpRecord {
    val fieldText = if (fields.isEmpty()) "" else " ${fields.joinToString(" ")}"
    return DumpRecord(
        line = "$kind ${scope(node.scope)}$fieldText children=$children",
        children = children,
    )
}

private fun directiveFields(
    mode: PlacementMode,
    name: String,
    attributes: String?,
    labelCount: Int?,
): kotlin.collections.List<String> =
    listOf(
        "mode=${mode.token()}",
        "name=${jsonString(name)}",
        "attributes=${optionalString(attributes)}",
        "label=${labelCount ?: "null"}",
    )

private fun scope(value: Scope): String =
    "scope=${value.start.line}:${value.start.column}..${value.end.line}:${value.end.column}"

private fun optionalString(value: String?): String = value?.let(::jsonString) ?: "null"

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
