package com.nouprax.markdown.core

internal class KindVisitor : Visitor<String> {
    override fun visit(
        citation: Citation,
        phase: MarkupVisitPhase,
    ): String = name(citation)

    override fun visit(
        footnote: Footnote,
        phase: MarkupVisitPhase,
    ): String = name(footnote)

    override fun visit(
        specimen: Specimen,
        phase: MarkupVisitPhase,
    ): String = name(specimen)

    override fun visit(
        metadata: Metadata,
        phase: MarkupVisitPhase,
    ): String = name(metadata)

    override fun visit(
        document: Document,
        phase: MarkupVisitPhase,
    ): String = name(document)

    override fun visit(
        callout: Callout,
        phase: MarkupVisitPhase,
    ): String = name(callout)

    override fun visit(
        paragraph: Paragraph,
        phase: MarkupVisitPhase,
    ): String = name(paragraph)

    override fun visit(
        heading: Heading,
        phase: MarkupVisitPhase,
    ): String = "heading:${heading.level}"

    override fun visit(
        thematicBreak: ThematicBreak,
        phase: MarkupVisitPhase,
    ): String = name(thematicBreak)

    override fun visit(
        list: List,
        phase: MarkupVisitPhase,
    ): String = name(list)

    override fun visit(
        listItem: ListItem,
        phase: MarkupVisitPhase,
    ): String = name(listItem)

    override fun visit(
        codeBlock: CodeBlock,
        phase: MarkupVisitPhase,
    ): String = name(codeBlock)

    override fun visit(
        htmlBlock: HTMLBlock,
        phase: MarkupVisitPhase,
    ): String = name(htmlBlock)

    override fun visit(
        formulaBlock: FormulaBlock,
        phase: MarkupVisitPhase,
    ): String = name(formulaBlock)

    override fun visit(
        table: Table,
        phase: MarkupVisitPhase,
    ): String = name(table)

    override fun visit(
        tableCaption: TableCaption,
        phase: MarkupVisitPhase,
    ): String = name(tableCaption)

    override fun visit(
        tableRow: TableRow,
        phase: MarkupVisitPhase,
    ): String = "row"

    override fun visit(
        tableCell: TableCell,
        phase: MarkupVisitPhase,
    ): String = "cell"

    override fun visit(
        directiveBlock: DirectiveBlock,
        phase: MarkupVisitPhase,
    ): String = name(directiveBlock)

    override fun visit(
        directiveLabel: DirectiveLabel,
        phase: MarkupVisitPhase,
    ): String = name(directiveLabel)

    override fun visit(
        text: Text,
        phase: MarkupVisitPhase,
    ): String = name(text)

    override fun visit(
        softBreak: SoftBreak,
        phase: MarkupVisitPhase,
    ): String = name(softBreak)

    override fun visit(
        lineBreak: LineBreak,
        phase: MarkupVisitPhase,
    ): String = name(lineBreak)

    override fun visit(
        code: Code,
        phase: MarkupVisitPhase,
    ): String = name(code)

    override fun visit(
        html: HTML,
        phase: MarkupVisitPhase,
    ): String = name(html)

    override fun visit(
        comment: Comment,
        phase: MarkupVisitPhase,
    ): String = name(comment)

    override fun visit(
        crossLink: CrossLink,
        phase: MarkupVisitPhase,
    ): String = name(crossLink)

    override fun visit(
        crossEmbedded: CrossEmbedded,
        phase: MarkupVisitPhase,
    ): String = name(crossEmbedded)

    override fun visit(
        formula: Formula,
        phase: MarkupVisitPhase,
    ): String = name(formula)

    override fun visit(
        emphasis: Emphasis,
        phase: MarkupVisitPhase,
    ): String = name(emphasis)

    override fun visit(
        strong: Strong,
        phase: MarkupVisitPhase,
    ): String = name(strong)

    override fun visit(
        strikethrough: Strikethrough,
        phase: MarkupVisitPhase,
    ): String = name(strikethrough)

    override fun visit(
        mark: Mark,
        phase: MarkupVisitPhase,
    ): String = name(mark)

    override fun visit(
        insertion: Insertion,
        phase: MarkupVisitPhase,
    ): String = name(insertion)

    override fun visit(
        span: Span,
        phase: MarkupVisitPhase,
    ): String = name(span)

