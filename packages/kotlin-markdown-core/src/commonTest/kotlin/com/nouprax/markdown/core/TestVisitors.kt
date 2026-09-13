package com.nouprax.markdown.core

internal class KindVisitor : Visitor<String> {
    override fun visit(
        citation: Citation,
        phase: MarkupWalkPhase,
    ): String = name(citation)

    override fun visit(
        footnote: Footnote,
        phase: MarkupWalkPhase,
    ): String = name(footnote)

    override fun visit(
        specimen: Specimen,
        phase: MarkupWalkPhase,
    ): String = name(specimen)

    override fun visit(
        metadata: Metadata,
        phase: MarkupWalkPhase,
    ): String = name(metadata)

    override fun visit(
        document: Document,
        phase: MarkupWalkPhase,
    ): String = name(document)

    override fun visit(
        callout: Callout,
        phase: MarkupWalkPhase,
    ): String = name(callout)

    override fun visit(
        paragraph: Paragraph,
        phase: MarkupWalkPhase,
    ): String = name(paragraph)

    override fun visit(
        heading: Heading,
        phase: MarkupWalkPhase,
    ): String = "heading:${heading.level}"

    override fun visit(
        thematicBreak: ThematicBreak,
        phase: MarkupWalkPhase,
    ): String = name(thematicBreak)

    override fun visit(
        list: List,
        phase: MarkupWalkPhase,
    ): String = name(list)

    override fun visit(
        listItem: ListItem,
        phase: MarkupWalkPhase,
    ): String = name(listItem)

    override fun visit(
        codeBlock: CodeBlock,
        phase: MarkupWalkPhase,
    ): String = name(codeBlock)

    override fun visit(
        htmlBlock: HTMLBlock,
        phase: MarkupWalkPhase,
    ): String = name(htmlBlock)

    override fun visit(
        formulaBlock: FormulaBlock,
        phase: MarkupWalkPhase,
    ): String = name(formulaBlock)

    override fun visit(
        table: Table,
        phase: MarkupWalkPhase,
    ): String = name(table)

    override fun visit(
        tableCaption: TableCaption,
        phase: MarkupWalkPhase,
    ): String = name(tableCaption)

    override fun visit(
        tableRow: TableRow,
        phase: MarkupWalkPhase,
    ): String = "row"

    override fun visit(
        tableCell: TableCell,
        phase: MarkupWalkPhase,
    ): String = "cell"

    override fun visit(
        directiveBlock: DirectiveBlock,
        phase: MarkupWalkPhase,
    ): String = name(directiveBlock)

    override fun visit(
        directiveLabel: DirectiveLabel,
        phase: MarkupWalkPhase,
    ): String = name(directiveLabel)

    override fun visit(
        text: Text,
        phase: MarkupWalkPhase,
    ): String = name(text)

    override fun visit(
        softBreak: SoftBreak,
        phase: MarkupWalkPhase,
    ): String = name(softBreak)

    override fun visit(
        lineBreak: LineBreak,
        phase: MarkupWalkPhase,
    ): String = name(lineBreak)

    override fun visit(
        code: Code,
        phase: MarkupWalkPhase,
    ): String = name(code)

    override fun visit(
        html: HTML,
        phase: MarkupWalkPhase,
    ): String = name(html)

    override fun visit(
        comment: Comment,
        phase: MarkupWalkPhase,
    ): String = name(comment)

    override fun visit(
        crossLink: CrossLink,
        phase: MarkupWalkPhase,
    ): String = name(crossLink)

    override fun visit(
        crossEmbedded: CrossEmbedded,
        phase: MarkupWalkPhase,
    ): String = name(crossEmbedded)

    override fun visit(
        formula: Formula,
        phase: MarkupWalkPhase,
    ): String = name(formula)

    override fun visit(
        emphasis: Emphasis,
        phase: MarkupWalkPhase,
    ): String = name(emphasis)

    override fun visit(
        strong: Strong,
        phase: MarkupWalkPhase,
    ): String = name(strong)

    override fun visit(
        strikethrough: Strikethrough,
        phase: MarkupWalkPhase,
    ): String = name(strikethrough)

    override fun visit(
        mark: Mark,
        phase: MarkupWalkPhase,
    ): String = name(mark)

    override fun visit(
        insertion: Insertion,
        phase: MarkupWalkPhase,
    ): String = name(insertion)

