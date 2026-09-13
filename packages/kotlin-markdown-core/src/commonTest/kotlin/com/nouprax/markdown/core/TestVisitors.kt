package com.nouprax.markdown.core

internal class KindVisitor : MarkupVisitor {
    val kinds = mutableListOf<String>()

    private fun record(
        kind: String,
        phase: MarkupVisitPhase,
    ) {
        if (phase == MarkupVisitPhase.ENTER) kinds += kind
    }

    override fun visit(
        citation: Citation,
        phase: MarkupVisitPhase,
    ): Unit = record(name(citation), phase)

    override fun visit(
        footnote: Footnote,
        phase: MarkupVisitPhase,
    ): Unit = record(name(footnote), phase)

    override fun visit(
        specimen: Specimen,
        phase: MarkupVisitPhase,
    ): Unit = record(name(specimen), phase)

    override fun visit(
        metadata: Metadata,
        phase: MarkupVisitPhase,
    ): Unit = record(name(metadata), phase)

    override fun visit(
        document: Document,
        phase: MarkupVisitPhase,
    ): Unit = record(name(document), phase)

    override fun visit(
        callout: Callout,
        phase: MarkupVisitPhase,
    ): Unit = record(name(callout), phase)

    override fun visit(
        paragraph: Paragraph,
        phase: MarkupVisitPhase,
    ): Unit = record(name(paragraph), phase)

    override fun visit(
        heading: Heading,
        phase: MarkupVisitPhase,
    ): Unit = record("heading:${heading.level}", phase)

    override fun visit(
        thematicBreak: ThematicBreak,
        phase: MarkupVisitPhase,
    ): Unit = record(name(thematicBreak), phase)

    override fun visit(
        list: List,
        phase: MarkupVisitPhase,
    ): Unit = record(name(list), phase)

    override fun visit(
        listItem: ListItem,
        phase: MarkupVisitPhase,
    ): Unit = record(name(listItem), phase)

    override fun visit(
        codeBlock: CodeBlock,
        phase: MarkupVisitPhase,
    ): Unit = record(name(codeBlock), phase)

    override fun visit(
        htmlBlock: HTMLBlock,
        phase: MarkupVisitPhase,
    ): Unit = record(name(htmlBlock), phase)

    override fun visit(
        formulaBlock: FormulaBlock,
        phase: MarkupVisitPhase,
    ): Unit = record(name(formulaBlock), phase)

    override fun visit(
        table: Table,
        phase: MarkupVisitPhase,
    ): Unit = record(name(table), phase)

    override fun visit(
        tableCaption: TableCaption,
        phase: MarkupVisitPhase,
    ): Unit = record(name(tableCaption), phase)

    override fun visit(
        tableRow: TableRow,
        phase: MarkupVisitPhase,
    ): Unit = record("row", phase)

    override fun visit(
        tableCell: TableCell,
        phase: MarkupVisitPhase,
    ): Unit = record("cell", phase)

    override fun visit(
        directiveBlock: DirectiveBlock,
        phase: MarkupVisitPhase,
    ): Unit = record(name(directiveBlock), phase)

    override fun visit(
        directiveLabel: DirectiveLabel,
        phase: MarkupVisitPhase,
    ): Unit = record(name(directiveLabel), phase)

    override fun visit(
        text: Text,
        phase: MarkupVisitPhase,
    ): Unit = record(name(text), phase)

    override fun visit(
        softBreak: SoftBreak,
        phase: MarkupVisitPhase,
    ): Unit = record(name(softBreak), phase)

    override fun visit(
        lineBreak: LineBreak,
        phase: MarkupVisitPhase,
    ): Unit = record(name(lineBreak), phase)

    override fun visit(
        code: Code,
        phase: MarkupVisitPhase,
    ): Unit = record(name(code), phase)

    override fun visit(
        html: HTML,
        phase: MarkupVisitPhase,
    ): Unit = record(name(html), phase)

    override fun visit(
        comment: Comment,
        phase: MarkupVisitPhase,
    ): Unit = record(name(comment), phase)

    override fun visit(
        crossLink: CrossLink,
        phase: MarkupVisitPhase,
    ): Unit = record(name(crossLink), phase)

    override fun visit(
        crossEmbedded: CrossEmbedded,
        phase: MarkupVisitPhase,
    ): Unit = record(name(crossEmbedded), phase)

    override fun visit(
        formula: Formula,
        phase: MarkupVisitPhase,
    ): Unit = record(name(formula), phase)

    override fun visit(
        emphasis: Emphasis,
        phase: MarkupVisitPhase,
    ): Unit = record(name(emphasis), phase)

    override fun visit(
        strong: Strong,
        phase: MarkupVisitPhase,
    ): Unit = record(name(strong), phase)

    override fun visit(
        strikethrough: Strikethrough,
        phase: MarkupVisitPhase,
    ): Unit = record(name(strikethrough), phase)

    override fun visit(
        mark: Mark,
        phase: MarkupVisitPhase,
    ): Unit = record(name(mark), phase)

    override fun visit(
        insertion: Insertion,
        phase: MarkupVisitPhase,
    ): Unit = record(name(insertion), phase)