    override fun visit(
        superscript: Superscript,
        phase: MarkupVisitPhase,
    ): String = name(superscript)

    override fun visit(
        definitionList: DefinitionList,
        phase: MarkupVisitPhase,
    ): String = name(definitionList)

    override fun visit(
        definition: Definition,
        phase: MarkupVisitPhase,
    ): String = name(definition)

    override fun visit(
        subscript: Subscript,
        phase: MarkupVisitPhase,
    ): String = name(subscript)

    override fun visit(
        link: Link,
        phase: MarkupVisitPhase,
    ): String = name(link)

    override fun visit(
        embedded: Embedded,
        phase: MarkupVisitPhase,
    ): String = name(embedded)

    override fun visit(
        directive: Directive,
        phase: MarkupVisitPhase,
    ): String = name(directive)

    override fun visit(
        cite: Cite,
        phase: MarkupVisitPhase,
    ): String = name(cite)
}

internal class RecordingVisitor : Visitor<Unit> {
    override fun visit(
        citation: Citation,
        phase: MarkupVisitPhase,
    ): Unit = record(citation)

    override fun visit(
        footnote: Footnote,
        phase: MarkupVisitPhase,
    ): Unit = record(footnote)

    override fun visit(
        specimen: Specimen,
        phase: MarkupVisitPhase,
    ): Unit = record(specimen)

    override fun visit(
        metadata: Metadata,
        phase: MarkupVisitPhase,
    ): Unit = record(metadata)

    val visited: MutableList<String> = mutableListOf()

    override fun visit(
        document: Document,
        phase: MarkupVisitPhase,
    ): Unit = record(document)

    override fun visit(
        callout: Callout,
        phase: MarkupVisitPhase,
    ): Unit = record(callout)

    override fun visit(
        paragraph: Paragraph,
        phase: MarkupVisitPhase,
    ): Unit = record(paragraph)

    override fun visit(
        heading: Heading,
        phase: MarkupVisitPhase,
    ): Unit = record(heading)

    override fun visit(
        thematicBreak: ThematicBreak,
        phase: MarkupVisitPhase,
    ): Unit = record(thematicBreak)

    override fun visit(
        list: List,
        phase: MarkupVisitPhase,
    ): Unit = record(list)

    override fun visit(
        listItem: ListItem,
        phase: MarkupVisitPhase,
    ): Unit = record(listItem)

    override fun visit(
        codeBlock: CodeBlock,
        phase: MarkupVisitPhase,
    ): Unit = record(codeBlock)

    override fun visit(
        htmlBlock: HTMLBlock,
        phase: MarkupVisitPhase,
    ): Unit = record(htmlBlock)

    override fun visit(
        formulaBlock: FormulaBlock,
        phase: MarkupVisitPhase,
    ): Unit = record(formulaBlock)

    override fun visit(
        table: Table,
        phase: MarkupVisitPhase,
    ): Unit = record(table)

    override fun visit(
        tableCaption: TableCaption,
        phase: MarkupVisitPhase,
    ): Unit = record(tableCaption)

    override fun visit(
        tableRow: TableRow,
        phase: MarkupVisitPhase,
    ): Unit = record(tableRow)

    override fun visit(
        tableCell: TableCell,
        phase: MarkupVisitPhase,
    ): Unit = record(tableCell)

    override fun visit(
        directiveBlock: DirectiveBlock,
        phase: MarkupVisitPhase,
    ): Unit = record(directiveBlock)

    override fun visit(
        directiveLabel: DirectiveLabel,
        phase: MarkupVisitPhase,
    ): Unit = record(directiveLabel)

    override fun visit(
        text: Text,
        phase: MarkupVisitPhase,
    ): Unit = record(text)

    override fun visit(
        softBreak: SoftBreak,
        phase: MarkupVisitPhase,
    ): Unit = record(softBreak)

    override fun visit(
        lineBreak: LineBreak,
        phase: MarkupVisitPhase,
    ): Unit = record(lineBreak)

    override fun visit(
        code: Code,
        phase: MarkupVisitPhase,
    ): Unit = record(code)

    override fun visit(
        html: HTML,
        phase: MarkupVisitPhase,
    ): Unit = record(html)