    override fun visit(
        span: Span,
        phase: MarkupWalkPhase,
    ): String = name(span)

    override fun visit(
        superscript: Superscript,
        phase: MarkupWalkPhase,
    ): String = name(superscript)

    override fun visit(
        definitionList: DefinitionList,
        phase: MarkupWalkPhase,
    ): String = name(definitionList)

    override fun visit(
        definition: Definition,
        phase: MarkupWalkPhase,
    ): String = name(definition)

    override fun visit(
        subscript: Subscript,
        phase: MarkupWalkPhase,
    ): String = name(subscript)

    override fun visit(
        link: Link,
        phase: MarkupWalkPhase,
    ): String = name(link)

    override fun visit(
        embedded: Embedded,
        phase: MarkupWalkPhase,
    ): String = name(embedded)

    override fun visit(
        directive: Directive,
        phase: MarkupWalkPhase,
    ): String = name(directive)

    override fun visit(
        cite: Cite,
        phase: MarkupWalkPhase,
    ): String = name(cite)
}

internal class RecordingVisitor : Visitor<Unit> {
    override fun visit(
        citation: Citation,
        phase: MarkupWalkPhase,
    ): Unit = record(citation)

    override fun visit(
        footnote: Footnote,
        phase: MarkupWalkPhase,
    ): Unit = record(footnote)

    override fun visit(
        specimen: Specimen,
        phase: MarkupWalkPhase,
    ): Unit = record(specimen)

    override fun visit(
        metadata: Metadata,
        phase: MarkupWalkPhase,
    ): Unit = record(metadata)

    val visited: MutableList<String> = mutableListOf()

    override fun visit(
        document: Document,
        phase: MarkupWalkPhase,
    ): Unit = record(document)

    override fun visit(
        callout: Callout,
        phase: MarkupWalkPhase,
    ): Unit = record(callout)

    override fun visit(
        paragraph: Paragraph,
        phase: MarkupWalkPhase,
    ): Unit = record(paragraph)

    override fun visit(
        heading: Heading,
        phase: MarkupWalkPhase,
    ): Unit = record(heading)

    override fun visit(
        thematicBreak: ThematicBreak,
        phase: MarkupWalkPhase,
    ): Unit = record(thematicBreak)

    override fun visit(
        list: List,
        phase: MarkupWalkPhase,
    ): Unit = record(list)

    override fun visit(
        listItem: ListItem,
        phase: MarkupWalkPhase,
    ): Unit = record(listItem)

    override fun visit(
        codeBlock: CodeBlock,
        phase: MarkupWalkPhase,
    ): Unit = record(codeBlock)

    override fun visit(
        htmlBlock: HTMLBlock,
        phase: MarkupWalkPhase,
    ): Unit = record(htmlBlock)

    override fun visit(
        formulaBlock: FormulaBlock,
        phase: MarkupWalkPhase,
    ): Unit = record(formulaBlock)

    override fun visit(
        table: Table,
        phase: MarkupWalkPhase,
    ): Unit = record(table)

    override fun visit(
        tableCaption: TableCaption,
        phase: MarkupWalkPhase,
    ): Unit = record(tableCaption)

    override fun visit(
        tableRow: TableRow,
        phase: MarkupWalkPhase,
    ): Unit = record(tableRow)

    override fun visit(
        tableCell: TableCell,
        phase: MarkupWalkPhase,
    ): Unit = record(tableCell)

    override fun visit(
        directiveBlock: DirectiveBlock,
        phase: MarkupWalkPhase,
    ): Unit = record(directiveBlock)

    override fun visit(
        directiveLabel: DirectiveLabel,
        phase: MarkupWalkPhase,
    ): Unit = record(directiveLabel)

    override fun visit(
        text: Text,
        phase: MarkupWalkPhase,
    ): Unit = record(text)

    override fun visit(
        softBreak: SoftBreak,
        phase: MarkupWalkPhase,
    ): Unit = record(softBreak)

    override fun visit(
        lineBreak: LineBreak,
        phase: MarkupWalkPhase,
    ): Unit = record(lineBreak)

    override fun visit(
        code: Code,
        phase: MarkupWalkPhase,
    ): Unit = record(code)

    override fun visit(
        html: HTML,
        phase: MarkupWalkPhase,
    ): Unit = record(html)

    override fun visit(
        comment: Comment,
        phase: MarkupWalkPhase,
    ): Unit = record(comment)

