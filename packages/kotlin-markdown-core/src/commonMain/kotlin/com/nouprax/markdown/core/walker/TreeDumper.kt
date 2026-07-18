package com.nouprax.markdown.core

/** Produces the canonical diagnostic tree for immutable Markdown markup. */
public object TreeDumper {
    /** Returns the canonical diagnostic dump for [document] and its
     * descendants, resolving absolute scopes through the snapshot. */
    public fun dump(document: Document): String {
        val visitor = DumpVisitor()
        val remainingChildren = mutableListOf<Int>()
        val lines = mutableListOf<String>()

        Walker.walk(document) { event, node, scope ->
            when (event) {
                WalkEvent.ENTERING -> {
                    val record = node.accept(visitor)
                    val line = record.line(scope)
                    if (remainingChildren.isEmpty()) {
                        lines += line
                    } else {
                        val parent = remainingChildren.lastIndex
                        val prefix =
                            remainingChildren
                                .dropLast(1)
                                .joinToString(separator = "") { if (it > 0) "│   " else "    " }
                        val connector = if (remainingChildren[parent] == 1) "└── " else "├── "
                        lines += prefix + connector + line
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
    val kind: String,
    val fields: kotlin.collections.List<String>,
    val children: Int,
) {
    fun line(scope: Scope): String {
        val fieldText = if (fields.isEmpty()) "" else " ${fields.joinToString(" ")}"
        return "$kind ${scopeText(scope)}$fieldText children=$children"
    }
}

private class DumpVisitor : Visitor<DumpRecord> {
    override fun visitDocument(node: Document): DumpRecord = record("Document", children = node.content.size)

    override fun visitBlockQuote(node: BlockQuote): DumpRecord = record("BlockQuote", children = node.content.size)

    override fun visitParagraph(node: Paragraph): DumpRecord = record("Paragraph", children = node.content.size)

    override fun visitHeading(node: Heading): DumpRecord =
        record("Heading", fields = listOf("level=${node.level}"), children = node.content.size)

    override fun visitThematicBreak(node: ThematicBreak): DumpRecord = record("ThematicBreak")

    override fun visitList(node: List): DumpRecord =
        record(
            "List",
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
            fields = listOf("checked=${node.checked ?: "null"}"),
            children = node.content.size,
        )

    override fun visitCodeBlock(node: CodeBlock): DumpRecord =
        record(
            "CodeBlock",
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
        record("HTMLBlock", fields = listOf("literal=${jsonString(node.literal)}"))

    override fun visitFormulaBlock(node: FormulaBlock): DumpRecord =
        record(
            "FormulaBlock",
            fields = listOf("mode=${node.mode.token()}", "literal=${jsonString(node.literal)}"),
        )

    override fun visitTable(node: Table): DumpRecord =
        record(
            "Table",
            fields = listOf("alignments=[${node.alignments.joinToString(",") { it.token() }}]"),
            children = 1 + node.rows.size,
        )

    override fun visitTableRow(node: TableRow): DumpRecord =
        record(
            "TableRow",
            fields = listOf("isHeader=${node.isHeader}"),
            children = node.cells.size,
        )

    override fun visitTableCell(node: TableCell): DumpRecord = record("TableCell", children = node.content.size)

    override fun visitDirectiveBlock(node: DirectiveBlock): DumpRecord =
        record(
            "DirectiveBlock",
            fields = directiveFields(node.mode, node.name, node.attributes, node.label?.size),
            children = node.label.orEmpty().size + node.content.size,
        )

    override fun visitFootnoteDefinition(node: FootnoteDefinition): DumpRecord =
        record(
            "FootnoteDefinition",
            fields = listOf("id=${jsonString(node.label)}"),
            children = node.content.size,
        )

    override fun visitText(node: Text): DumpRecord =
        record("Text", fields = listOf("literal=${jsonString(node.literal)}"))

    override fun visitSoftBreak(node: SoftBreak): DumpRecord = record("SoftBreak")

    override fun visitLineBreak(node: LineBreak): DumpRecord = record("LineBreak")

    override fun visitCode(node: Code): DumpRecord =
        record(
            "Code",
            fields = listOf("mode=${node.mode.token()}", "literal=${jsonString(node.literal)}"),
        )

    override fun visitHTML(node: HTML): DumpRecord =
        record("HTML", fields = listOf("literal=${jsonString(node.literal)}"))

    override fun visitFormula(node: Formula): DumpRecord =
        record(
            "Formula",
            fields = listOf("mode=${node.mode.token()}", "literal=${jsonString(node.literal)}"),
        )

    override fun visitEmphasis(node: Emphasis): DumpRecord = record("Emphasis", children = node.content.size)

    override fun visitStrong(node: Strong): DumpRecord = record("Strong", children = node.content.size)

    override fun visitStrikethrough(node: Strikethrough): DumpRecord =
        record("Strikethrough", children = node.content.size)

    override fun visitLink(node: Link): DumpRecord =
        record(
            "Link",
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
            fields = directiveFields(node.mode, node.name, node.attributes, node.label?.size),
            children = node.label.orEmpty().size,
        )

    override fun visitFootnoteReference(node: FootnoteReference): DumpRecord =
        record("FootnoteReference", fields = listOf("id=${jsonString(node.label)}"))
}

private fun record(
    kind: String,
    fields: kotlin.collections.List<String> = emptyList(),
    children: Int = 0,
): DumpRecord = DumpRecord(kind, fields, children)

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

private fun scopeText(value: Scope): String =
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