    override fun visit(
        comment: Comment,
        phase: MarkupVisitPhase,
    ): Unit = record(comment)

    override fun visit(
        crossLink: CrossLink,
        phase: MarkupVisitPhase,
    ): Unit = record(crossLink)

    override fun visit(
        crossEmbedded: CrossEmbedded,
        phase: MarkupVisitPhase,
    ): Unit = record(crossEmbedded)

    override fun visit(
        formula: Formula,
        phase: MarkupVisitPhase,
    ): Unit = record(formula)

    override fun visit(
        emphasis: Emphasis,
        phase: MarkupVisitPhase,
    ): Unit = record(emphasis)

    override fun visit(
        strong: Strong,
        phase: MarkupVisitPhase,
    ): Unit = record(strong)

    override fun visit(
        strikethrough: Strikethrough,
        phase: MarkupVisitPhase,
    ): Unit = record(strikethrough)

    override fun visit(
        mark: Mark,
        phase: MarkupVisitPhase,
    ): Unit = record(mark)

    override fun visit(
        insertion: Insertion,
        phase: MarkupVisitPhase,
    ): Unit = record(insertion)

    override fun visit(
        span: Span,
        phase: MarkupVisitPhase,
    ): Unit = record(span)

    override fun visit(
        superscript: Superscript,
        phase: MarkupVisitPhase,
    ): Unit = record(superscript)

    override fun visit(
        definitionList: DefinitionList,
        phase: MarkupVisitPhase,
    ): Unit = record(definitionList)

    override fun visit(
        definition: Definition,
        phase: MarkupVisitPhase,
    ): Unit = record(definition)

    override fun visit(
        subscript: Subscript,
        phase: MarkupVisitPhase,
    ): Unit = record(subscript)

    override fun visit(
        link: Link,
        phase: MarkupVisitPhase,
    ): Unit = record(link)

    override fun visit(
        embedded: Embedded,
        phase: MarkupVisitPhase,
    ): Unit = record(embedded)

    override fun visit(
        directive: Directive,
        phase: MarkupVisitPhase,
    ): Unit = record(directive)

    override fun visit(
        cite: Cite,
        phase: MarkupVisitPhase,
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
        phase: MarkupVisitPhase,
    ): Unit = record(metadata, phase)

    val events: MutableList<String> = mutableListOf()
    val tableRowKinds: MutableList<Int> = mutableListOf()
    var entered: Int = 0
        private set
    var exited: Int = 0
        private set

    private fun record(
        node: Markup,
        phase: MarkupVisitPhase,
    ) {
        record(name(node), phase)
    }

    private fun record(
        name: String,
        phase: MarkupVisitPhase,
    ) {
        when (phase) {
            MarkupVisitPhase.ENTERING -> entered++
            MarkupVisitPhase.EXITING -> exited++
        }
        if (recordEvents) events += "${phase.name.lowercase()}:$name"
    }

    override fun visit(
        document: Document,
        phase: MarkupVisitPhase,
    ): Unit = record(document, phase)

    override fun visit(
        callout: Callout,
        phase: MarkupVisitPhase,
    ): Unit = record(callout, phase)

    override fun visit(
        paragraph: Paragraph,
        phase: MarkupVisitPhase,
    ): Unit = record(paragraph, phase)

    override fun visit(
        heading: Heading,
        phase: MarkupVisitPhase,
    ): Unit = record(heading, phase)

    override fun visit(
        thematicBreak: ThematicBreak,
        phase: MarkupVisitPhase,
    ): Unit = record(thematicBreak, phase)

    override fun visit(
        list: List,
        phase: MarkupVisitPhase,
    ): Unit = record(list, phase)

    override fun visit(
        listItem: ListItem,
        phase: MarkupVisitPhase,
    ): Unit = record(listItem, phase)

    override fun visit(
        codeBlock: CodeBlock,
        phase: MarkupVisitPhase,
    ): Unit = record(codeBlock, phase)

    override fun visit(
        htmlBlock: HTMLBlock,
        phase: MarkupVisitPhase,
    ): Unit = record(htmlBlock, phase)

    override fun visit(
        formulaBlock: FormulaBlock,
        phase: MarkupVisitPhase,
    ): Unit = record(formulaBlock, phase)

    override fun visit(
        table: Table,
        phase: MarkupVisitPhase,
    ): Unit = record(table, phase)