    override fun visit(
        crossLink: CrossLink,
        phase: MarkupWalkPhase,
    ): Unit = record(crossLink)

    override fun visit(
        crossEmbedded: CrossEmbedded,
        phase: MarkupWalkPhase,
    ): Unit = record(crossEmbedded)

    override fun visit(
        formula: Formula,
        phase: MarkupWalkPhase,
    ): Unit = record(formula)

    override fun visit(
        emphasis: Emphasis,
        phase: MarkupWalkPhase,
    ): Unit = record(emphasis)

    override fun visit(
        strong: Strong,
        phase: MarkupWalkPhase,
    ): Unit = record(strong)

    override fun visit(
        strikethrough: Strikethrough,
        phase: MarkupWalkPhase,
    ): Unit = record(strikethrough)

    override fun visit(
        mark: Mark,
        phase: MarkupWalkPhase,
    ): Unit = record(mark)

    override fun visit(
        insertion: Insertion,
        phase: MarkupWalkPhase,
    ): Unit = record(insertion)

    override fun visit(
        span: Span,
        phase: MarkupWalkPhase,
    ): Unit = record(span)

    override fun visit(
        superscript: Superscript,
        phase: MarkupWalkPhase,
    ): Unit = record(superscript)

    override fun visit(
        definitionList: DefinitionList,
        phase: MarkupWalkPhase,
    ): Unit = record(definitionList)

    override fun visit(
        definition: Definition,
        phase: MarkupWalkPhase,
    ): Unit = record(definition)

    override fun visit(
        subscript: Subscript,
        phase: MarkupWalkPhase,
    ): Unit = record(subscript)

    override fun visit(
        link: Link,
        phase: MarkupWalkPhase,
    ): Unit = record(link)

    override fun visit(
        embedded: Embedded,
        phase: MarkupWalkPhase,
    ): Unit = record(embedded)

    override fun visit(
        directive: Directive,
        phase: MarkupWalkPhase,
    ): Unit = record(directive)

    override fun visit(
        cite: Cite,
        phase: MarkupWalkPhase,
    ): Unit = record(cite)

    private fun record(node: Markup) {
        visited += name(node)
    }
}

private fun name(node: Markup): String = node::class.simpleName ?: "unknown"

