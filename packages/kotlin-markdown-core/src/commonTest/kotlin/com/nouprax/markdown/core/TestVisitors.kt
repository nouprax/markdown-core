package com.nouprax.markdown.core

internal class KindVisitor : Visitor<String> {
    override fun visit(document: Document): String = name(document)

    override fun visit(callout: Callout): String = name(callout)

    override fun visit(paragraph: Paragraph): String = name(paragraph)

    override fun visit(heading: Heading): String = "heading:${heading.level}"

    override fun visit(thematicBreak: ThematicBreak): String = name(thematicBreak)

    override fun visit(list: List): String = name(list)

    override fun visit(listItem: ListItem): String = name(listItem)

    override fun visit(codeBlock: CodeBlock): String = name(codeBlock)

    override fun visit(htmlBlock: HTMLBlock): String = name(htmlBlock)

    override fun visit(formulaBlock: FormulaBlock): String = name(formulaBlock)

    override fun visit(table: Table): String = name(table)

    override fun visit(tableCaption: TableCaption): String = name(tableCaption)

    override fun visit(tableRow: TableRow): String = "row"

    override fun visit(tableCell: TableCell): String = "cell"

    override fun visit(directiveBlock: DirectiveBlock): String = name(directiveBlock)

    override fun visit(directiveLabel: DirectiveLabel): String = name(directiveLabel)

    override fun visit(text: Text): String = name(text)

    override fun visit(softBreak: SoftBreak): String = name(softBreak)

    override fun visit(lineBreak: LineBreak): String = name(lineBreak)

    override fun visit(code: Code): String = name(code)

    override fun visit(html: HTML): String = name(html)

    override fun visit(comment: Comment): String = name(comment)

    override fun visit(crossLink: CrossLink): String = name(crossLink)

    override fun visit(crossEmbedded: CrossEmbedded): String = name(crossEmbedded)

    override fun visit(formula: Formula): String = name(formula)

    override fun visit(emphasis: Emphasis): String = name(emphasis)

    override fun visit(strong: Strong): String = name(strong)

    override fun visit(strikethrough: Strikethrough): String = name(strikethrough)

    override fun visit(mark: Mark): String = name(mark)

    override fun visit(insertion: Insertion): String = name(insertion)

    override fun visit(span: Span): String = name(span)

    override fun visit(superscript: Superscript): String = name(superscript)

    override fun visit(definitionList: DefinitionList): String = name(definitionList)

    override fun visit(definition: Definition): String = name(definition)

    override fun visit(subscript: Subscript): String = name(subscript)

    override fun visit(link: Link): String = name(link)

    override fun visit(embedded: Embedded): String = name(embedded)

    override fun visit(directive: Directive): String = name(directive)

    override fun visit(cite: Cite): String = name(cite)
}

internal class RecordingVisitor : Visitor<Unit> {
    val visited: MutableList<String> = mutableListOf()

    override fun visit(document: Document): Unit = record(document)

    override fun visit(callout: Callout): Unit = record(callout)

    override fun visit(paragraph: Paragraph): Unit = record(paragraph)

    override fun visit(heading: Heading): Unit = record(heading)

    override fun visit(thematicBreak: ThematicBreak): Unit = record(thematicBreak)

    override fun visit(list: List): Unit = record(list)

    override fun visit(listItem: ListItem): Unit = record(listItem)

    override fun visit(codeBlock: CodeBlock): Unit = record(codeBlock)

    override fun visit(htmlBlock: HTMLBlock): Unit = record(htmlBlock)

    override fun visit(formulaBlock: FormulaBlock): Unit = record(formulaBlock)

    override fun visit(table: Table): Unit = record(table)

    override fun visit(tableCaption: TableCaption): Unit = record(tableCaption)

    override fun visit(tableRow: TableRow): Unit = record(tableRow)

    override fun visit(tableCell: TableCell): Unit = record(tableCell)