    override fun visit(
        tableCaption: TableCaption,
        phase: MarkupVisitPhase,
    ): Unit = record(tableCaption, phase)

    override fun visit(
        tableRow: TableRow,
        phase: MarkupVisitPhase,
    ) {
        record(tableRow, phase)
        if (phase == MarkupVisitPhase.ENTERING) tableRowKinds += tableRow.scope.start.line
    }

    override fun visit(
        tableCell: TableCell,
        phase: MarkupVisitPhase,
    ): Unit = record(tableCell, phase)

    override fun visit(
        directiveBlock: DirectiveBlock,
        phase: MarkupVisitPhase,
    ): Unit = record(directiveBlock, phase)

    override fun visit(
        directiveLabel: DirectiveLabel,
        phase: MarkupVisitPhase,
    ): Unit = record(directiveLabel, phase)

    override fun visit(
        text: Text,
        phase: MarkupVisitPhase,
    ): Unit = record(text, phase)

    override fun visit(
        softBreak: SoftBreak,
        phase: MarkupVisitPhase,
    ): Unit = record(softBreak, phase)

    override fun visit(
        lineBreak: LineBreak,
        phase: MarkupVisitPhase,
    ): Unit = record(lineBreak, phase)

    override fun visit(
        code: Code,
        phase: MarkupVisitPhase,
    ): Unit = record(code, phase)

    override fun visit(
        html: HTML,
        phase: MarkupVisitPhase,
    ): Unit = record(html, phase)

    override fun visit(
        comment: Comment,
        phase: MarkupVisitPhase,
    ): Unit = record(comment, phase)

    override fun visit(
        crossLink: CrossLink,
        phase: MarkupVisitPhase,
    ): Unit = record(crossLink, phase)

    override fun visit(
        crossEmbedded: CrossEmbedded,
        phase: MarkupVisitPhase,
    ): Unit = record(crossEmbedded, phase)

    override fun visit(
        formula: Formula,
        phase: MarkupVisitPhase,
    ): Unit = record(formula, phase)

    override fun visit(
        emphasis: Emphasis,
        phase: MarkupVisitPhase,
    ): Unit = record(emphasis, phase)

    override fun visit(
        strong: Strong,
        phase: MarkupVisitPhase,
    ): Unit = record(strong, phase)

    override fun visit(
        strikethrough: Strikethrough,
        phase: MarkupVisitPhase,
    ): Unit = record(strikethrough, phase)

    override fun visit(
        mark: Mark,
        phase: MarkupVisitPhase,
    ): Unit = record(mark, phase)

    override fun visit(
        insertion: Insertion,
        phase: MarkupVisitPhase,
    ): Unit = record(insertion, phase)

    override fun visit(
        span: Span,
        phase: MarkupVisitPhase,
    ): Unit = record(span, phase)

    override fun visit(
        superscript: Superscript,
        phase: MarkupVisitPhase,
    ): Unit = record(superscript, phase)

    override fun visit(
        definitionList: DefinitionList,
        phase: MarkupVisitPhase,
    ): Unit = record(definitionList, phase)

    override fun visit(
        definition: Definition,
        phase: MarkupVisitPhase,
    ): Unit = record(definition, phase)

    override fun visit(
        subscript: Subscript,
        phase: MarkupVisitPhase,
    ): Unit = record(subscript, phase)

    override fun visit(
        link: Link,
        phase: MarkupVisitPhase,
    ): Unit = record(link, phase)

    override fun visit(
        embedded: Embedded,
        phase: MarkupVisitPhase,
    ): Unit = record(embedded, phase)

    override fun visit(
        directive: Directive,
        phase: MarkupVisitPhase,
    ): Unit = record(directive, phase)

    override fun visit(
        cite: Cite,
        phase: MarkupVisitPhase,
    ): Unit = record(cite, phase)

    override fun visit(
        citation: Citation,
        phase: MarkupVisitPhase,
    ): Unit = record("Citation", phase)

    override fun visit(
        specimen: Specimen,
        phase: MarkupVisitPhase,
    ): Unit = record("Specimen", phase)

    override fun visit(
        footnote: Footnote,
        phase: MarkupVisitPhase,
    ): Unit = record("Footnote", phase)
}