internal class RecordingWalkingVisitor(
    private val recordEvents: Boolean = true,
) : Visitor<Unit> {
    override fun visit(
        metadata: Metadata,
        phase: MarkupWalkPhase,
    ): Unit = record(metadata, phase)

    val events: MutableList<String> = mutableListOf()
    val tableRowKinds: MutableList<Int> = mutableListOf()
    var entered: Int = 0
        private set
    var exited: Int = 0
        private set

    private fun record(
        node: Markup,
        phase: MarkupWalkPhase,
    ) {
        record(name(node), phase)
    }

    private fun record(
        name: String,
        phase: MarkupWalkPhase,
    ) {
        when (phase) {
            MarkupWalkPhase.ENTERING -> entered++
            MarkupWalkPhase.EXITING -> exited++
        }
        if (recordEvents) events += "${phase.name.lowercase()}:$name"
    }

    override fun visit(
        document: Document,
        phase: MarkupWalkPhase,
    ): Unit = record(document, phase)

    override fun visit(
        callout: Callout,
        phase: MarkupWalkPhase,
    ): Unit = record(callout, phase)

    override fun visit(
        paragraph: Paragraph,
        phase: MarkupWalkPhase,
    ): Unit = record(paragraph, phase)

    override fun visit(
        heading: Heading,
        phase: MarkupWalkPhase,
    ): Unit = record(heading, phase)

    override fun visit(
        thematicBreak: ThematicBreak,
        phase: MarkupWalkPhase,
    ): Unit = record(thematicBreak, phase)

    override fun visit(
        list: List,
        phase: MarkupWalkPhase,
    ): Unit = record(list, phase)

    override fun visit(
        listItem: ListItem,
        phase: MarkupWalkPhase,
    ): Unit = record(listItem, phase)

    override fun visit(
        codeBlock: CodeBlock,
        phase: MarkupWalkPhase,
    ): Unit = record(codeBlock, phase)

    override fun visit(
        htmlBlock: HTMLBlock,
        phase: MarkupWalkPhase,
    ): Unit = record(htmlBlock, phase)

    override fun visit(
        formulaBlock: FormulaBlock,
        phase: MarkupWalkPhase,
    ): Unit = record(formulaBlock, phase)

    override fun visit(
        table: Table,
        phase: MarkupWalkPhase,
    ): Unit = record(table, phase)

    override fun visit(
        tableCaption: TableCaption,
        phase: MarkupWalkPhase,
    ): Unit = record(tableCaption, phase)

    override fun visit(
        tableRow: TableRow,
        phase: MarkupWalkPhase,
    ) {
        record(tableRow, phase)
        if (phase == MarkupWalkPhase.ENTERING) tableRowKinds += tableRow.scope.start.line
    }

    override fun visit(
        tableCell: TableCell,
        phase: MarkupWalkPhase,
    ): Unit = record(tableCell, phase)

    override fun visit(
        directiveBlock: DirectiveBlock,
        phase: MarkupWalkPhase,
    ): Unit = record(directiveBlock, phase)

    override fun visit(
        directiveLabel: DirectiveLabel,
        phase: MarkupWalkPhase,
    ): Unit = record(directiveLabel, phase)

    override fun visit(
        text: Text,
        phase: MarkupWalkPhase,
    ): Unit = record(text, phase)

    override fun visit(
        softBreak: SoftBreak,
        phase: MarkupWalkPhase,
    ): Unit = record(softBreak, phase)

    override fun visit(
        lineBreak: LineBreak,
        phase: MarkupWalkPhase,
    ): Unit = record(lineBreak, phase)

    override fun visit(
        code: Code,
        phase: MarkupWalkPhase,
    ): Unit = record(code, phase)

    override fun visit(
        html: HTML,
        phase: MarkupWalkPhase,
    ): Unit = record(html, phase)

    override fun visit(
        comment: Comment,
        phase: MarkupWalkPhase,
    ): Unit = record(comment, phase)

    override fun visit(
        crossLink: CrossLink,
        phase: MarkupWalkPhase,
    ): Unit = record(crossLink, phase)

    override fun visit(
        crossEmbedded: CrossEmbedded,
        phase: MarkupWalkPhase,
    ): Unit = record(crossEmbedded, phase)

    override fun visit(
        formula: Formula,
        phase: MarkupWalkPhase,
    ): Unit = record(formula, phase)

    override fun visit(
        emphasis: Emphasis,
        phase: MarkupWalkPhase,
    ): Unit = record(emphasis, phase)

    override fun visit(
        strong: Strong,
        phase: MarkupWalkPhase,
    ): Unit = record(strong, phase)

    override fun visit(
        strikethrough: Strikethrough,
        phase: MarkupWalkPhase,
    ): Unit = record(strikethrough, phase)

    override fun visit(
        mark: Mark,
        phase: MarkupWalkPhase,
    ): Unit = record(mark, phase)

    override fun visit(
        insertion: Insertion,
        phase: MarkupWalkPhase,
    ): Unit = record(insertion, phase)

    override fun visit(
        span: Span,
        phase: MarkupWalkPhase,
    ): Unit = record(span, phase)

    override fun visit(
        superscript: Superscript,
        phase: MarkupWalkPhase,
    ): Unit = record(superscript, phase)

    override fun visit(
        definitionList: DefinitionList,
        phase: MarkupWalkPhase,
    ): Unit = record(definitionList, phase)

    override fun visit(
        definition: Definition,
        phase: MarkupWalkPhase,
    ): Unit = record(definition, phase)

    override fun visit(
        subscript: Subscript,
        phase: MarkupWalkPhase,
    ): Unit = record(subscript, phase)

    override fun visit(
        link: Link,
        phase: MarkupWalkPhase,
    ): Unit = record(link, phase)

    override fun visit(
        embedded: Embedded,
        phase: MarkupWalkPhase,
    ): Unit = record(embedded, phase)

    override fun visit(
        directive: Directive,
        phase: MarkupWalkPhase,
    ): Unit = record(directive, phase)

    override fun visit(
        cite: Cite,
        phase: MarkupWalkPhase,
    ): Unit = record(cite, phase)

    override fun visit(
        citation: Citation,
        phase: MarkupWalkPhase,
    ): Unit = record("Citation", phase)

    override fun visit(
        specimen: Specimen,
        phase: MarkupWalkPhase,
    ): Unit = record("Specimen", phase)

    override fun visit(
        footnote: Footnote,
        phase: MarkupWalkPhase,
    ): Unit = record("Footnote", phase)
}