    override fun visit(directiveBlock: DirectiveBlock): Unit = record(directiveBlock)

    override fun visit(directiveLabel: DirectiveLabel): Unit = record(directiveLabel)

    override fun visit(text: Text): Unit = record(text)

    override fun visit(softBreak: SoftBreak): Unit = record(softBreak)

    override fun visit(lineBreak: LineBreak): Unit = record(lineBreak)

    override fun visit(code: Code): Unit = record(code)

    override fun visit(html: HTML): Unit = record(html)

    override fun visit(comment: Comment): Unit = record(comment)

    override fun visit(crossLink: CrossLink): Unit = record(crossLink)

    override fun visit(crossEmbedded: CrossEmbedded): Unit = record(crossEmbedded)

    override fun visit(formula: Formula): Unit = record(formula)

    override fun visit(emphasis: Emphasis): Unit = record(emphasis)

    override fun visit(strong: Strong): Unit = record(strong)

    override fun visit(strikethrough: Strikethrough): Unit = record(strikethrough)

    override fun visit(mark: Mark): Unit = record(mark)

    override fun visit(insertion: Insertion): Unit = record(insertion)

    override fun visit(span: Span): Unit = record(span)

    override fun visit(superscript: Superscript): Unit = record(superscript)

    override fun visit(definitionList: DefinitionList): Unit = record(definitionList)

    override fun visit(definition: Definition): Unit = record(definition)

    override fun visit(subscript: Subscript): Unit = record(subscript)

    override fun visit(link: Link): Unit = record(link)

    override fun visit(embedded: Embedded): Unit = record(embedded)

    override fun visit(directive: Directive): Unit = record(directive)

    override fun visit(cite: Cite): Unit = record(cite)

    private fun record(node: Markup) {
        visited += name(node)
    }
}

private fun name(node: Markup): String = node::class.simpleName ?: "unknown"