    override fun visit(
        span: Span,
        phase: MarkupVisitPhase,
    ): Unit = record(name(span), phase)

    override fun visit(
        superscript: Superscript,
        phase: MarkupVisitPhase,
    ): Unit = record(name(superscript), phase)

    override fun visit(
        definitionList: DefinitionList,
        phase: MarkupVisitPhase,
    ): Unit = record(name(definitionList), phase)

    override fun visit(
        definition: Definition,
        phase: MarkupVisitPhase,
    ): Unit = record(name(definition), phase)

    override fun visit(
        subscript: Subscript,
        phase: MarkupVisitPhase,
    ): Unit = record(name(subscript), phase)

    override fun visit(
        link: Link,
        phase: MarkupVisitPhase,
    ): Unit = record(name(link), phase)

    override fun visit(
        embedded: Embedded,
        phase: MarkupVisitPhase,
    ): Unit = record(name(embedded), phase)

    override fun visit(
        directive: Directive,
        phase: MarkupVisitPhase,
    ): Unit = record(name(directive), phase)

    override fun visit(
        cite: Cite,
        phase: MarkupVisitPhase,
    ): Unit = record(name(cite), phase)
}

internal class RecordingVisitor : MarkupVisitor {
    override fun visit(
        citation: Citation,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(citation) else Unit

    override fun visit(
        footnote: Footnote,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(footnote) else Unit

    override fun visit(
        specimen: Specimen,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(specimen) else Unit

    override fun visit(
        metadata: Metadata,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(metadata) else Unit

    val visited: MutableList<String> = mutableListOf()

    override fun visit(
        document: Document,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(document) else Unit

    override fun visit(
        callout: Callout,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(callout) else Unit

    override fun visit(
        paragraph: Paragraph,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(paragraph) else Unit

    override fun visit(
        heading: Heading,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(heading) else Unit

    override fun visit(
        thematicBreak: ThematicBreak,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(thematicBreak) else Unit

    override fun visit(
        list: List,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(list) else Unit

    override fun visit(
        listItem: ListItem,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(listItem) else Unit

    override fun visit(
        codeBlock: CodeBlock,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(codeBlock) else Unit

    override fun visit(
        htmlBlock: HTMLBlock,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(htmlBlock) else Unit

    override fun visit(
        formulaBlock: FormulaBlock,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(formulaBlock) else Unit

    override fun visit(
        table: Table,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(table) else Unit

    override fun visit(
        tableCaption: TableCaption,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(tableCaption) else Unit

    override fun visit(
        tableRow: TableRow,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(tableRow) else Unit

    override fun visit(
        tableCell: TableCell,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(tableCell) else Unit

    override fun visit(
        directiveBlock: DirectiveBlock,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(directiveBlock) else Unit

    override fun visit(
        directiveLabel: DirectiveLabel,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(directiveLabel) else Unit

    override fun visit(
        text: Text,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(text) else Unit

    override fun visit(
        softBreak: SoftBreak,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(softBreak) else Unit

    override fun visit(
        lineBreak: LineBreak,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(lineBreak) else Unit

    override fun visit(
        code: Code,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(code) else Unit

    override fun visit(
        html: HTML,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(html) else Unit

    override fun visit(
        comment: Comment,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(comment) else Unit

    override fun visit(
        crossLink: CrossLink,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(crossLink) else Unit

    override fun visit(
        crossEmbedded: CrossEmbedded,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(crossEmbedded) else Unit

    override fun visit(
        formula: Formula,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(formula) else Unit

    override fun visit(
        emphasis: Emphasis,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(emphasis) else Unit

    override fun visit(
        strong: Strong,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(strong) else Unit

    override fun visit(
        strikethrough: Strikethrough,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(strikethrough) else Unit

    override fun visit(
        mark: Mark,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(mark) else Unit

    override fun visit(
        insertion: Insertion,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(insertion) else Unit

    override fun visit(
        span: Span,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(span) else Unit

    override fun visit(
        superscript: Superscript,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(superscript) else Unit

    override fun visit(
        definitionList: DefinitionList,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(definitionList) else Unit

    override fun visit(
        definition: Definition,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(definition) else Unit

    override fun visit(
        subscript: Subscript,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(subscript) else Unit

    override fun visit(
        link: Link,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(link) else Unit

    override fun visit(
        embedded: Embedded,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(embedded) else Unit

    override fun visit(
        directive: Directive,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(directive) else Unit

    override fun visit(
        cite: Cite,
        phase: MarkupVisitPhase,
    ): Unit = if (phase == MarkupVisitPhase.ENTER) record(cite) else Unit

    private fun record(node: Markup) {
        visited += name(node)
    }
}

private fun name(node: Markup): String = node::class.simpleName ?: "unknown"

internal class RecordingWalkingVisitor(
    private val recordEvents: Boolean = true,
) : MarkupVisitor {
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
            MarkupVisitPhase.ENTER -> entered++
            MarkupVisitPhase.EXIT -> exited++
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
        if (phase == MarkupVisitPhase.ENTER) tableRowKinds += tableRow.scope.start.line
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
