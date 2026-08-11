@file:kotlin.jvm.JvmName("MarkdownCoreKt")
@file:kotlin.jvm.JvmMultifileClass

package com.nouprax.markdown.core

/** Produces the canonical diagnostic tree for immutable Markdown markup. */
public object MarkupDumper {
    /** Returns the canonical diagnostic dump for [document] and its
     * descendants, resolving absolute scopes through the snapshot. */
    public fun dump(document: Document): String = dump(document, document)

    /**
     * Returns the canonical diagnostic dump for the subtree rooted at
     * [node]. Scopes print with the subtree as origin: the root's start
     * line becomes line 1, later lines shift by the same amount, and
     * columns are line-local and unchanged. Position-free markers
     * (`0:0..0:0`) print unchanged.
     */
    public fun dump(
        document: Document,
        node: Markup,
    ): String {
        val origin = node.scope.start.line
        val offset = if (origin > 0) origin - 1 else 0
        val visitor = DumpVisitor()
        val remainingChildren = mutableListOf<Int>()
        val lines = mutableListOf<String>()

        MarkupWalker.walk(document, node) { event, current, scope ->
            when (event) {
                WalkEvent.ENTERING -> {
                    val record = current.accept(visitor)
                    val line = record.line(scope, offset)
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
    fun line(
        scope: Scope,
        offset: Int,
    ): String {
        val fieldText = if (fields.isEmpty()) "" else " ${fields.joinToString(" ")}"
        return "$kind ${scopeText(scope, offset)}$fieldText children=$children"
    }
}

private class DumpVisitor : MarkupVisitor<DumpRecord> {
    override fun visit(node: Document): DumpRecord = record("Document", children = node.content.size)

    override fun visit(node: BlockQuote): DumpRecord = record("BlockQuote", children = node.content.size)

    override fun visit(node: Paragraph): DumpRecord = record("Paragraph", children = node.content.size)

    override fun visit(node: Heading): DumpRecord =
        record("Heading", fields = listOf("level=${node.level}"), children = node.content.size)

    override fun visit(node: ThematicBreak): DumpRecord = record("ThematicBreak")

    override fun visit(node: List): DumpRecord =
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

    override fun visit(node: ListItem): DumpRecord =
        record(
            "ListItem",
            fields = listOf("checked=${node.checked ?: "null"}"),
            children = node.content.size,
        )

    override fun visit(node: CodeBlock): DumpRecord =
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

    override fun visit(node: HTMLBlock): DumpRecord =
        record("HTMLBlock", fields = listOf("comment=${node.comment}", "literal=${jsonString(node.literal)}"))

    override fun visit(node: FormulaBlock): DumpRecord =
        record(
            "FormulaBlock",
            fields = listOf("mode=${node.mode.token()}", "literal=${jsonString(node.literal)}"),
        )

    override fun visit(node: Table): DumpRecord =
        record(
            "Table",
            fields = listOf("alignments=[${node.alignments.joinToString(",") { it.token() }}]"),
            children = 1 + node.rows.size,
        )

    override fun visit(node: TableRow): DumpRecord =
        record(
            "TableRow",
            fields = listOf("isHeader=${node.isHeader}"),
            children = node.cells.size,
        )

    override fun visit(node: TableCell): DumpRecord = record("TableCell", children = node.content.size)

    override fun visit(node: DirectiveBlock): DumpRecord =
        record(
            "DirectiveBlock",
            fields = directiveFields(node.mode, node.name, node.attributes),
            children = (if (node.label == null) 0 else 1) + node.content.size,
        )

    override fun visit(node: DirectiveLabel): DumpRecord = record("DirectiveLabel", children = node.content.size)

    override fun visit(node: FootnoteDefinition): DumpRecord =
        record(
            "FootnoteDefinition",
            fields = listOf("id=${jsonString(node.label)}"),
            children = node.content.size,
        )

    override fun visit(node: Text): DumpRecord = record("Text", fields = listOf("literal=${jsonString(node.literal)}"))

    override fun visit(node: SoftBreak): DumpRecord = record("SoftBreak")

    override fun visit(node: LineBreak): DumpRecord = record("LineBreak")

    override fun visit(node: Code): DumpRecord =
        record(
            "Code",
            fields = listOf("mode=${node.mode.token()}", "literal=${jsonString(node.literal)}"),
        )

    override fun visit(node: HTML): DumpRecord =
        record("HTML", fields = listOf("comment=${node.comment}", "literal=${jsonString(node.literal)}"))

    override fun visit(node: Formula): DumpRecord =
        record(
            "Formula",
            fields = listOf("mode=${node.mode.token()}", "literal=${jsonString(node.literal)}"),
        )

    override fun visit(node: Emphasis): DumpRecord = record("Emphasis", children = node.content.size)

    override fun visit(node: Strong): DumpRecord = record("Strong", children = node.content.size)

    override fun visit(node: Strikethrough): DumpRecord = record("Strikethrough", children = node.content.size)

    override fun visit(node: Link): DumpRecord =
        record(
            "Link",
            fields =
                listOf(
                    "destination=${optionalString(node.destination)}",
                    "title=${optionalString(node.title)}",
                ),
            children = node.content.size,
        )

    override fun visit(node: Image): DumpRecord =
        record(
            "Image",
            fields =
                listOf(
                    "source=${optionalString(node.source)}",
                    "title=${optionalString(node.title)}",
                ),
            children = node.content.size,
        )

    override fun visit(node: Directive): DumpRecord =
        record(
            "Directive",
            fields = directiveFields(node.mode, node.name, node.attributes),
            children = if (node.label == null) 0 else 1,
        )

    override fun visit(node: FootnoteReference): DumpRecord =
        record("FootnoteReference", fields = listOf("id=${jsonString(node.label)}"))

    override fun visit(node: ReferenceDefinition): DumpRecord =
        record(
            "ReferenceDefinition",
            fields =
                listOf(
                    "label=${jsonString(node.label)}",
                    "destination=${optionalString(node.destination)}",
                    "title=${optionalString(node.title)}",
                ),
        )

    override fun visit(node: LinkReference): DumpRecord =
        referenceRecord("LinkReference", node.label, node.form, node.content.size)

    override fun visit(node: ImageReference): DumpRecord =
        referenceRecord("ImageReference", node.label, node.form, node.content.size)

    override fun visit(node: CrossLink): DumpRecord =
        record("CrossLink", fields = listOf("reference=${jsonString(node.reference)}"))

    override fun visit(node: Embed): DumpRecord =
        record("Embed", fields = listOf("reference=${jsonString(node.reference)}"))
}

private fun referenceRecord(
    kind: String,
    label: String,
    form: ReferenceForm,
    children: Int,
): DumpRecord = DumpRecord(kind, listOf("label=${jsonString(label)}", "form=${form.token()}"), children)

private fun ReferenceForm.token(): String =
    when (this) {
        ReferenceForm.FULL -> "full"
        ReferenceForm.COLLAPSED -> "collapsed"
        ReferenceForm.SHORTCUT -> "shortcut"
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
): kotlin.collections.List<String> =
    listOf(
        "mode=${mode.token()}",
        "name=${jsonString(name)}",
        "attributes=${optionalString(attributes)}",
    )

private fun scopeText(
    value: Scope,
    offset: Int,
): String {
    val start = if (value.start.line > 0) value.start.line - offset else value.start.line
    val end = if (value.end.line > 0) value.end.line - offset else value.end.line
    return "scope=$start:${value.start.column}..$end:${value.end.column}"
}

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