internal class RecordingWalkingVisitor(
    private val recordEvents: Boolean = true,
) : WalkingVisitor {
    val events: MutableList<String> = mutableListOf()
    val tableRowKinds: MutableList<Int> = mutableListOf()
    var entered: Int = 0
        private set
    var exited: Int = 0
        private set

    private fun record(
        node: Markup,
        phase: WalkPhase,
    ) {
        record(name(node), phase)
    }

    private fun record(
        name: String,
        phase: WalkPhase,
    ) {
        when (phase) {
            WalkPhase.ENTERING -> entered++
            WalkPhase.EXITING -> exited++
        }
        if (recordEvents) events += "${phase.name.lowercase()}:$name"
    }

    override fun visit(
        document: Document,
        phase: WalkPhase,
    ): Unit = record(document, phase)

    override fun visit(
        callout: Callout,
        phase: WalkPhase,
    ): Unit = record(callout, phase)

    override fun visit(
        paragraph: Paragraph,
        phase: WalkPhase,
    ): Unit = record(paragraph, phase)

    override fun visit(
        heading: Heading,
        phase: WalkPhase,
    ): Unit = record(heading, phase)

    override fun visit(
        thematicBreak: ThematicBreak,
        phase: WalkPhase,
    ): Unit = record(thematicBreak, phase)

    override fun visit(
        list: List,
        phase: WalkPhase,
    ): Unit = record(list, phase)

    override fun visit(
        listItem: ListItem,
        phase: WalkPhase,
    ): Unit = record(listItem, phase)

    override fun visit(
        codeBlock: CodeBlock,
        phase: WalkPhase,
    ): Unit = record(codeBlock, phase)

    override fun visit(
        htmlBlock: HTMLBlock,
        phase: WalkPhase,
    ): Unit = record(htmlBlock, phase)

    override fun visit(
        formulaBlock: FormulaBlock,
        phase: WalkPhase,
    ): Unit = record(formulaBlock, phase)

    override fun visit(
        table: Table,
        phase: WalkPhase,
    ): Unit = record(table, phase)

    override fun visit(
        tableCaption: TableCaption,
        phase: WalkPhase,
    ): Unit = record(tableCaption, phase)

    override fun visit(
        tableRow: TableRow,
        phase: WalkPhase,
    ) {
        record(tableRow, phase)
        if (phase == WalkPhase.ENTERING) tableRowKinds += tableRow.scope.start.line
    }

    override fun visit(
        tableCell: TableCell,
        phase: WalkPhase,
    ): Unit = record(tableCell, phase)

    override fun visit(
        directiveBlock: DirectiveBlock,
        phase: WalkPhase,
    ): Unit = record(directiveBlock, phase)

    override fun visit(
        directiveLabel: DirectiveLabel,
        phase: WalkPhase,
    ): Unit = record(directiveLabel, phase)

    override fun visit(
        text: Text,
        phase: WalkPhase,
    ): Unit = record(text, phase)

    override fun visit(
        softBreak: SoftBreak,
        phase: WalkPhase,
    ): Unit = record(softBreak, phase)

    override fun visit(
        lineBreak: LineBreak,
        phase: WalkPhase,
    ): Unit = record(lineBreak, phase)

    override fun visit(
        code: Code,
        phase: WalkPhase,
    ): Unit = record(code, phase)

    override fun visit(
        html: HTML,
        phase: WalkPhase,
    ): Unit = record(html, phase)

    override fun visit(
        comment: Comment,
        phase: WalkPhase,
    ): Unit = record(comment, phase)

    override fun visit(
        crossLink: CrossLink,
        phase: WalkPhase,
    ): Unit = record(crossLink, phase)

    override fun visit(
        crossEmbedded: CrossEmbedded,
        phase: WalkPhase,
    ): Unit = record(crossEmbedded, phase)

    override fun visit(
        formula: Formula,
        phase: WalkPhase,
    ): Unit = record(formula, phase)

    override fun visit(
        emphasis: Emphasis,
        phase: WalkPhase,
    ): Unit = record(emphasis, phase)

    override fun visit(
        strong: Strong,
        phase: WalkPhase,
    ): Unit = record(strong, phase)

    override fun visit(
        strikethrough: Strikethrough,
        phase: WalkPhase,
    ): Unit = record(strikethrough, phase)

    override fun visit(
        mark: Mark,
        phase: WalkPhase,
    ): Unit = record(mark, phase)

    override fun visit(
        insertion: Insertion,
        phase: WalkPhase,
    ): Unit = record(insertion, phase)

    override fun visit(
        span: Span,
        phase: WalkPhase,
    ): Unit = record(span, phase)

    override fun visit(
        superscript: Superscript,
        phase: WalkPhase,
    ): Unit = record(superscript, phase)

    override fun visit(
        definitionList: DefinitionList,
        phase: WalkPhase,
    ): Unit = record(definitionList, phase)

    override fun visit(
        definition: Definition,
        phase: WalkPhase,
    ): Unit = record(definition, phase)

    override fun visit(
        subscript: Subscript,
        phase: WalkPhase,
    ): Unit = record(subscript, phase)

    override fun visit(
        link: Link,
        phase: WalkPhase,
    ): Unit = record(link, phase)

    override fun visit(
        embedded: Embedded,
        phase: WalkPhase,
    ): Unit = record(embedded, phase)

    override fun visit(
        directive: Directive,
        phase: WalkPhase,
    ): Unit = record(directive, phase)

    override fun visit(
        cite: Cite,
        phase: WalkPhase,
    ): Unit = record(cite, phase)

    override fun visit(
        citation: Citation,
        phase: WalkPhase,
    ): Unit = record("Citation", phase)

    override fun visit(
        specimen: Specimen,
        phase: WalkPhase,
    ): Unit = record("Specimen", phase)

    override fun visit(
        footnote: Footnote,
        phase: WalkPhase,
    ): Unit = record("Footnote", phase)
}
