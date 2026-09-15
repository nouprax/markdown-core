package com.nouprax.markdown.core

// The walker selects typed callbacks and schedules each kind's named fields.
internal class MarkupWalker(
    private val visitor: MarkupVisitor,
) {
    private val actions = mutableListOf<Pair<Markup, MarkupVisitPhase>>()

    fun walk(root: Markup) {
        actions += root to MarkupVisitPhase.ENTER
        while (actions.isNotEmpty()) {
            val (node, phase) = actions.removeAt(actions.lastIndex)
            if (phase == MarkupVisitPhase.ENTER) actions += node to MarkupVisitPhase.EXIT
            dispatch(node, phase)
        }
    }

    private fun schedule(node: Markup) {
        actions += node to MarkupVisitPhase.ENTER
    }

    private fun schedule(nodes: kotlin.collections.List<Markup>) {
        for (index in nodes.indices.reversed()) schedule(nodes[index])
    }

    private fun dispatch(
        node: Markup,
        phase: MarkupVisitPhase,
    ) {
        when (node) {
            is Document -> dispatch(node, phase)
            is Callout -> dispatch(node, phase)
            is Paragraph -> dispatch(node, phase)
            is Heading -> dispatch(node, phase)
            is ThematicBreak -> dispatch(node, phase)
            is List -> dispatch(node, phase)
            is ListItem -> dispatch(node, phase)
            is CodeBlock -> dispatch(node, phase)
            is HTMLBlock -> dispatch(node, phase)
            is FormulaBlock -> dispatch(node, phase)
            is Table -> dispatch(node, phase)
            is TableCaption -> dispatch(node, phase)
            is TableRow -> dispatch(node, phase)
            is TableCell -> dispatch(node, phase)
            is DirectiveBlock -> dispatch(node, phase)
            is DirectiveLabel -> dispatch(node, phase)
            is Text -> dispatch(node, phase)
            is SoftBreak -> dispatch(node, phase)
            is LineBreak -> dispatch(node, phase)
            is Code -> dispatch(node, phase)
            is HTML -> dispatch(node, phase)
            is Comment -> dispatch(node, phase)
            is CrossLink -> dispatch(node, phase)
            is CrossEmbedded -> dispatch(node, phase)
            is Formula -> dispatch(node, phase)
            is Emphasis -> dispatch(node, phase)
            is Strong -> dispatch(node, phase)
            is Strikethrough -> dispatch(node, phase)
            is Mark -> dispatch(node, phase)
            is Insertion -> dispatch(node, phase)
            is Span -> dispatch(node, phase)
            is Superscript -> dispatch(node, phase)
            is Subscript -> dispatch(node, phase)
            is DefinitionList -> dispatch(node, phase)
            is Definition -> dispatch(node, phase)
            is Link -> dispatch(node, phase)
            is Embedded -> dispatch(node, phase)
            is Directive -> dispatch(node, phase)
            is Cite -> dispatch(node, phase)
            is Citation -> dispatch(node, phase)
            is Footnote -> dispatch(node, phase)
            is Specimen -> dispatch(node, phase)
            is Metadata -> dispatch(node, phase)
        }
    }

    private fun dispatch(
        document: Document,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(document, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(document.specimens)
        schedule(document.footnotes)
        schedule(document.content)
        document.metadata?.let { schedule(it) }
    }

    private fun dispatch(
        callout: Callout,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(callout, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(callout.content)
        callout.title?.let(::schedule)
    }

    private fun dispatch(
        paragraph: Paragraph,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(paragraph, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(paragraph.content)
    }

    private fun dispatch(
        heading: Heading,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(heading, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(heading.content)
    }

    private fun dispatch(
        thematicBreak: ThematicBreak,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(thematicBreak, phase)
    }

    private fun dispatch(
        list: List,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(list, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(list.items)
    }

    private fun dispatch(
        listItem: ListItem,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(listItem, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(listItem.content)
    }

    private fun dispatch(
        codeBlock: CodeBlock,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(codeBlock, phase)
    }

    private fun dispatch(
        htmlBlock: HTMLBlock,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(htmlBlock, phase)
    }

    private fun dispatch(
        formulaBlock: FormulaBlock,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(formulaBlock, phase)
    }

    private fun dispatch(
        table: Table,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(table, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(table.foot)
        schedule(table.content)
        schedule(table.head)
        table.caption?.let { schedule(it) }
    }

    private fun dispatch(
        tableCaption: TableCaption,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(tableCaption, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(tableCaption.content)
    }

    private fun dispatch(
        tableRow: TableRow,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(tableRow, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(tableRow.cells)
    }

    private fun dispatch(
        tableCell: TableCell,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(tableCell, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(tableCell.content)
    }

    private fun dispatch(
        directiveBlock: DirectiveBlock,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(directiveBlock, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(directiveBlock.content)
        directiveBlock.label?.let { schedule(it) }
    }

    private fun dispatch(
        directiveLabel: DirectiveLabel,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(directiveLabel, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(directiveLabel.content)
    }

    private fun dispatch(
        text: Text,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(text, phase)
    }

    private fun dispatch(
        softBreak: SoftBreak,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(softBreak, phase)
    }

    private fun dispatch(
        lineBreak: LineBreak,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(lineBreak, phase)
    }

    private fun dispatch(
        code: Code,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(code, phase)
    }

    private fun dispatch(
        html: HTML,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(html, phase)
    }

    private fun dispatch(
        comment: Comment,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(comment, phase)
    }

    private fun dispatch(
        crossLink: CrossLink,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(crossLink, phase)
    }

    private fun dispatch(
        crossEmbedded: CrossEmbedded,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(crossEmbedded, phase)
    }

    private fun dispatch(
        formula: Formula,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(formula, phase)
    }

    private fun dispatch(
        emphasis: Emphasis,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(emphasis, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(emphasis.content)
    }

    private fun dispatch(
        strong: Strong,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(strong, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(strong.content)
    }

    private fun dispatch(
        strikethrough: Strikethrough,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(strikethrough, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(strikethrough.content)
    }

    private fun dispatch(
        mark: Mark,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(mark, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(mark.content)
    }

    private fun dispatch(
        insertion: Insertion,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(insertion, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(insertion.content)
    }

    private fun dispatch(
        span: Span,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(span, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(span.content)
    }

    private fun dispatch(
        superscript: Superscript,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(superscript, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(superscript.content)
    }

    private fun dispatch(
        subscript: Subscript,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(subscript, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(subscript.content)
    }

    private fun dispatch(
        definitionList: DefinitionList,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(definitionList, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(definitionList.definitions)
    }

    private fun dispatch(
        definition: Definition,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(definition, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        for (body in definition.content.asReversed()) schedule(body)
        schedule(definition.term)
    }

    private fun dispatch(
        link: Link,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(link, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(link.content)
    }

    private fun dispatch(
        embedded: Embedded,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(embedded, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(embedded.content)
    }

    private fun dispatch(
        directive: Directive,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(directive, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        directive.label?.let { schedule(it) }
    }

    private fun dispatch(
        cite: Cite,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(cite, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(cite.citations)
    }

    private fun dispatch(
        citation: Citation,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(citation, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(citation.suffix)
        schedule(citation.prefix)
    }

    private fun dispatch(
        footnote: Footnote,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(footnote, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(footnote.content)
    }

    private fun dispatch(
        specimen: Specimen,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(specimen, phase)
        if (phase != MarkupVisitPhase.ENTER) return
        schedule(specimen.content)
    }

    private fun dispatch(
        metadata: Metadata,
        phase: MarkupVisitPhase,
    ) {
        visitor.visit(metadata, phase)
    }
}
