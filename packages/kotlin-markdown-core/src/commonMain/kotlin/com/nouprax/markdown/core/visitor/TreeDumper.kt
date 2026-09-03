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
        val fieldText = if (fields.isEmpty()) "" else " ${fields.joinToString(" ")}"
        val text = "$kind ${scope(node.scope)}$fieldText children=$children"
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
        state.container("Document", node, children = node.content)
    }

    override fun visitBlockQuote(node: BlockQuote) {
        state.container("BlockQuote", node, children = node.content)
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

    override fun visitFootnoteDefinition(node: FootnoteDefinition) {
        state.container("FootnoteDefinition", node, association(node.label, node.identifier), node.content)
    }

    override fun visitReferenceDefinition(node: ReferenceDefinition) {
        state.line(
            "ReferenceDefinition",
            node,
            association(node.label, node.identifier) +
                listOf(
                    "destination=${jsonString(node.destination)}",
                    "title=${optionalString(node.title)}",
                ),
        )
    }

    override fun visitLinkReference(node: LinkReference) {
        state.container(
            "LinkReference",
            node,
            association(node.label, node.identifier) + listOf("form=${formName(node.form)}"),
            node.content,
        )
    }

    override fun visitImageReference(node: ImageReference) {
        state.container(
            "ImageReference",
            node,
            association(node.label, node.identifier) + listOf("form=${formName(node.form)}"),
            node.content,
        )
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
                "destination=${jsonString(node.destination)}",
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
                "source=${jsonString(node.source)}",
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

    override fun visitFootnoteReference(node: FootnoteReference) {
        state.line("FootnoteReference", node, association(node.label, node.identifier))
    }

    private fun association(
        label: String,
        identifier: String,
    ): kotlin.collections.List<String> = listOf("label=${jsonString(label)}", "identifier=${jsonString(identifier)}")

    private fun formName(form: ReferenceForm): String =
        when (form) {
            ReferenceForm.FULL -> "full"
            ReferenceForm.COLLAPSED -> "collapsed"
            ReferenceForm.SHORTCUT -> "shortcut"
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
